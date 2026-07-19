/**
 * spi_slave_task.c - CC1310 SPI SLAVE to the M4F (SPI master). gen2.
 *
 * Replaces the gen1 CC1310 spi_master_task.c (role reversed: CC1310 is now the
 * SPI slave). Ported from the gen1 CC3235 slave (spiTask.c): SPI_PERIPHERAL,
 * SPI_POL0_PHA1 (mode 1), CALLBACK mode. Carries 128-byte frames (spi_frame.h).
 *
 * 2-line handshake (active-low):
 *   MASTER_READY (M4F out -> CC1310 in, IRQ falling) "master wants to send".
 *   SLAVE_READY  (CC1310 out -> M4F in)              "armed SPI_transfer(),
 *                                                     clock now" / request-to-send.
 *   Slave ALWAYS arms SPI_transfer() first, THEN pulls SLAVE_READY low; the
 *   master clocks on that. (TI requirement - the peripheral must be armed before
 *   the master drives the clock.)
 *
 * Integration with radio_task.c (unchanged):
 *   M4F -> node: RX FRAME_NODE_CMD -> radioQueue (RadioMessageStruct) -> RF.
 *   node -> M4F: radio_task posts MessageStruct to spiQueue + SEND_DATA_TO_SLAVE
 *                -> we frame it as FRAME_NODE_DATA and send up to the M4F.
 *
 * Build: TI-RTOS (SYS/BIOS) on CC1310. Copied into the CCS project alongside
 * radio_task.c (see bramka-setup/cc1310-firmware).
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <mqueue.h>

#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Event.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/BIOS.h>
#include <ti/drivers/GPIO.h>
#include <ti/drivers/SPI.h>
#include <ti/display/Display.h>

#include "Board.h"
#include "spi_frame.h"

/* ---- MessageStruct / RadioMessageStruct: MUST match radio_task.c byte-for-byte
 * (and the M4F shared/node_protocol.h, 44 bytes). ---- */
typedef struct {
    uint8_t id;
    uint8_t type;
    uint8_t cmd;
    uint8_t length;
    union {
        struct {
            float Tin; float Tout; float T4; float T3; float T2; float T1; float Tcol;
            int energyGain; int flowRate; uint8_t pumpState;
        } solarData;
        struct { uint8_t pumpState; } pumpData;
        struct { float sBuforTemp; } buforData;
        struct { char text[30]; } textData;
    } payload;
} MessageStruct;

/* NodeFrame: chip identity + message. Mirrors Shared/Protocol/node_protocol.h
 * (factory_id[8] + MessageStruct[44] = 52 B) so the SPI payload and the M4F/Go
 * side agree byte-for-byte. factory_id all-zero = legacy/unknown ('D' RF frame). */
typedef struct {
    uint8_t       factory_id[8];
    MessageStruct message;
} NodeFrame;

typedef struct {
    NodeFrame frame;
    uint8_t   retryCounter;
} RadioMessageStruct;

/* ---- Task / event config ---- */
#define SPI_SLAVE_TASK_STACK   2048
#define SPI_SLAVE_TASK_PRIORITY 2
#define SPI_QUEUE_NAME         "/spiQueue"
#define SPI_QUEUE_MAX_MSG      sizeof(NodeFrame)

#define EVENT_SPI_DONE             (1 << 1)   /* SPI transfer callback */
#define EVENT_MASTER_INITIATE      (1 << 2)   /* MASTER_READY falling edge */
#define SEND_DATA_TO_SLAVE         (1 << 3)   /* radio -> node data to send up */

#define SPI_TIMEOUT_TICKS   (300 * 1000 / Clock_tickPeriod)   /* 300 ms */
#define MAX_RETRIES         3

/* radio_task.c provides these; it posts node->gateway data here for us to ship up. */
extern mqd_t        radioQueue;
extern Event_Handle radioEventHandle;
#define EVENT_SEND_PACKET   (1 << 0)          /* radio_task's "send to node" bit */

/* We own these; radio_task.c externs them. */
mqd_t        spiQueue;
Event_Handle spiMasterEventHandle;

static Event_Struct spiEventStruct;
static Task_Struct  spiTaskStruct;
static uint8_t      spiTaskStack[SPI_SLAVE_TASK_STACK];
extern Display_Handle display;   /* global, defined with the concentrator (as in radio_task.c) */

static SPI_Handle   spiHandle;
static SpiFrame     txFrame;
static SpiFrame     rxFrame;
static uint8_t      txSeq = 1;

/* ---- CRC16-CCITT (init 0xFFFF, poly 0x1021, no reflect) - identical to the M4F
 * protocol_crc16(), so frame CRCs match. ---- */
static uint16_t spi_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    size_t i; int b;
    for (i = 0; i < len; i++) {
        crc ^= ((uint16_t)data[i]) << 8;
        for (b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void frame_finalize(SpiFrame *f)
{
    uint16_t c;
    f->magic = SPI_FRAME_MAGIC;
    c = spi_crc16((const uint8_t *)f, SPI_FRAME_CRC_OFFSET);
    f->crc_be[0] = (uint8_t)(c >> 8);
    f->crc_be[1] = (uint8_t)(c & 0xFFu);
}

static bool frame_valid(const SpiFrame *f)
{
    uint16_t c;
    if (f->magic != SPI_FRAME_MAGIC) return false;
    c = spi_crc16((const uint8_t *)f, SPI_FRAME_CRC_OFFSET);
    return (f->crc_be[0] == (uint8_t)(c >> 8)) &&
           (f->crc_be[1] == (uint8_t)(c & 0xFFu));
}

static void frame_make_nop(SpiFrame *f)
{
    memset(f, 0, sizeof(*f));
    f->type = SPI_FRAME_NOP;
    frame_finalize(f);
}

static void frame_make_node_data(SpiFrame *f, const NodeFrame *nf,
                                 uint8_t seq, uint8_t pending)
{
    uint8_t len = (uint8_t)sizeof(NodeFrame);
    memset(f, 0, sizeof(*f));
    f->type = SPI_FRAME_NODE_DATA;
    f->seq = seq;
    f->pending = pending;
    if (len > SPI_FRAME_PAYLOAD_MAX) len = SPI_FRAME_PAYLOAD_MAX;
    f->len = len;
    memcpy(f->payload, nf, len);   /* factory_id + MessageStruct */
    frame_finalize(f);
}

/* Forward a received M4F command frame to the radio (RF to node). The SPI payload
 * is a NodeFrame (52 B); tolerate a legacy 44 B MessageStruct (factory_id 0). */
static void route_rx_frame(void)
{
    if (!frame_valid(&rxFrame)) {
        if (display) Display_printf(display, 0, 0, "[SPI Slave] RX frame invalid");
        return;
    }
    if (rxFrame.type == SPI_FRAME_NODE_CMD) {
        RadioMessageStruct job;
        memset(&job, 0, sizeof(job));
        if (rxFrame.len >= sizeof(NodeFrame)) {
            memcpy(&job.frame, rxFrame.payload, sizeof(NodeFrame));
        } else if (rxFrame.len >= sizeof(MessageStruct)) {
            memcpy(&job.frame.message, rxFrame.payload, sizeof(MessageStruct)); /* legacy */
        } else {
            return;
        }
        job.retryCounter = 0;
        if (mq_send(radioQueue, (char *)&job, sizeof(job), 0) == 0) {
            Event_post(radioEventHandle, EVENT_SEND_PACKET);
            if (display) Display_printf(display, 0, 0,
                "[SPI Slave] RX cmd -> radio (type=%d cmd=%d)",
                job.frame.message.type, job.frame.message.cmd);
        } else if (display) {
            Display_printf(display, 0, 0, "[SPI Slave] radioQueue full - cmd dropped");
        }
    }
    /* FRAME_NOP / FRAME_ACK: nothing to forward. */
}

/* ---- ISR / callbacks ---- */
static void masterReadyFxn(uint_least8_t index)
{
    (void)index;
    Event_post(spiMasterEventHandle, EVENT_MASTER_INITIATE);
}

static void spiTransferCallback(SPI_Handle h, SPI_Transaction *t)
{
    (void)h; (void)t;
    Event_post(spiMasterEventHandle, EVENT_SPI_DONE);
}

/* Arm SPI_transfer() (callback mode), then pull SLAVE_READY low so the master
 * clocks. Returns true if the transfer completed before timeout. */
static bool slave_do_transfer(void)
{
    SPI_Transaction t;
    uint32_t events;

    t.count = SPI_FRAME_SIZE;
    t.txBuf = (void *)&txFrame;
    t.rxBuf = (void *)&rxFrame;
    t.arg   = NULL;

    /* Defensive: clear any stuck transfer left armed by a prior aborted attempt
     * (no-op if none pending). Without this, once currentTransaction is stuck,
     * EVERY SPI_transfer() returns false ("arm failed") forever. Cancel may post
     * EVENT_SPI_DONE via the callback - consume that stale event first. */
    SPI_transferCancel(spiHandle);
    Event_pend(spiMasterEventHandle, 0, EVENT_SPI_DONE, BIOS_NO_WAIT);

    if (!SPI_transfer(spiHandle, &t)) {           /* arm (returns immediately) */
        if (display) Display_printf(display, 0, 0, "[SPI Slave] SPI_transfer arm failed");
        return false;
    }
    GPIO_write(Board_SPI_SLAVE_READY, 0);         /* armed -> master may clock */
    events = Event_pend(spiMasterEventHandle, 0, EVENT_SPI_DONE, SPI_TIMEOUT_TICKS);
    GPIO_write(Board_SPI_SLAVE_READY, 1);         /* deassert */
    if ((events & EVENT_SPI_DONE) == 0) {
        /* Master never clocked: cancel so the driver clears currentTransaction. */
        SPI_transferCancel(spiHandle);
        Event_pend(spiMasterEventHandle, 0, EVENT_SPI_DONE, BIOS_NO_WAIT);
        return false;
    }
    return true;
}

static void spiSlaveTaskFunction(UArg a0, UArg a1)
{
    (void)a0; (void)a1;
    Event_Params evParams;
    Event_Params_init(&evParams);
    Event_construct(&spiEventStruct, &evParams);
    spiMasterEventHandle = Event_handle(&spiEventStruct);

    SPI_Params spiParams;
    SPI_Params_init(&spiParams);
    spiParams.frameFormat     = SPI_POL0_PHA1;          /* mode 1, match M4F */
    spiParams.mode            = SPI_SLAVE;              /* CC1310 SDK naming (newer SDKs: SPI_PERIPHERAL) */
    spiParams.transferMode    = SPI_MODE_CALLBACK;
    spiParams.transferCallbackFxn = spiTransferCallback;
    spiParams.bitRate         = 1000000;                /* informational for slave */

    spiHandle = SPI_open(Board_SPI0, &spiParams);
    if (spiHandle == NULL) {
        if (display) Display_printf(display, 0, 0, "[SPI Slave] SPI_open failed");
        while (1) {}
    }

    /* SLAVE_READY out (idle high); MASTER_READY in, falling-edge IRQ. */
    GPIO_setConfig(Board_SPI_SLAVE_READY, GPIO_CFG_OUTPUT | GPIO_CFG_OUT_HIGH);
    GPIO_setConfig(Board_SPI_MASTER_READY, GPIO_CFG_INPUT | GPIO_CFG_IN_INT_FALLING);
    GPIO_setCallback(Board_SPI_MASTER_READY, masterReadyFxn);
    GPIO_enableInt(Board_SPI_MASTER_READY);

    if (display) Display_printf(display, 0, 0, "[SPI Slave] ready (mode1, CALLBACK)");

    for (;;) {
        uint32_t events = Event_pend(spiMasterEventHandle, 0,
                                     EVENT_MASTER_INITIATE | SEND_DATA_TO_SLAVE,
                                     BIOS_WAIT_FOREVER);

        /* FULL-DUPLEX: every transfer carries our pending uplink frame (or a NOP) AND
         * receives the master's downlink (or a NOP) - so a simultaneous up+down is one
         * exchange, never a collision. Drain every queued uplink frame; if the master
         * initiated with nothing queued, do one NOP transfer to receive its cmd.
         * route_rx_frame() runs after EVERY transfer (the old SEND_DATA path skipped it
         * and dropped a piggybacked downlink cmd - the collision bug). */
        {
            NodeFrame nf;
            bool didXfer = false;
            int retry;
            while (mq_receive(spiQueue, (char *)&nf, SPI_QUEUE_MAX_MSG, NULL) != -1) {
                frame_make_node_data(&txFrame, &nf, txSeq++, 0);
                for (retry = 0; retry < MAX_RETRIES; retry++) {
                    memset(&rxFrame, 0, sizeof(rxFrame));
                    if (slave_do_transfer()) {
                        route_rx_frame();
                        break;
                    }
                }
                didXfer = true;
            }
            /* No uplink queued but the master wants to send -> one NOP transfer to
             * receive its cmd. Gated on MASTER_INITIATE so a spurious wake never pulls
             * SLAVE_READY and provokes a pointless exchange. */
            if ((events & EVENT_MASTER_INITIATE) && !didXfer) {
                frame_make_nop(&txFrame);
                for (retry = 0; retry < MAX_RETRIES; retry++) {
                    memset(&rxFrame, 0, sizeof(rxFrame));
                    if (slave_do_transfer()) {
                        route_rx_frame();
                        break;
                    }
                }
            }
        }
    }
}

void spiSlaveTaskInit(void)
{
    struct mq_attr qattr;
    qattr.mq_flags   = 0;
    qattr.mq_maxmsg  = 10;
    qattr.mq_msgsize = SPI_QUEUE_MAX_MSG;
    qattr.mq_curmsgs = 0;

    spiQueue = mq_open(SPI_QUEUE_NAME, O_CREAT | O_RDWR | O_NONBLOCK, 0, &qattr);
    if ((spiQueue == (mqd_t)-1) && (display != NULL)) {
        Display_printf(display, 0, 0, "[SPI Slave] mq_open failed");
    }

    Task_Params taskParams;
    Task_Params_init(&taskParams);
    taskParams.stack     = spiTaskStack;
    taskParams.stackSize = sizeof(spiTaskStack);
    taskParams.priority  = SPI_SLAVE_TASK_PRIORITY;
    Task_construct(&spiTaskStruct, spiSlaveTaskFunction, &taskParams, NULL);
}
