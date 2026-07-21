/*
 * th_sensor_task.c - rev-2 T&H sensor node application task.
 *
 * Mirrors SolarControllerNode/solar_controller_task.c but for a sensor: a periodic
 * timer wakes the task, it reads SHT35 (every wake) + MCP3421 battery (every Nth
 * wake), builds a NODE_TH_SENSOR telemetry MessageStruct and hands it to the RF
 * task; it also handles provisioning downlinks (JOIN_ACCEPT / REMOVE / UNREGISTERED).
 * No actions (capabilities = 0) -> no relay/command handling.
 *
 * RF layer: reuse SolarControllerNode/rfEchoTx.c ('E' frame + factory_id + JOIN
 * button + ACK/retry). Adapt three things there (see README-rev2.md):
 *   1. buttonCallback2: joinMsg.type = NODE_TH_SENSOR (6), not SOLAR_CONTROLLER.
 *   2. route received commands to thNodeQueue / thNodeEventHandle (below).
 *   3. joinData already carries NODE_CAPABILITIES (=0 here via node_identity.h).
 *
 * Power: structured event-driven so the CC1310 Power policy idles between wakes.
 * True deep-standby tuning (long RTC period, Display/UART off) is a follow-up once
 * telemetry + JOIN are verified on the board.
 */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <mqueue.h>
#include <fcntl.h>

#include <ti/drivers/timer/GPTimerCC26XX.h>
#include <ti/drivers/GPIO.h>
#include <ti/display/Display.h>
#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Event.h>
#include <ti/sysbios/knl/Clock.h>

#include "Board.h"
#include "node_identity.h"   /* gNodeAddress, gFactoryId, CMD_*, identity_persist */
#include "th_sense.h"        /* SHT35 + MCP3421 + SoC facade */

/* --- Node wire identity (match Shared/Protocol/node_protocol.h) --- */
#define NODE_TH_SENSOR      6u
#define CMD_SEND_DATA_TO_DB 0u

/* --- Cadence --- */
#define TH_MEASURE_PERIOD_S  60u    /* SHT35 read + telemetry every 60 s */
#define TH_BATT_EVERY        5u     /* battery (MCP3421) every Nth measure (raise for prod) */
#define TH_TIMER_LOAD        (48000000u * TH_MEASURE_PERIOD_S)  /* 48 MHz GPT ticks */

/* --- Task --- */
#define TH_TASK_STACK_SIZE   1280
#define TH_TASK_PRIORITY     2
#define TH_QUEUE_NAME        "/thNodeQueue"

#define EVENT_MEASURE     (1 << 0)
#define EVENT_RECEIVE_CMD (1 << 1)

/* Local MessageStruct - byte-identical layout to node_protocol.h (thData variant). */
typedef struct {
    uint8_t id;
    uint8_t type;
    uint8_t cmd;
    uint8_t length;
    union {
        struct {
            float    temperature;   /* deg C */
            float    humidity;      /* %RH   */
            uint16_t batt_mv;       /* LFP mV via MCP3421 + 1:2 divider */
            uint8_t  soh_pct;       /* rev-2: repurposed as SoC % (voltage-LUT) */
            int32_t  acc_uah;       /* rev-2: unused (0); reserved */
        } thData;
        struct {
            uint8_t  factory_id[NODE_FACTORY_ID_LEN];
            uint32_t capabilities;
        } joinData;
        struct {
            uint8_t factory_id[NODE_FACTORY_ID_LEN];
            uint8_t assigned_addr;
        } joinAcceptData;
    } payload;
} MessageStruct;

/* RF task (rfEchoTx.c) owns these; we push telemetry here + post EVENT_SEND_PACKET. */
extern mqd_t        radioQueue;
extern Event_Handle radioEventHandle;
#define EVENT_SEND_PACKET (1 << 0)

/* Our inbound queue (rfEchoTx.c routes received downlink commands here). */
mqd_t        thNodeQueue;
Event_Handle thNodeEventHandle;

extern Display_Handle display;

static Task_Struct  thTaskStruct;
static uint8_t      thTaskStack[TH_TASK_STACK_SIZE];
static Event_Struct thEventStruct;

static GPTimerCC26XX_Handle hTimer;

/* Cached battery reading (refreshed every TH_BATT_EVERY measures, sent every measure). */
static uint16_t sBattMv  = 0;
static uint8_t  sSocPct  = 0;
static uint8_t  sBattCtr = 0;

static void timerCallback(GPTimerCC26XX_Handle h, GPTimerCC26XX_IntMask mask)
{
    (void)h; (void)mask;
    Event_post(thNodeEventHandle, EVENT_MEASURE);
}

/* Read sensors (battery only every Nth call) and, if provisioned, send telemetry. */
static void measureAndSend(void)
{
    float tempC = 0.0f, rh = 0.0f;

    th_sense_periph(true);
    th_sense_settle();

    bool thOk = th_sense_read_th(&tempC, &rh);

    if (sBattCtr == 0) {                 /* refresh battery this cycle */
        uint16_t mv; uint8_t soc; bool low;
        if (th_sense_read_batt(&mv, &soc, &low)) {
            sBattMv = mv; sSocPct = soc;
            Display_printf(display, 0, 0, "[TH] batt %u mV, SoC %u%%%s",
                           mv, soc, low ? " (LOW)" : "");
        }
    }
    sBattCtr = (uint8_t)((sBattCtr + 1) % TH_BATT_EVERY);

    th_sense_periph(false);

    if (!thOk) {
        Display_printf(display, 0, 0, "[TH] SHT35 read failed");
        return;
    }
    Display_printf(display, 0, 0, "[TH] T=%.2f C  RH=%.1f %%", tempC, rh);

    /* Unprovisioned -> stay radio-silent (JOIN on the button only). */
    if (gNodeAddress == ADDR_UNPROVISIONED) return;

    MessageStruct m;
    memset(&m, 0, sizeof(m));
    m.id   = gNodeAddress;
    m.type = NODE_TH_SENSOR;
    m.cmd  = CMD_SEND_DATA_TO_DB;
    m.payload.thData.temperature = tempC;
    m.payload.thData.humidity    = rh;
    m.payload.thData.batt_mv     = sBattMv;
    m.payload.thData.soh_pct     = sSocPct;   /* SoC % */
    m.payload.thData.acc_uah     = 0;
    m.length = (uint8_t)(4 + sizeof(m.payload.thData));

    mq_send(radioQueue, (char *)&m, m.length, 0);
    Event_post(radioEventHandle, EVENT_SEND_PACKET);
}

static void handleCommand(const MessageStruct *cmd)
{
    if (cmd->cmd == CMD_JOIN_ACCEPT) {
        /* rfEchoTx already verified the frame factory_id is ours. Adopt + persist +
         * confirm at once (first telemetry from the new address flips the gateway
         * to 'active'). */
        gNodeAddress = cmd->payload.joinAcceptData.assigned_addr;
        identity_persist();
        Display_printf(display, 0, 0, "[TH] JOIN_ACCEPT: address 0x%02x adopted", gNodeAddress);
        sBattCtr = 0;          /* force a battery read on the confirming telemetry */
        measureAndSend();
    } else if (cmd->cmd == CMD_REMOVE || cmd->cmd == CMD_UNREGISTERED) {
        gNodeAddress = ADDR_UNPROVISIONED;
        identity_persist();
        Display_printf(display, 0, 0, "[TH] %s -> unprovisioned (silent)",
                       (cmd->cmd == CMD_REMOVE) ? "REMOVE" : "UNREGISTERED");
    }
}

static void thSensorTaskFunction(UArg arg0, UArg arg1)
{
    (void)arg0; (void)arg1;

    if (!th_sense_init()) {
        Display_printf(display, 0, 0, "[TH] I2C init failed");
    }

    /* Periodic measure timer. */
    GPTimerCC26XX_Params tp;
    GPTimerCC26XX_Params_init(&tp);
    tp.width = GPT_CONFIG_32BIT;
    tp.mode  = GPT_MODE_PERIODIC;
    tp.debugStallMode = GPTimerCC26XX_DEBUG_STALL_OFF;
    hTimer = GPTimerCC26XX_open(Board_GPTIMER2A, &tp);
    GPTimerCC26XX_setLoadValue(hTimer, TH_TIMER_LOAD);
    GPTimerCC26XX_registerInterrupt(hTimer, timerCallback, GPT_INT_TIMEOUT);
    GPTimerCC26XX_start(hTimer);

    Display_printf(display, 0, 0, "[TH] sensor task started (addr 0x%02x)", gNodeAddress);

    for (;;) {
        uint32_t events = Event_pend(thNodeEventHandle, 0,
                                     EVENT_MEASURE | EVENT_RECEIVE_CMD, BIOS_WAIT_FOREVER);
        if (events & EVENT_MEASURE) {
            measureAndSend();
        }
        if (events & EVENT_RECEIVE_CMD) {
            MessageStruct cmd;
            while (mq_receive(thNodeQueue, (char *)&cmd, sizeof(MessageStruct), NULL) != -1) {
                handleCommand(&cmd);
            }
        }
    }
}

void thSensorTaskInit(void)
{
    /* Inbound command queue (rfEchoTx.c posts here on downlink RX). */
    struct mq_attr attr;
    attr.mq_flags   = 0;
    attr.mq_maxmsg  = 4;
    attr.mq_msgsize = sizeof(MessageStruct);
    attr.mq_curmsgs = 0;
    thNodeQueue = mq_open(TH_QUEUE_NAME, O_CREAT | O_RDWR, 0, &attr);

    Event_Params ep;
    Event_Params_init(&ep);
    Event_construct(&thEventStruct, &ep);
    thNodeEventHandle = Event_handle(&thEventStruct);

    Task_Params tparams;
    Task_Params_init(&tparams);
    tparams.stackSize = TH_TASK_STACK_SIZE;
    tparams.priority  = TH_TASK_PRIORITY;
    tparams.stack     = &thTaskStack;
    Task_construct(&thTaskStruct, thSensorTaskFunction, &tparams, NULL);
}
