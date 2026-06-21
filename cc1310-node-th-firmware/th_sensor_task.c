/*
 * th_sensor_task.c - gen2 temperature & humidity node (Phase 0).
 *
 * Stand-in for a real T/H sensor: every 60 s it generates random temperature
 * and humidity and ships them to the gateway as a NODE_TH_SENSOR (type 6)
 * MessageStruct via the radio task (radioQueue + EVENT_SEND_PACKET). The button
 * forces an immediate send for testing.
 *
 * Phase 0 keeps the CURRENT addressing scheme: this node is 0xF3, gateway 0xF0.
 * Provisioning (later) will assign the address dynamically.
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

/* --- wire identity (Phase 0: fixed; provisioning assigns later) --- */
#define TH_NODE_ADDRESS     0xF3u

/* --- node_protocol.h mirror (keep byte-identical with the gateway) --- */
#define NODE_TH_SENSOR      6u
#define CMD_SEND_DATA_TO_DB 0u

#define TH_TASK_STACK_SIZE  1024
#define TH_TASK_PRIORITY    2

/* thEventHandle events */
#define EVENT_MEASURE       (1 << 0)   /* periodic timer tick      */
#define EVENT_FORCE         (1 << 1)   /* button: send immediately */

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
        struct { char text[30]; } textData;
    } payload;
} MessageStruct;

extern mqd_t        radioQueue;
extern Event_Handle radioEventHandle;
extern Display_Handle display;

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
    msg->id   = TH_NODE_ADDRESS;
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

static void thTaskFunction(UArg arg0, UArg arg1)
{
    uint32_t      events;
    uint8_t       tickCount = 0;
    MessageStruct msg;

    srand((unsigned)Clock_getTicks() ^ TH_NODE_ADDRESS);

    GPTimerCC26XX_Params_init(&tParams);
    tParams.width          = GPT_CONFIG_32BIT;
    tParams.mode           = GPT_MODE_PERIODIC;
    tParams.debugStallMode = GPTimerCC26XX_DEBUG_STALL_OFF;
    hTimer = GPTimerCC26XX_open(Board_GPTIMER2A, &tParams);
    GPTimerCC26XX_setLoadValue(hTimer, TH_TIMER_LOAD);
    GPTimerCC26XX_registerInterrupt(hTimer, timerCallback, GPT_INT_TIMEOUT);
    GPTimerCC26XX_start(hTimer);

    while (1) {
        events = Event_pend(thEventHandle, 0, EVENT_MEASURE | EVENT_FORCE, BIOS_WAIT_FOREVER);

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
