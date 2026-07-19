/**
 * spi_master.c - M4F SPI master link to the CC1310 (SPI slave). See spi_master.h
 * and docs/ARCHITECTURE-GEN2.md sec.3.
 *
 * MCSPI = MCU_SPI0, mode 1 (CPOL0/CPHA1) @ 1 MHz, INTERRUPT + CALLBACK (matches
 * the CC1310 gen1 slave: SPI_POL0_PHA1, 1 MHz). 2-line GPIO handshake:
 *   MASTER_READY (M4F out, active-low) - "master wants to send, slave arm".
 *   SLAVE_READY  (M4F in, active-low)  - "slave armed SPI_transfer(), clock now"
 *                                        (also = slave's request-to-send).
 *
 * SLAVE_READY is INTERRUPT-driven (with a poll backstop). The MCU_GPIO interrupt
 * for the M4F is routed GPIO bank 1 -> WKUP_MCU_GPIOMUX_INTROUTER0 -> OUTP -> NVIC
 * via Sciclient. Contrary to an earlier assumption, the Linux board config DOES
 * grant host M4_0 the introuter OUTP 4..7 (verified live by the IRQ sweep), so the
 * route succeeds. spi_setup_slave_ready_irq() claims the first granted OUTP, wires
 * a falling-edge bank ISR (slave pulls SLAVE_READY low = "armed / request"), and
 * the ISR posts gSrSem. The task blocks on gSrSem; a backstop timeout still samples
 * the level so the link survives even if an edge is ever missed. If NO OUTP is
 * granted, it falls back to pure polling (gIrqActive=false).
 *
 * Choreography (ported from gen1, roles reversed):
 *   A) Master sends (we have a NODE_CMD): assert MASTER_READY -> wait SLAVE_READY
 *      low -> MCSPI_transfer -> release MASTER_READY. Retry 3x / 300 ms.
 *   B) Slave sends (SLAVE_READY low unsolicited): MCSPI_transfer (TX=NOP) ->
 *      RX holds the slave frame -> route to engine.
 *
 * NOTE: not lint-able locally (TI SDK + FreeRTOS) -> build in CCS.
 */
#include <string.h>
#include <kernel/dpl/DebugP.h>
#include <kernel/dpl/SemaphoreP.h>
#include <kernel/dpl/AddrTranslateP.h>
#include <kernel/dpl/HwiP.h>
#include <drivers/gpio.h>
#include <drivers/mcspi.h>
#include <drivers/sciclient.h>
#include <drivers/hw_include/cslr_soc.h>
#include "ti_drivers_config.h"
#include "ti_drivers_open_close.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "protocol.h"      /* protocol_crc16 (CRC16-CCITT, init 0xFFFF, poly 0x1021) */
#include "spi_frame.h"
#include "spi_master.h"

/* ========================================================================== */
/*  syscfg-generated symbols (verified in ti_drivers_config.h):                */
/*  SPI_MASTER_READY = MCU_GPIO0 output on ball E5 / header pin 10.            */
/*    (A6/pin12 was abandoned - it would not drive high, ~0.4V unloaded.)      */
/*  SPI_SLAVE_READY  = MCU_GPIO0 pin 16 (D4, input, bank 1, FALL-edge IRQ)     */
/*  MCSPI CONFIG_MCSPI0 = MCU_SPI0, ch0 = CS1, CALLBACK, mode1, 1MHz.          */
/* ========================================================================== */
#define MR_BASE_CFG   (SPI_MASTER_READY_BASE_ADDR)
#define MR_PIN        (SPI_MASTER_READY_PIN)
#define SR_BASE_CFG   (SPI_SLAVE_READY_BASE_ADDR)
#define SR_PIN        (SPI_SLAVE_READY_PIN)

#define SPI_XFER_TIMEOUT_TICKS   pdMS_TO_TICKS(300u)
#define SPI_POLL_MS              2u      /* SLAVE_READY sample period in POLL fallback */
#define SPI_IRQ_BACKSTOP_MS      50u     /* task wait timeout in IRQ mode (safety net) */
#define SPI_ARM_TIMEOUT_MS       500u    /* max wait for slave to arm (scenario A); > slave hold */
#define SPI_MAX_RETRIES          3
#define SPI_OUT_DEPTH            8u
#define SPI_TASK_STACK_WORDS     2048u   /* bumped from 1024 - NodeFrame (52B) enlarged the SPI frame locals + queue items */
#define SPI_TASK_PRIORITY        2u

/* SLAVE_READY GPIO-IRQ routing (WKUP_MCU_GPIOMUX_INTROUTER0). M4_0 owns OUTP 4..7;
 * NVIC intrNum for OUTP n = (n - 4) + 16. src_index = MCU_GPIO0 bank of SR_PIN. */
#define SR_INTROUTER_ID    (TISCI_DEV_WKUP_MCU_GPIOMUX_INTROUTER0)
#define SR_OUTP_FIRST      (4u)
#define SR_OUTP_LAST       (7u)

/* ========================================================================== */
/*  State                                                                      */
/* ========================================================================== */
extern volatile uint8_t gbShutdown;            /* set by Linux shutdown (ipc_rpmsg_echo.c) */

static QueueHandle_t  gNodeInQueue;            /* engine input (we produce), NodeFrame */

static uint8_t        gSpiOutStorage[SPI_OUT_DEPTH * sizeof(NodeFrame)];
static StaticQueue_t  gSpiOutQueueObj;
static QueueHandle_t  gSpiOutQueue;            /* node commands (we consume), NodeFrame */

static StackType_t    gSpiStack[SPI_TASK_STACK_WORDS];
static StaticTask_t   gSpiTaskObj;
static TaskHandle_t   gSpiTask;

static SemaphoreP_Object gXferDoneSem;         /* posted by MCSPI callback (ISR) */
static SemaphoreP_Object gSrSem;               /* posted by SLAVE_READY ISR + post_cmd */
static HwiP_Object        gSrHwi;              /* SLAVE_READY bank interrupt */
static bool           gIrqActive = false;      /* true once the GPIO-IRQ is wired */
static int32_t        gIrqOutp = -1;           /* introuter OUTP claimed for SR */
static volatile uint32_t gSrIrqCount = 0;      /* SLAVE_READY edges seen (diagnostic) */
static volatile bool  gSpiStopped = false;     /* set after the task tears down + exits */

static uint32_t       gMrBase, gSrBase;        /* AddrTranslate'd GPIO bases */
static uint8_t        gTxSeq = 1;

static SpiFrame       gTxFrame;
static SpiFrame       gRxFrame;

/* ========================================================================== */
/*  Frame helpers (CRC over bytes [0..125], big-endian in crc_be)              */
/* ========================================================================== */
static void frame_finalize(SpiFrame *f)
{
    uint16_t c;
    f->magic = SPI_FRAME_MAGIC;
    c = protocol_crc16((const uint8_t *)f, SPI_FRAME_CRC_OFFSET);
    f->crc_be[0] = (uint8_t)(c >> 8);
    f->crc_be[1] = (uint8_t)(c & 0xFFu);
}

static bool frame_valid(const SpiFrame *f)
{
    uint16_t c;
    if (f->magic != SPI_FRAME_MAGIC) {
        return false;
    }
    c = protocol_crc16((const uint8_t *)f, SPI_FRAME_CRC_OFFSET);
    return (f->crc_be[0] == (uint8_t)(c >> 8)) &&
           (f->crc_be[1] == (uint8_t)(c & 0xFFu));
}

static void frame_make_nop(SpiFrame *f)
{
    memset(f, 0, sizeof(*f));
    f->type = SPI_FRAME_NOP;
    frame_finalize(f);
}

static void frame_make_node_cmd(SpiFrame *f, const NodeFrame *nf,
                                uint8_t seq, uint8_t pending)
{
    uint8_t len = (uint8_t)sizeof(NodeFrame);
    memset(f, 0, sizeof(*f));
    f->type    = SPI_FRAME_NODE_CMD;
    f->seq     = seq;
    f->pending = pending;
    if (len > SPI_FRAME_PAYLOAD_MAX) {
        len = SPI_FRAME_PAYLOAD_MAX;
    }
    f->len = len;
    memcpy(f->payload, nf, len);   /* factory_id + MessageStruct */
    frame_finalize(f);
}

static void route_rx_frame(void)
{
    if (!frame_valid(&gRxFrame)) {
        DebugP_log("[SPI] RX frame invalid (magic/CRC)\r\n");
        return;
    }
    if (gRxFrame.type == SPI_FRAME_NODE_DATA) {
        NodeFrame nf;
        memset(&nf, 0, sizeof(nf));
        if (gRxFrame.len >= sizeof(NodeFrame)) {
            memcpy(&nf, gRxFrame.payload, sizeof(NodeFrame));            /* v2: factory_id + msg */
        } else if (gRxFrame.len >= sizeof(MessageStruct)) {
            memcpy(&nf.msg, gRxFrame.payload, sizeof(MessageStruct));    /* legacy: factory_id 0 */
        } else {
            return;
        }
        if (xQueueSend(gNodeInQueue, &nf, 0) != pdTRUE) {
            DebugP_log("[SPI] nodeIn full - node data dropped\r\n");
        } else {
            DebugP_log("[SPI] RX node data -> engine (type=%u cmd=%u)\r\n",
                        (unsigned)nf.msg.type, (unsigned)nf.msg.cmd);
        }
    }
    /* FRAME_NOP / FRAME_ACK: nothing to route. */
}

/* ========================================================================== */
/*  MCSPI completion callback (CALLBACK mode, ISR context)                      */
/* ========================================================================== */
void spiMcspiCallback(MCSPI_Handle handle, MCSPI_Transaction *transaction)
{
    (void)handle;
    (void)transaction;
    SemaphoreP_post(&gXferDoneSem);   /* ISR-safe */
}

/* ========================================================================== */
/*  Handshake + transfer primitives                                            */
/* ========================================================================== */
static inline void mr_assert(void)  { GPIO_pinWriteLow(gMrBase, MR_PIN); }   /* request */
static inline void mr_release(void) { GPIO_pinWriteHigh(gMrBase, MR_PIN); }  /* idle    */
static inline bool sr_is_low(void)  { return (GPIO_pinRead(gSrBase, SR_PIN) == 0u); }

/* One 128-byte transfer (gTxFrame -> gRxFrame), non-blocking start + sem wait. */
static bool spi_do_transfer(void)
{
    MCSPI_Transaction t;
    int32_t status;

    MCSPI_Transaction_init(&t);
    t.channel   = gConfigMcspi0ChCfg[0].chNum;
    t.dataSize  = 8;
    t.csDisable = TRUE;
    t.count     = SPI_FRAME_SIZE;
    t.txBuf     = (void *)&gTxFrame;
    t.rxBuf     = (void *)&gRxFrame;
    t.args      = NULL;

    status = MCSPI_transfer(gMcspiHandle[CONFIG_MCSPI0], &t);
    if (status != SystemP_SUCCESS) {
        DebugP_log("[SPI] MCSPI_transfer start failed: %d\r\n", (int)status);
        return false;
    }
    if (SemaphoreP_pend(&gXferDoneSem, SPI_XFER_TIMEOUT_TICKS) != SystemP_SUCCESS) {
        DebugP_log("[SPI] transfer timeout\r\n");
        return false;
    }
    return (t.status == MCSPI_TRANSFER_COMPLETED);
}

/* Scenario B: slave initiated. Clock a NOP, receive the slave frame. */
static void spi_rx_from_slave(void)
{
    frame_make_nop(&gTxFrame);
    memset(&gRxFrame, 0, sizeof(gRxFrame));
    if (spi_do_transfer()) {
        route_rx_frame();
    }
}

/* Clock the prepared gTxFrame now that the slave is armed; release MASTER_READY. */
static bool spi_tx_clock_now(void)
{
    bool ok;
    memset(&gRxFrame, 0, sizeof(gRxFrame));
    ok = spi_do_transfer();
    mr_release();
    if (ok) {
        route_rx_frame();   /* slave may piggyback data in its RX half */
        return true;
    }
    return false;
}

/* Scenario A: master initiated. Assert MASTER_READY ONCE and wait for the slave to
 * arm (SLAVE_READY low), then clock. Do NOT re-assert per-retry: that cycled a
 * master window against the slave's hold window and they drifted out of overlap
 * (livelock). gen1's master used the SLAVE_READY edge IRQ and clocked instantly.
 *
 * IRQ mode: clear gSrSem, assert MR, then block on gSrSem until the falling edge
 * (the slave arming) arrives - zero CPU spent waiting. A final level re-check
 * tolerates a missed edge. POLL mode: sample the level every SPI_POLL_MS. */
static bool spi_tx_cmd(const NodeFrame *nf, uint8_t seq, uint8_t pending)
{
    frame_make_node_cmd(&gTxFrame, nf, seq, pending);

    if (gIrqActive) {
        while (SemaphoreP_pend(&gSrSem, 0u) == SystemP_SUCCESS) { }   /* drain stale */
        mr_assert();
        if (sr_is_low()) {                       /* slave already armed (no new edge) */
            return spi_tx_clock_now();
        }
        (void)SemaphoreP_pend(&gSrSem, pdMS_TO_TICKS(SPI_ARM_TIMEOUT_MS));
        if (sr_is_low()) {
            return spi_tx_clock_now();
        }
        mr_release();
        DebugP_log("[SPI] tx: slave not ready (irq timeout)\r\n");
        return false;
    }

    /* POLL fallback. */
    {
        uint32_t waited;
        mr_assert();
        for (waited = 0u; waited < SPI_ARM_TIMEOUT_MS; waited += SPI_POLL_MS) {
            if (sr_is_low()) {
                return spi_tx_clock_now();
            }
            vTaskDelay(pdMS_TO_TICKS(SPI_POLL_MS));
        }
        mr_release();
        DebugP_log("[SPI] tx: slave not ready (poll timeout)\r\n");
        return false;
    }
}

/* ========================================================================== */
/*  Task                                                                       */
/* ========================================================================== */
static int32_t spi_irq_route_release(uint32_t outp);   /* defined in IRQ section */

static void spiTask(void *args)
{
    (void)args;
    DebugP_log("[SPI] master task started (SLAVE_READY mode = %s, OUTP=%d)\r\n",
                gIrqActive ? "IRQ" : "POLL", (int)gIrqOutp);

    while (!gbShutdown) {
        NodeFrame cmd;
        uint32_t waitMs = gIrqActive ? SPI_IRQ_BACKSTOP_MS : SPI_POLL_MS;

        /* Wake on: outbound command posted (post_cmd posts gSrSem), a SLAVE_READY
         * falling edge (ISR posts gSrSem), or the backstop timeout (level resample). */
        (void)SemaphoreP_pend(&gSrSem, pdMS_TO_TICKS(waitMs));

        /* 1) Master-initiated: drain outbound commands. */
        while (xQueueReceive(gSpiOutQueue, &cmd, 0) == pdTRUE) {
            uint8_t pending = (uint8_t)uxQueueMessagesWaiting(gSpiOutQueue);
            if (!spi_tx_cmd(&cmd, gTxSeq++, pending)) {
                DebugP_log("[SPI] cmd send GIVEUP (node type=%u)\r\n",
                            (unsigned)cmd.msg.type);
            }
        }

        /* 2) Slave-initiated: SLAVE_READY pulled low without us asserting. */
        if (sr_is_low()) {
            spi_rx_from_slave();
        }
    }

    /* Shutdown: tear down the GPIO-IRQ (disable bank intr, drop the edge trigger,
     * destruct the Hwi, release the introuter route via DMSC) BEFORE doShutdown()
     * closes drivers - a live Hwi / held route during Drivers_close + System_deinit
     * can wedge the remoteproc stop. mr_release() first so we stop driving the line. */
    mr_release();
    if (gIrqActive) {
        GPIO_bankIntrDisable(gSrBase, GPIO_GET_BANK_INDEX(SR_PIN));
        GPIO_setTrigType(gSrBase, SR_PIN, GPIO_TRIG_TYPE_NONE);
        HwiP_destruct(&gSrHwi);
        if (gIrqOutp >= 0) {
            (void)spi_irq_route_release((uint32_t)gIrqOutp);
        }
        gIrqActive = false;
    }
    gSpiStopped = true;   /* tell doShutdown() the IRQ is fully torn down */
    vTaskDelete(NULL);
}

/* ========================================================================== */
/*  SLAVE_READY GPIO interrupt (Sciclient introuter route + bank ISR)           */
/*                                                                              */
/*  The MCU_GPIO0 bank interrupt for the M4F is routed via                      */
/*  WKUP_MCU_GPIOMUX_INTROUTER0 to an OUTP wired to the M4F NVIC. Host M4_0     */
/*  owns OUTP 4..7 (verified live: the IRQ sweep returned r=0 for all four).    */
/*  src_index = the introuter input for SLAVE_READY's GPIO bank.                */
/*  NVIC intrNum for OUTP n = (n - 4) + 16  (CSLR ..._OUTP_4 == 0).             */
/* ========================================================================== */
#define SR_INTROUTER_IN \
    ((uint16_t)(CSLR_WKUP_MCU_GPIOMUX_INTROUTER0_IN_MCU_GPIO0_GPIO_BANK_0 + \
                GPIO_GET_BANK_INDEX(SR_PIN)))

static int32_t spi_irq_route_set(uint32_t outp)
{
    struct tisci_msg_rm_irq_set_req  req;
    struct tisci_msg_rm_irq_set_resp resp;

    memset(&req, 0, sizeof(req));
    req.valid_params  = TISCI_MSG_VALUE_RM_DST_ID_VALID |
                        TISCI_MSG_VALUE_RM_DST_HOST_IRQ_VALID;
    req.src_id        = SR_INTROUTER_ID;
    req.src_index     = SR_INTROUTER_IN;
    req.dst_id        = SR_INTROUTER_ID;
    req.dst_host_irq  = (uint16_t)outp;
    req.secondary_host = TISCI_MSG_VALUE_RM_UNUSED_SECONDARY_HOST;

    return Sciclient_rmIrqSetRaw(&req, &resp, SystemP_WAIT_FOREVER);
}

static int32_t spi_irq_route_release(uint32_t outp)
{
    struct tisci_msg_rm_irq_release_req req;

    memset(&req, 0, sizeof(req));
    req.valid_params  = TISCI_MSG_VALUE_RM_DST_ID_VALID |
                        TISCI_MSG_VALUE_RM_DST_HOST_IRQ_VALID;
    req.src_id        = SR_INTROUTER_ID;
    req.src_index     = SR_INTROUTER_IN;
    req.dst_id        = SR_INTROUTER_ID;
    req.dst_host_irq  = (uint16_t)outp;
    req.secondary_host = TISCI_MSG_VALUE_RM_UNUSED_SECONDARY_HOST;

    return Sciclient_rmIrqReleaseRaw(&req, SystemP_WAIT_FOREVER);
}

/* Bank ISR: slave pulled SLAVE_READY low (falling edge). Clear bank status, signal. */
static void sr_bank_isr(void *args)
{
    uint32_t bankNum = GPIO_GET_BANK_INDEX(SR_PIN);
    uint32_t pinMask = GPIO_GET_BANK_BIT_MASK(SR_PIN);
    uint32_t st;
    (void)args;

    st = GPIO_getBankIntrStatus(gSrBase, bankNum);
    GPIO_clearBankIntrStatus(gSrBase, bankNum, st);
    if (st & pinMask) {
        gSrIrqCount++;
        SemaphoreP_post(&gSrSem);   /* ISR-safe */
    }
}

/* Claim the first granted introuter OUTP (4..7) and wire the falling-edge bank ISR
 * on SLAVE_READY. Sets gIrqActive; on any failure falls back to pure polling. */
static void spi_setup_slave_ready_irq(void)
{
    uint32_t bankNum = GPIO_GET_BANK_INDEX(SR_PIN);
    HwiP_Params hwiPrms;
    int32_t routed = -1;
    uint32_t outp;
    int32_t  r;

    for (outp = SR_OUTP_FIRST; outp <= SR_OUTP_LAST; outp++) {
        if (spi_irq_route_set(outp) == 0) {
            routed = (int32_t)outp;
            break;
        }
    }
    if (routed < 0) {
        DebugP_log("[SPI][IRQ] no introuter OUTP granted to M4_0 -> POLL fallback\r\n");
        gIrqActive = false;
        return;
    }

    GPIO_setTrigType(gSrBase, SR_PIN, GPIO_TRIG_TYPE_FALL_EDGE);
    GPIO_clearBankIntrStatus(gSrBase, bankNum, GPIO_getBankIntrStatus(gSrBase, bankNum));
    GPIO_bankIntrEnable(gSrBase, bankNum);

    HwiP_Params_init(&hwiPrms);
    hwiPrms.intNum   = (uint32_t)((routed - (int32_t)SR_OUTP_FIRST) + 16);
    hwiPrms.callback = &sr_bank_isr;
    hwiPrms.isPulse  = 1;
    hwiPrms.args     = NULL;
    r = HwiP_construct(&gSrHwi, &hwiPrms);
    if (r != SystemP_SUCCESS) {
        DebugP_log("[SPI][IRQ] HwiP_construct failed (%d) -> POLL fallback\r\n", (int)r);
        GPIO_bankIntrDisable(gSrBase, bankNum);
        GPIO_setTrigType(gSrBase, SR_PIN, GPIO_TRIG_TYPE_NONE);
        (void)spi_irq_route_release((uint32_t)routed);
        gIrqActive = false;
        return;
    }

    gIrqOutp   = routed;
    gIrqActive = true;
    DebugP_log("[SPI][IRQ] SLAVE_READY IRQ active: OUTP %d, intrNum %u, bank %u (FALL edge)\r\n",
               (int)routed, (unsigned)hwiPrms.intNum, (unsigned)bankNum);
}

/* ========================================================================== */
/*  Public API                                                                 */
/* ========================================================================== */
uint32_t spi_master_stack_hwm(void)
{
    /* Free words still holding the 0xA5 fill from the deep end (Cortex-M stack grows
     * down -> headroom sits at index 0+). Self-scan: the SDK FreeRTOS config has
     * INCLUDE_uxTaskGetStackHighWaterMark=0. */
    uint32_t freeWords = 0u;
    while (freeWords < SPI_TASK_STACK_WORDS &&
           gSpiStack[freeWords] == (StackType_t)0xA5A5A5A5u) {
        freeWords++;
    }
    return freeWords;
}

bool spi_master_post_cmd(const NodeFrame *frame)
{
    if (frame == NULL || gSpiOutQueue == NULL) {
        return false;
    }
    if (xQueueSend(gSpiOutQueue, frame, 0) != pdTRUE) {
        return false;
    }
    SemaphoreP_post(&gSrSem);   /* wake the SPI task to drain the queue */
    return true;
}

void spi_master_shutdown(uint32_t timeoutMs)
{
    uint32_t waited = 0u;

    if (gSpiTask == NULL || gSpiStopped) {
        return;
    }
    /* Wake the task now (it would otherwise wait up to the backstop) so it sees
     * gbShutdown, tears down the IRQ, and sets gSpiStopped. */
    SemaphoreP_post(&gSrSem);

    while (!gSpiStopped && waited < timeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(10u));
        waited += 10u;
    }
    if (!gSpiStopped) {
        DebugP_log("[SPI] shutdown: task did not stop in %ums (IRQ teardown may be incomplete)\r\n",
                   (unsigned)timeoutMs);
    } else {
        DebugP_log("[SPI] shutdown: task stopped, IRQ torn down (%ums)\r\n", (unsigned)waited);
    }
}

void spi_master_init(QueueHandle_t nodeInQueue)
{
    DebugP_log("[SPI] init begin\r\n");
    gNodeInQueue = nodeInQueue;

    gSpiOutQueue = xQueueCreateStatic(SPI_OUT_DEPTH, sizeof(NodeFrame),
                                      gSpiOutStorage, &gSpiOutQueueObj);
    DebugP_assert(gSpiOutQueue != NULL);
    (void)SemaphoreP_constructBinary(&gXferDoneSem, 0);
    (void)SemaphoreP_constructBinary(&gSrSem, 0);

    /* GPIO bases need address translation for M4F access. */
    gMrBase = (uint32_t)AddrTranslateP_getLocalAddr(MR_BASE_CFG);
    gSrBase = (uint32_t)AddrTranslateP_getLocalAddr(SR_BASE_CFG);

    /* MASTER_READY: output, idle high (deasserted). */
    GPIO_setDirMode(gMrBase, MR_PIN, GPIO_DIRECTION_OUTPUT);
    mr_release();

    /* SLAVE_READY: input. Try to wire its falling-edge interrupt (poll fallback). */
    GPIO_setDirMode(gSrBase, SR_PIN, GPIO_DIRECTION_INPUT);
    spi_setup_slave_ready_irq();

    memset(gSpiStack, 0xA5, sizeof(gSpiStack));   /* fill for the stack-HWM diagnostic */
    gSpiTask = xTaskCreateStatic(spiTask, "spi_master", SPI_TASK_STACK_WORDS,
                                 NULL, SPI_TASK_PRIORITY, gSpiStack, &gSpiTaskObj);
    DebugP_assert(gSpiTask != NULL);

    DebugP_log("[SPI] master init done (MCU_SPI0, mode1, 1MHz, CALLBACK, %s)\r\n",
                gIrqActive ? "IRQ" : "poll");
}
