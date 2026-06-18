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
 * SLAVE_READY is POLLED, not interrupt-driven: on AM62 an MCU_GPIO interrupt for
 * the M4F must be routed through the WKUP GPIOMUX introuter via DMSC, and when
 * the M4F runs alongside Linux that resource is NOT granted to the M4F (Sciclient
 * returns -1). So the SPI task polls the SLAVE_READY level every SPI_POLL_MS.
 * NOTE: only the handshake line is polled (a cheap GPIO read, task sleeps between
 * polls) - the 128-byte data transfer itself is still INTERRUPT + CALLBACK, so no
 * CPU is burned shifting bytes.
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
#include <drivers/gpio.h>
#include <drivers/mcspi.h>
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
/*  SPI_SLAVE_READY  = MCU_GPIO0 pin 8 (B6, input)                             */
/*  MCSPI CONFIG_MCSPI0 = MCU_SPI0, ch0 = CS1, CALLBACK, mode1, 1MHz.          */
/* ========================================================================== */
#define MR_BASE_CFG   (SPI_MASTER_READY_BASE_ADDR)
#define MR_PIN        (SPI_MASTER_READY_PIN)
#define SR_BASE_CFG   (SPI_SLAVE_READY_BASE_ADDR)
#define SR_PIN        (SPI_SLAVE_READY_PIN)

#define SPI_XFER_TIMEOUT_TICKS   pdMS_TO_TICKS(300u)
#define SPI_POLL_MS              2u      /* SLAVE_READY poll interval */
#define SPI_ARM_TIMEOUT_MS       500u    /* max wait for slave to arm (scenario A); > slave hold */
#define SPI_MAX_RETRIES          3
#define SPI_OUT_DEPTH            8u
#define SPI_TASK_STACK_WORDS     1024u
#define SPI_TASK_PRIORITY        2u

/* ========================================================================== */
/*  State                                                                      */
/* ========================================================================== */
extern volatile uint8_t gbShutdown;            /* set by Linux shutdown (ipc_rpmsg_echo.c) */

static QueueHandle_t  gNodeInQueue;            /* engine input (we produce) */

static uint8_t        gSpiOutStorage[SPI_OUT_DEPTH * sizeof(MessageStruct)];
static StaticQueue_t  gSpiOutQueueObj;
static QueueHandle_t  gSpiOutQueue;            /* node commands (we consume) */

static StackType_t    gSpiStack[SPI_TASK_STACK_WORDS];
static StaticTask_t   gSpiTaskObj;
static TaskHandle_t   gSpiTask;

static SemaphoreP_Object gXferDoneSem;         /* posted by MCSPI callback (ISR) */

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

static void frame_make_node_cmd(SpiFrame *f, const MessageStruct *m,
                                uint8_t seq, uint8_t pending)
{
    uint8_t len = (uint8_t)sizeof(MessageStruct);
    memset(f, 0, sizeof(*f));
    f->type    = SPI_FRAME_NODE_CMD;
    f->seq     = seq;
    f->pending = pending;
    if (len > SPI_FRAME_PAYLOAD_MAX) {
        len = SPI_FRAME_PAYLOAD_MAX;
    }
    f->len = len;
    memcpy(f->payload, m, len);
    frame_finalize(f);
}

static void route_rx_frame(void)
{
    if (!frame_valid(&gRxFrame)) {
        DebugP_log("[SPI] RX frame invalid (magic/CRC)\r\n");
        return;
    }
    if (gRxFrame.type == SPI_FRAME_NODE_DATA &&
        gRxFrame.len >= sizeof(MessageStruct)) {
        MessageStruct m;
        memcpy(&m, gRxFrame.payload, sizeof(MessageStruct));
        if (xQueueSend(gNodeInQueue, &m, 0) != pdTRUE) {
            DebugP_log("[SPI] nodeIn full - node data dropped\r\n");
        } else {
            DebugP_log("[SPI] RX node data -> engine (type=%u cmd=%u)\r\n",
                        (unsigned)m.type, (unsigned)m.cmd);
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

/* Scenario A: master initiated. Assert MASTER_READY, poll-wait for the slave to
 * arm (SLAVE_READY low), clock the command. Retry up to SPI_MAX_RETRIES. */
static bool spi_tx_cmd(const MessageStruct *m, uint8_t seq, uint8_t pending)
{
    uint32_t waited;
    frame_make_node_cmd(&gTxFrame, m, seq, pending);

    /* Assert MASTER_READY ONCE and poll SLAVE_READY continuously until the slave
     * arms (pulls it low), then clock. Do NOT re-assert per-retry: that cycled a
     * 300ms master window against the slave's 300ms hold window and they drifted
     * permanently out of overlap (livelock). gen1's master used the SLAVE_READY
     * edge IRQ and clocked instantly; we poll, so we hold MASTER_READY and wait
     * in a single window - the slave reacts to the one edge within ms. */
    mr_assert();
    for (waited = 0u; waited < SPI_ARM_TIMEOUT_MS; waited += SPI_POLL_MS) {
        if (sr_is_low()) {
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
        vTaskDelay(pdMS_TO_TICKS(SPI_POLL_MS));
    }
    mr_release();
    DebugP_log("[SPI] tx: slave not ready (timeout)\r\n");
    return false;
}

/* ========================================================================== */
/*  Task                                                                       */
/* ========================================================================== */
static void spiTask(void *args)
{
    (void)args;
    DebugP_log("[SPI] master task started (poll SLAVE_READY @ %ums)\r\n",
                (unsigned)SPI_POLL_MS);

    while (!gbShutdown) {
        uint32_t bits = 0;
        MessageStruct cmd;

        /* Wake on a posted outbound command, or after the poll interval to
         * sample SLAVE_READY for a slave-initiated transfer. */
        (void)xTaskNotifyWait(0u, 0xFFFFFFFFu, &bits, pdMS_TO_TICKS(SPI_POLL_MS));

        /* 1) Master-initiated: drain outbound commands. */
        while (xQueueReceive(gSpiOutQueue, &cmd, 0) == pdTRUE) {
            uint8_t pending = (uint8_t)uxQueueMessagesWaiting(gSpiOutQueue);
            if (!spi_tx_cmd(&cmd, gTxSeq++, pending)) {
                DebugP_log("[SPI] cmd send GIVEUP (node type=%u)\r\n",
                            (unsigned)cmd.type);
            }
        }

        /* 2) Slave-initiated: SLAVE_READY pulled low without us asserting. */
        if (sr_is_low()) {
            spi_rx_from_slave();
        }
    }

    /* Shutdown: stop touching MCSPI before doShutdown() closes drivers. */
    mr_release();
    vTaskDelete(NULL);
}

/* ========================================================================== */
/*  Public API                                                                 */
/* ========================================================================== */
bool spi_master_post_cmd(const MessageStruct *msg)
{
    if (msg == NULL || gSpiOutQueue == NULL) {
        return false;
    }
    if (xQueueSend(gSpiOutQueue, msg, 0) != pdTRUE) {
        return false;
    }
    if (gSpiTask != NULL) {
        xTaskNotify(gSpiTask, 0u, eNoAction);   /* wake to process the queue */
    }
    return true;
}

void spi_master_init(QueueHandle_t nodeInQueue)
{
    DebugP_log("[SPI] init begin\r\n");
    gNodeInQueue = nodeInQueue;

    gSpiOutQueue = xQueueCreateStatic(SPI_OUT_DEPTH, sizeof(MessageStruct),
                                      gSpiOutStorage, &gSpiOutQueueObj);
    DebugP_assert(gSpiOutQueue != NULL);
    (void)SemaphoreP_constructBinary(&gXferDoneSem, 0);

    /* GPIO bases need address translation for M4F access. */
    gMrBase = (uint32_t)AddrTranslateP_getLocalAddr(MR_BASE_CFG);
    gSrBase = (uint32_t)AddrTranslateP_getLocalAddr(SR_BASE_CFG);

    /* MASTER_READY: output, idle high (deasserted). */
    GPIO_setDirMode(gMrBase, MR_PIN, GPIO_DIRECTION_OUTPUT);
    mr_release();

    /* SLAVE_READY: input (polled - no interrupt; see file header). */
    GPIO_setDirMode(gSrBase, SR_PIN, GPIO_DIRECTION_INPUT);

    gSpiTask = xTaskCreateStatic(spiTask, "spi_master", SPI_TASK_STACK_WORDS,
                                 NULL, SPI_TASK_PRIORITY, gSpiStack, &gSpiTaskObj);
    DebugP_assert(gSpiTask != NULL);

    DebugP_log("[SPI] master init done (MCU_SPI0, mode1, 1MHz, CALLBACK, poll)\r\n");
}
