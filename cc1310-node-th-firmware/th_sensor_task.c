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

/* Persistence of the provisioning-assigned address via direct internal-flash
 * access (driverlib). This CC1310 project has NO syscfg, so instead of the NVS
 * driver we reserve one flash sector ourselves with a sector-aligned const array
 * (the linker allocates it in flash and keeps code/other rodata out of it) and
 * erase/program it directly. Survives reboot; a firmware reflash resets it. */
#include DeviceFamily_constructPath(driverlib/flash.h)
#include <ti/drivers/dpl/HwiP.h>

/* --- wire identity --- */
#define TH_NODE_ADDRESS     0xF3u   /* factory default until provisioning assigns one */

/* --- node_protocol.h mirror (keep byte-identical with the gateway) --- */
#define NODE_TH_SENSOR      6u
#define CMD_SEND_DATA_TO_DB 0u
#define CMD_JOIN_REQUEST    4u
#define CMD_JOIN_ACCEPT     5u
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
 * gNodeAddress is the wire source/RX-filter address: factory default until a
 * JOIN_ACCEPT assigns one (then persisted to NVS if available). gFactoryId is
 * this chip's stable FCFG IEEE id, matched against JOIN_ACCEPT.factory_id. */
uint8_t gNodeAddress = TH_NODE_ADDRESS;
uint8_t gFactoryId[NODE_FACTORY_ID_LEN];

static void identity_init(void);
static void identity_persist(uint8_t addr);
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

/* --- provisioning identity persistence (direct internal flash) --- */
#define IDENTITY_MAGIC        0xB1A2C3D4u
#define IDENTITY_SECTOR_SIZE  4096u   /* CC1310 flash sector size */
typedef struct { uint32_t magic; uint8_t addr; } IdentityRec;

/* One reserved flash sector for the assigned address. Sector-aligned AND
 * sector-sized so erasing it touches nothing else; const => the linker places it
 * in flash and keeps other code/rodata out of this sector. */
static const uint8_t gIdentityFlash[IDENTITY_SECTOR_SIZE]
    __attribute__((aligned(IDENTITY_SECTOR_SIZE)));

/* Read this chip's factory id and load any previously-assigned address from flash
 * (so a provisioned node keeps its identity across reboots). */
static void identity_init(void)
{
    read_factory_id(gFactoryId);

    const IdentityRec *rec = (const IdentityRec *)gIdentityFlash;
    bool stored = (rec->magic == IDENTITY_MAGIC && rec->addr >= 0x10u && rec->addr <= 0xEFu);
    if (stored) {
        gNodeAddress = rec->addr;
    }
    Display_printf(display, 0, 0, "[TH] identity: addr 0x%02x (%s)",
                   gNodeAddress, stored ? "flash" : "default/unprovisioned");
}

/* Persist the assigned address: erase the reserved sector, then program the
 * record. Flash erase/program stalls flash reads, so we mask interrupts for the
 * (rare, one-shot) operation - any ISR fetched from flash would otherwise hang. */
static void identity_persist(uint8_t addr)
{
    IdentityRec rec;
    rec.magic = IDENTITY_MAGIC;
    rec.addr  = addr;

    uint32_t base = (uint32_t)gIdentityFlash;
    uintptr_t key = HwiP_disable();
    uint32_t st = FlashSectorErase(base);
    if (st == FAPI_STATUS_SUCCESS) {
        st = FlashProgram((uint8_t *)&rec, base, sizeof(rec));
    }
    HwiP_restore(key);

    if (st != FAPI_STATUS_SUCCESS) {
        Display_printf(display, 0, 0, "[TH] identity persist FAILED (flash st=%lu)", (unsigned long)st);
    }
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
            gNodeAddress = m.payload.joinAcceptData.assigned_addr;
            identity_persist(gNodeAddress);
            Display_printf(display, 0, 0,
                "[TH] JOIN_ACCEPT: assigned addr 0x%02x (now reporting under it)", gNodeAddress);
        } else {
            Display_printf(display, 0, 0, "[TH] JOIN_ACCEPT for another chip - ignored");
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
        if (doSend) {
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
