/*
 * th_sensor_task.c - gen2 temperature & humidity node (Phase 0).
 *
 * Stand-in for a real T/H sensor: every 60 s it generates random temperature
 * and humidity and ships them to the gateway as a NODE_TH_SENSOR (type 6)
 * MessageStruct via the radio task (radioQueue + EVENT_SEND_PACKET). The button
 * forces an immediate send for testing.
 *
 * Addressing: this node is 0xF3 (Phase 0 fixed), gen2 gateway is 0x00 (gen1 = 0xF0,
 * so we no longer cross-talk with the old network). Provisioning assigns the node
 * address dynamically later.
 *
 * Ported from solar_controller_task.c - all ADC/PWM/relay/ePaper/energy logic
 * stripped; only the timer + RF-send skeleton remains.
 */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <mqueue.h>

#include <ti/drivers/timer/GPTimerCC26XX.h>
#include <ti/sysbios/BIOS.h>
#include <ti/display/Display.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Event.h>
#include <ti/sysbios/knl/Clock.h>

#include "Board.h"

/* CC1310 factory IEEE address (FCFG1) - the per-chip identity used at JOIN.
 * NOTE: verify FCFG1_O_MAC_15_4_0/_1 names against the installed SDK headers. */
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_types.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_fcfg1.h)

/* Persistence of the provisioning-assigned address via the TI NVS driver
 * (internal flash). The NVS region (flashBuf @ NVS_REGIONS_BASE 0x1A000) is
 * reserved in the board file (CC1310_LAUNCHXL.c) the TI-recommended way; the
 * driver handles flash erase/program + cache (VIMS) correctly. Survives reboot;
 * a firmware reflash leaves it (region is NOINIT, not reprogrammed at load). */
#include <ti/drivers/NVS.h>

/* --- wire identity --- */
#define TH_NODE_ADDRESS     0xF3u   /* factory default until provisioning assigns one */

/* --- node_protocol.h mirror (keep byte-identical with the gateway) --- */
#define NODE_TH_SENSOR      6u
#define CMD_SEND_DATA_TO_DB 0u
#define CMD_JOIN_REQUEST    4u
#define CMD_JOIN_ACCEPT     5u
#define CMD_REMOVE          6u
#define ADDR_UNPROVISIONED  0xFFu
#define NODE_FACTORY_ID_LEN 8u

#define TH_TASK_STACK_SIZE  1024
#define TH_TASK_PRIORITY    2

/* thEventHandle events */
#define EVENT_MEASURE       (1 << 0)   /* periodic timer tick           */
#define EVENT_FORCE         (1 << 1)   /* (test) send a reading now     */
#define EVENT_JOIN          (1 << 2)   /* button: send a JOIN request   */

/* radio task's send event (rfEchoTx.c) */
#define EVENT_SEND_PACKET   (1 << 0)

/* 10 s on the 48 MHz GPT (proven load value from the solar node); send every
 * 6th tick -> one reading per 60 s. */
#define TH_TIMER_LOAD       480000000U
#define TH_SEND_EVERY       6

typedef struct {
    uint8_t id;
    uint8_t type;
    uint8_t cmd;
    uint8_t length;
    union {
        struct {
            float Tin, Tout, T4, T3, T2, T1, Tcol;
            int   energyGain, flowRate;
            uint8_t pumpState;
        } solarData;
        struct { uint8_t pumpState; } pumpData;
        struct { float sBuforTemp; } buforData;
        struct { float temperature; float humidity; } thData;
        struct { uint8_t factory_id[NODE_FACTORY_ID_LEN]; } joinData;
        struct { uint8_t factory_id[NODE_FACTORY_ID_LEN]; uint8_t assigned_addr; } joinAcceptData;
        struct { char text[30]; } textData;
    } payload;
} MessageStruct;

extern mqd_t        radioQueue;
extern Event_Handle radioEventHandle;
extern Display_Handle display;

/* --- provisioning identity (shared with rfEchoTx.c) ---
 * gNodeAddress is the wire source/RX-filter address. Default = ADDR_UNPROVISIONED
 * (0xFF): an unprovisioned node is SILENT (no periodic telemetry), it only sends
 * JOIN on the button. A JOIN_ACCEPT assigns a pool address (persisted to NVS);
 * a REMOVE clears it back to 0xFF. gFactoryId = this chip's FCFG IEEE id. */
uint8_t gNodeAddress = ADDR_UNPROVISIONED;
uint8_t gFactoryId[NODE_FACTORY_ID_LEN];

static void identity_init(void);
static bool identity_persist(uint8_t addr);   /* true = written + read-back verified */
void node_handle_rx_command(const uint8_t *bytes, uint8_t len);

static Task_Struct  thTaskStruct;
static uint8_t      thTaskStack[TH_TASK_STACK_SIZE];
static Event_Struct thEventStruct;
Event_Handle        thEventHandle;          /* button (rfEchoTx.c) posts EVENT_FORCE here */

static GPTimerCC26XX_Handle hTimer;
static GPTimerCC26XX_Params tParams;

static void timerCallback(GPTimerCC26XX_Handle handle, GPTimerCC26XX_IntMask mask)
{
    Event_post(thEventHandle, EVENT_MEASURE);
}

/* Phase-0 stand-ins for a real sensor: T in 18.00-25.99 C, RH in 30.00-69.99 %. */
static float randTemp(void) { return 18.0f + (float)(rand() % 800)  / 100.0f; }
static float randHum(void)  { return 30.0f + (float)(rand() % 4000) / 100.0f; }

static void sendReading(MessageStruct *msg)
{
    msg->id   = gNodeAddress;   /* assigned address once provisioned, else factory default */
    msg->type = NODE_TH_SENSOR;
    msg->cmd  = CMD_SEND_DATA_TO_DB;
    msg->payload.thData.temperature = randTemp();
    msg->payload.thData.humidity    = randHum();
    msg->length = 4 + sizeof(msg->payload.thData);   /* = 12 */

    mq_send(radioQueue, (char *)msg, msg->length, 0);
    Event_post(radioEventHandle, EVENT_SEND_PACKET);

    Display_printf(display, 0, 0, "[TH] sent T=%.2f H=%.2f",
                   msg->payload.thData.temperature, msg->payload.thData.humidity);
}

/* Read the CC1310's unique factory IEEE address (8 B) - the chip identity used
 * during provisioning, before the node has an assigned address. */
static void read_factory_id(uint8_t out[NODE_FACTORY_ID_LEN])
{
    uint32_t lo = HWREG(FCFG1_BASE + FCFG1_O_MAC_15_4_0);
    uint32_t hi = HWREG(FCFG1_BASE + FCFG1_O_MAC_15_4_1);
    out[0] = (uint8_t)lo;        out[1] = (uint8_t)(lo >> 8);
    out[2] = (uint8_t)(lo >> 16); out[3] = (uint8_t)(lo >> 24);
    out[4] = (uint8_t)hi;        out[5] = (uint8_t)(hi >> 8);
    out[6] = (uint8_t)(hi >> 16); out[7] = (uint8_t)(hi >> 24);
}

/* JOIN request: src ADDR_UNPROVISIONED, type = our node type, payload = factory
 * id. The radio task takes the wire src from msg->id (so 0xFF here). */
static void sendJoinRequest(MessageStruct *msg)
{
    msg->id   = ADDR_UNPROVISIONED;
    msg->type = NODE_TH_SENSOR;
    msg->cmd  = CMD_JOIN_REQUEST;
    read_factory_id(msg->payload.joinData.factory_id);
    msg->length = 4 + sizeof(msg->payload.joinData);   /* = 12 */

    mq_send(radioQueue, (char *)msg, msg->length, 0);
    Event_post(radioEventHandle, EVENT_SEND_PACKET);

    Display_printf(display, 0, 0,
        "[TH] JOIN sent (factory %02x%02x%02x%02x%02x%02x%02x%02x)",
        msg->payload.joinData.factory_id[0], msg->payload.joinData.factory_id[1],
        msg->payload.joinData.factory_id[2], msg->payload.joinData.factory_id[3],
        msg->payload.joinData.factory_id[4], msg->payload.joinData.factory_id[5],
        msg->payload.joinData.factory_id[6], msg->payload.joinData.factory_id[7]);
}

/* --- provisioning identity persistence (TI NVS driver) --- */
#define IDENTITY_MAGIC  0xB1A2C3D4u
/* 8 B, 4-aligned: some flash needs multiple-of-4 write lengths (NVS docs). */
typedef struct { uint32_t magic; uint8_t addr; uint8_t _pad[3]; } IdentityRec;
static NVS_Handle gNvs = NULL;

/* Read this chip's factory id and load any previously-assigned address from NVS
 * (so a provisioned node keeps its identity across reboots). */
static void identity_init(void)
{
    read_factory_id(gFactoryId);

    NVS_init();
    NVS_Params p;
    NVS_Params_init(&p);
    gNvs = NVS_open(Board_NVSINTERNAL, &p);

    bool stored = false;
    if (gNvs != NULL) {
        IdentityRec rec;
        if (NVS_read(gNvs, 0, &rec, sizeof(rec)) == NVS_STATUS_SUCCESS &&
            rec.magic == IDENTITY_MAGIC && rec.addr >= 0x10u && rec.addr <= 0xEFu) {
            gNodeAddress = rec.addr;
            stored = true;
        }
    }
    Display_printf(display, 0, 0, "[TH] identity: addr 0x%02x (%s)",
                   gNodeAddress,
                   (gNvs == NULL) ? "NVS open FAILED" : (stored ? "NVS" : "default/unprovisioned"));
}

/* Persist the assigned address and PROVE it by reading back (the gateway treats
 * this read-back as the basis of its confirmation). NVS_WRITE_ERASE erases the
 * sector first; NVS_WRITE_POST_VERIFY already read-back-verifies inside the
 * driver - we add an explicit read for certainty + clear logging.
 * Returns true only when the stored value matches. */
static bool identity_persist(uint8_t addr)
{
    if (gNvs == NULL) {
        Display_printf(display, 0, 0, "[TH] identity persist skipped (NVS not open)");
        return false;
    }
    IdentityRec rec;
    rec.magic = IDENTITY_MAGIC;
    rec.addr  = addr;
    rec._pad[0] = rec._pad[1] = rec._pad[2] = 0;

    int_fast16_t st = NVS_write(gNvs, 0, &rec, sizeof(rec),
                                NVS_WRITE_ERASE | NVS_WRITE_POST_VERIFY);
    if (st != NVS_STATUS_SUCCESS) {
        Display_printf(display, 0, 0, "[TH] identity persist FAILED (NVS st=%d)", (int)st);
        return false;
    }

    IdentityRec chk;
    if (NVS_read(gNvs, 0, &chk, sizeof(chk)) != NVS_STATUS_SUCCESS ||
        chk.magic != IDENTITY_MAGIC || chk.addr != addr) {
        Display_printf(display, 0, 0, "[TH] identity read-back MISMATCH (addr 0x%02x)", addr);
        return false;
    }
    Display_printf(display, 0, 0, "[TH] identity persisted+verified: addr 0x%02x", addr);
    return true;
}

/* Confirm a completed REMOVE to the gateway: one final frame from the OLD
 * address (so the gateway frees it) carrying our factory id. Sent before we go
 * silent. The node's own ACK-match may miss (we flip to 0xFF right after), so it
 * may retransmit a few times - harmless, the gateway's confirm is idempotent. */
static void sendRemoveConfirm(uint8_t oldAddr)
{
    MessageStruct msg;
    msg.id   = oldAddr;
    msg.type = NODE_TH_SENSOR;
    msg.cmd  = CMD_REMOVE;
    memcpy(msg.payload.joinData.factory_id, gFactoryId, NODE_FACTORY_ID_LEN);
    msg.length = 4 + sizeof(msg.payload.joinData);   /* = 12 */
    mq_send(radioQueue, (char *)&msg, msg.length, 0);
    Event_post(radioEventHandle, EVENT_SEND_PACKET);
    Display_printf(display, 0, 0, "[TH] REMOVE confirm queued (from 0x%02x)", oldAddr);
}

/* Handle a gateway command addressed to this node (called from rfEchoTx.c RX with
 * the raw MessageStruct bytes). Step 5: a JOIN_ACCEPT whose factory_id matches us
 * switches our address to the assigned one (and persists it). */
void node_handle_rx_command(const uint8_t *bytes, uint8_t len)
{
    MessageStruct m;
    if (len > sizeof(m)) len = sizeof(m);
    memcpy(&m, bytes, len);

    if (m.cmd == CMD_JOIN_ACCEPT) {
        if (memcmp(m.payload.joinAcceptData.factory_id, gFactoryId, NODE_FACTORY_ID_LEN) == 0) {
            uint8_t assigned = m.payload.joinAcceptData.assigned_addr;
            /* Adopt the address ONLY after a verified NVS write+read-back. The
             * first telemetry under it is the gateway's provisioning confirmation. */
            if (identity_persist(assigned)) {
                gNodeAddress = assigned;
                Display_printf(display, 0, 0,
                    "[TH] JOIN_ACCEPT: now 0x%02x (sending immediate confirm)", assigned);
                /* Report once RIGHT NOW so the gateway flips us active immediately,
                 * instead of waiting for the next periodic tick (up to 60 s). */
                MessageStruct confirm;
                sendReading(&confirm);
            } else {
                Display_printf(display, 0, 0,
                    "[TH] JOIN_ACCEPT: NVS failed - staying unprovisioned (gateway will retry)");
            }
        } else {
            Display_printf(display, 0, 0, "[TH] JOIN_ACCEPT for another chip - ignored");
        }
    } else if (m.cmd == CMD_REMOVE) {
        if (memcmp(m.payload.joinData.factory_id, gFactoryId, NODE_FACTORY_ID_LEN) == 0) {
            uint8_t oldAddr = gNodeAddress;
            /* Clear identity (write 0xFF). Only after a verified clear do we send
             * the confirmation from the OLD address and go silent. If it fails we
             * keep the address so the gateway can retry on next contact. */
            if (identity_persist(ADDR_UNPROVISIONED)) {
                sendRemoveConfirm(oldAddr);
                gNodeAddress = ADDR_UNPROVISIONED;
                Display_printf(display, 0, 0,
                    "[TH] REMOVE: cleared -> unprovisioned (confirmed from 0x%02x)", oldAddr);
            } else {
                Display_printf(display, 0, 0,
                    "[TH] REMOVE: NVS clear failed - keeping 0x%02x (gateway will retry)", oldAddr);
            }
        } else {
            Display_printf(display, 0, 0, "[TH] REMOVE for another chip - ignored");
        }
    }
}

static void thTaskFunction(UArg arg0, UArg arg1)
{
    uint32_t      events;
    uint8_t       tickCount = 0;
    MessageStruct msg;

    identity_init();
    srand((unsigned)Clock_getTicks() ^ gNodeAddress);

    GPTimerCC26XX_Params_init(&tParams);
    tParams.width          = GPT_CONFIG_32BIT;
    tParams.mode           = GPT_MODE_PERIODIC;
    tParams.debugStallMode = GPTimerCC26XX_DEBUG_STALL_OFF;
    hTimer = GPTimerCC26XX_open(Board_GPTIMER2A, &tParams);
    GPTimerCC26XX_setLoadValue(hTimer, TH_TIMER_LOAD);
    GPTimerCC26XX_registerInterrupt(hTimer, timerCallback, GPT_INT_TIMEOUT);
    GPTimerCC26XX_start(hTimer);

    while (1) {
        events = Event_pend(thEventHandle, 0,
                            EVENT_MEASURE | EVENT_FORCE | EVENT_JOIN, BIOS_WAIT_FOREVER);

        if (events & EVENT_JOIN) {
            sendJoinRequest(&msg);
        }

        bool doSend = false;
        if (events & EVENT_MEASURE) {
            if (++tickCount >= TH_SEND_EVERY) { tickCount = 0; doSend = true; }
        }
        if (events & EVENT_FORCE) {
            doSend = true;
        }
        /* Only a provisioned node reports telemetry; unprovisioned (0xFF) stays
         * silent and is heard from only via JOIN (button). */
        if (doSend && gNodeAddress != ADDR_UNPROVISIONED) {
            sendReading(&msg);
        }
    }
}

void thSensorTaskInit(void)
{
    Event_Params evParams;
    Event_Params_init(&evParams);
    Event_construct(&thEventStruct, &evParams);
    thEventHandle = Event_handle(&thEventStruct);

    Task_Params params;
    Task_Params_init(&params);
    params.stackSize = TH_TASK_STACK_SIZE;
    params.stack     = &thTaskStack;
    params.priority  = TH_TASK_PRIORITY;
    Task_construct(&thTaskStruct, thTaskFunction, &params, NULL);
}
