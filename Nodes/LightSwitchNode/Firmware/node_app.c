/*
 * node_app.c - LightSwitch app task. Dispatch identyczny z gen2 (solar_controller_task.c):
 * JOIN_ACCEPT / REMOVE / UNREGISTERED oraz komenda przekaźnika (TURN_PUMP_ON_OFF =
 * ACTION_SET_RELAY z silnika automatyki). Zamiast pompy sterujemy przekaźnikiem światła
 * (CONFIG_RELAY1 = DIO28). Bez czujników/telemetrii solar.
 *
 * MessageStruct/enumy są zduplikowane 1:1 z rfEchoTx.c (wzorzec gen2 - każdy .c ma własną
 * kopię; muszą być bajtowo identyczne).
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <mqueue.h>
#include <fcntl.h>

#include <ti/drivers/GPIO.h>
#include <ti/display/Display.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Event.h>
#include <ti/sysbios/BIOS.h>

#include "ti_drivers_config.h"     /* CONFIG_RELAY1 */
#include "node_identity.h"         /* gNodeAddress, CMD_JOIN_ACCEPT/REMOVE/UNREGISTERED, identity_persist */
#include "rf_node.h"               /* radioQueue, radioEventHandle, EVENT_SEND_PACKET */

/* ---- kontrakt drutu (1:1 z rfEchoTx.c) ---- */
typedef enum {
    SOLAR_CONTROLLER       = 0,
    BUFOR_CONTROLLER       = 1,
    CURTAINS_CONTROLLER    = 2,
    LIGHT_CONTROLLER       = 3,
    VENTILATION_CONTROLLER = 4,
} nodeType;
typedef enum {
    SEND_DATA_TO_DB        = 0,
    SEND_PUMP_STATUS       = 1,
    TURN_PUMP_ON_OFF       = 2,
    SEND_TEXT_MSG          = 3,
} solarControllerCmd;

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
        struct { uint8_t factory_id[NODE_FACTORY_ID_LEN]; uint32_t capabilities; } joinData;
        struct { uint8_t factory_id[NODE_FACTORY_ID_LEN]; uint8_t assigned_addr; } joinAcceptData;
    } payload;
} MessageStruct;

/* ---- task config ---- */
#define APP_TASK_STACK_SIZE   1024
#define APP_TASK_PRIORITY     1
#define APP_QUEUE_NAME        "/appCmdQueue"
#define EVENT_RECEIVE_CMD     (1 << 1)   /* MUSI = wartość w rfEchoTx.c */

extern Display_Handle display;

/* Nazwy symboli zgodne z externami w rfEchoTx.c (RX wrzuca tu komendy z bramki). */
Event_Handle solarControllerEventHandle;
mqd_t        solarControllerQueue;

static Task_Struct  appTaskStruct;
static uint8_t      appTaskStack[APP_TASK_STACK_SIZE];
static Event_Struct appEventStruct;

/* Podświetlenie: niebieska dioda ma sprzętowy always-on dim (20k->GND); MCU steruje
 * jaśniejszą gałęzią (2k tranzystor na CONFIG_GPIO_BLED = DIO26). Światło ON -> jasno. */
static void syncBacklight(void)
{
    GPIO_write(CONFIG_GPIO_BLED, (uint8_t)GPIO_read(CONFIG_RELAY1));
}

/* Zbuduj i wyślij raport stanu przekaźnika do bramki (reużywa SEND_PUMP_STATUS). */
static void reportRelayState(void)
{
    MessageStruct m;
    memset(&m, 0, sizeof(m));
    m.id   = gNodeAddress;
    m.type = LIGHT_CONTROLLER;
    m.cmd  = SEND_PUMP_STATUS;
    m.payload.pumpData.pumpState = (uint8_t)GPIO_read(CONFIG_RELAY1);
    m.length = 4 + sizeof(m.payload.pumpData);
    mq_send(radioQueue, (char *)&m, m.length, 0);
    Event_post(radioEventHandle, EVENT_SEND_PACKET);
}

void app_relay_toggle(void)
{
    GPIO_toggle(CONFIG_RELAY1);
    syncBacklight();
    reportRelayState();
}

static void appTaskFunction(UArg arg0, UArg arg1)
{
    (void)arg0; (void)arg1;

    GPIO_setConfig(CONFIG_RELAY1, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_LOW);
    GPIO_write(CONFIG_RELAY1, 0);
    GPIO_setConfig(CONFIG_GPIO_BLED, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_LOW);
    syncBacklight();   /* światło OFF -> jasna gałąź BLED zgaszona (zostaje dim HW) */

    for (;;)
    {
        UInt events = Event_pend(solarControllerEventHandle, 0, EVENT_RECEIVE_CMD, BIOS_WAIT_FOREVER);

        if (events & EVENT_RECEIVE_CMD)
        {
            MessageStruct command;
            while (mq_receive(solarControllerQueue, (char *)&command, sizeof(MessageStruct), NULL) != -1)
            {
                /* JOIN_ACCEPT: przyjmij adres z bramki, zapisz, potwierdź od razu
                 * (pierwszy raport z nowego adresu przełącza bramkę na 'active'). */
                if (command.cmd == CMD_JOIN_ACCEPT)
                {
                    gNodeAddress = command.payload.joinAcceptData.assigned_addr;
                    identity_persist();
                    Display_printf(display, 0, 0, "[App] JOIN_ACCEPT: address 0x%02x adopted", gNodeAddress);
                    reportRelayState();
                }
                /* REMOVE / UNREGISTERED: skasuj adres, zapisz, milcz do kolejnego JOIN. */
                else if (command.cmd == CMD_REMOVE || command.cmd == CMD_UNREGISTERED)
                {
                    gNodeAddress = ADDR_UNPROVISIONED;
                    identity_persist();
                    Display_printf(display, 0, 0, "[App] %s -> unprovisioned (silent)",
                                   (command.cmd == CMD_REMOVE) ? "REMOVE" : "UNREGISTERED");
                }
                /* ACTION_SET_RELAY (silnik emituje TURN_PUMP_ON_OFF): ustaw przekaźnik światła. */
                else if (command.cmd == TURN_PUMP_ON_OFF)
                {
                    GPIO_write(CONFIG_RELAY1, command.payload.pumpData.pumpState);
                    syncBacklight();
                    Display_printf(display, 0, 0, "[App] SET_RELAY -> %d", command.payload.pumpData.pumpState);
                    reportRelayState();
                }
            }
        }
    }
}

void appTaskInit(void)
{
    Event_Params evParams;
    Event_Params_init(&evParams);
    Event_construct(&appEventStruct, &evParams);
    solarControllerEventHandle = Event_handle(&appEventStruct);

    struct mq_attr qattr;
    qattr.mq_flags   = 0;
    qattr.mq_maxmsg  = 10;
    /* MUSI == bufor w mq_receive (sizeof(MessageStruct)); POSIX mq_receive wymaga
     * bufora >= mq_msgsize, inaczej EMSGSIZE i komendy nigdy nie są odbierane. */
    qattr.mq_msgsize = sizeof(MessageStruct);
    qattr.mq_curmsgs = 0;
    solarControllerQueue = mq_open(APP_QUEUE_NAME, O_CREAT | O_RDWR | O_NONBLOCK, 0, &qattr);
    if (solarControllerQueue == (mqd_t)-1) {
        Display_printf(display, 0, 0, "[App Task] Failed to open message queue");
    }

    Task_Params params;
    Task_Params_init(&params);
    params.stackSize = APP_TASK_STACK_SIZE;
    params.stack     = &appTaskStack;
    params.priority  = APP_TASK_PRIORITY;
    Task_construct(&appTaskStruct, appTaskFunction, &params, NULL);
}
