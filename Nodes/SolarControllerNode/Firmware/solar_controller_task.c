#include <stddef.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <mqueue.h>
#include <stdlib.h>
#include <ti/drivers/timer/GPTimerCC26XX.h>
#include <ti/drivers/PWM.h>
#include <ti/drivers/Watchdog.h>

#include <ti/drivers/Power.h>
#include <ti/drivers/Board.h>
#include <ti/sysbios/BIOS.h>
#include <ti/drivers/GPIO.h>
#include <ti/drivers/SPI.h>
#include <ti/display/Display.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Event.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/devices/DeviceFamily.h>

#include "ads1256.h"
#include "Board.h"
#include "node_identity.h"   /* gNodeAddress, CMD_JOIN_ACCEPT/REMOVE/UNREGISTERED, identity_persist */


//#define MAX_MSG_SIZE          50
#define SOLAR_CONTROLLER_QUEUE_NAME      "/solarControllerQueue"
#define SOLAR_CONTROLLER_TASK_STACK_SIZE 1536
#define SOLAR_CONTROLLER_TASK_PRIORITY   2


#define EVENT_SEND_PACKET     (1 << 0)
#define EVENT_RECEIVE_CMD     (1 << 1)
#define EVENT_MEASURE_TEMPERATURE (1 << 2)
#define SPI_MSG_LENGTH  6

typedef enum
{
    SOLAR_CONTROLLER          = 0,
    BUFOR_CONTROLLER          = 1,
    CURTAINS_CONTROLLER       = 2,
    LIGHT_CONTROLLER          = 3,
    VENTILATION_CONTROLLER    = 4,
} nodeType;
typedef enum
{
    SEND_DATA_TO_DB           = 0,
    SEND_PUMP_STATUS          = 1,
    TURN_PUMP_ON_OFF          = 2,
} solarControllerCmd;

typedef struct {
    uint8_t id;
    uint8_t type;
    uint8_t cmd;
    uint8_t length;
    union {
        struct {
            float Tin;
            float Tout;
            float T4;
            float T3;
            float T2;
            float T1;
            float Tcol;
            int   energyGain;
            int   flowRate;
            uint8_t pumpState;
        } solarData;

        struct {
            uint8_t pumpState;
        } pumpData;

        struct {
            float sBuforTemp;
        } buforData;

        struct {
            char text[30];
        } textData;

        /* provisioning: node -> gw (CMD_JOIN_REQUEST). */
        struct {
            uint8_t  factory_id[NODE_FACTORY_ID_LEN];
            uint32_t capabilities;   /* NODE_CAP(ACTION_*) this node can execute */
        } joinData;

        /* provisioning: gw -> node (CMD_JOIN_ACCEPT). */
        struct {
            uint8_t factory_id[NODE_FACTORY_ID_LEN];
            uint8_t assigned_addr;
        } joinAcceptData;
    } payload;
} MessageStruct;

#define SOLAR_CONTROLLER_QUEUE_MAX_MSG_SIZE sizeof(MessageStruct)

extern mqd_t radioQueue;
extern Event_Handle radioEventHandle;

static GPTimerCC26XX_Handle hTimer;
static GPTimerCC26XX_Params tParams;

static Watchdog_Handle watchdogHandle;
static Watchdog_Params watchdogParams;
static uint32_t        reloadValue;

static PWM_Handle pwm1 = NULL;
static PWM_Params pwm1Params;
static uint16_t   pwm1Period = 1200;
static uint16_t   pwm1Duty = 0;

static float measuredTemp[] = {500.0f, 500.0f, 500.0f, 500.0f, 500.0f, 500.0f, 500.0f, 500.0f};
static float measuredTempPrev[] = {500.0f, 500.0f, 500.0f, 500.0f, 500.0f, 500.0f, 500.0f, 500.0f};
static uint16_t   flowValue = 0; // Przep�yw w procentach
static float      energyGainFloat = 0;
static int   energyGain = 0;
static int        updatePwm = 1;
static float      deltaT_float = 0;
int min3couter = 17;
int send_data_to_db_flag = 11;

static Task_Struct solarControllerTaskStruct;
static uint8_t solarControllerTaskStack[SOLAR_CONTROLLER_TASK_STACK_SIZE];

//SPI_Handle spiHandleMasterEpaper;
//SPI_Params      spiParams0;
//SPI_Params      spiParams1;

static Event_Struct solarControllerEventStruct;
Event_Handle solarControllerEventHandle;
extern Display_Handle display;
mqd_t solarControllerQueue;

static void watchdogCallback(uintptr_t watchdogHandle)
{
    //GPIO_write(CC1310_LAUNCHXL_GPIO_RELAY3, 0);
    while (1) {}
}
void timerCallback(GPTimerCC26XX_Handle handle, GPTimerCC26XX_IntMask interruptMask)
{
    Event_post(solarControllerEventHandle, EVENT_MEASURE_TEMPERATURE);
}

static void solarControllerTaskFunction(UArg arg0, UArg arg1)
{
    //Zmienna do event�w
    uint32_t events;

    MessageStruct solarMessage;

    // Konfiguracja przeka�nik�w
    GPIO_setConfig(CC1310_LAUNCHXL_GPIO_RELAY1, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_LOW);
    GPIO_write(CC1310_LAUNCHXL_GPIO_RELAY1, 0);
    GPIO_setConfig(CC1310_LAUNCHXL_GPIO_RELAY2, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_LOW);
    GPIO_write(CC1310_LAUNCHXL_GPIO_RELAY2, 0);
    GPIO_setConfig(CC1310_LAUNCHXL_GPIO_RELAY3, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_LOW);
    GPIO_write(CC1310_LAUNCHXL_GPIO_RELAY3, 0);

    //Inicjalizacja przetwornika ADC
    ADS1256_HardwareInit(Board_SPI0);
    ADS1256_Init();

    //Konfiguracja timera sprzetowego
    GPTimerCC26XX_Params_init(&tParams);
    tParams.width = GPT_CONFIG_32BIT;
    tParams.mode = GPT_MODE_PERIODIC;
    tParams.debugStallMode = GPTimerCC26XX_DEBUG_STALL_OFF;
    hTimer = GPTimerCC26XX_open(Board_GPTIMER2A, &tParams);
    GPTimerCC26XX_setLoadValue(hTimer, 480000000);
    GPTimerCC26XX_registerInterrupt(hTimer, timerCallback,GPT_INT_TIMEOUT);

    Watchdog_Params_init(&watchdogParams);
    watchdogParams.callbackFxn = (Watchdog_Callback) watchdogCallback;
    watchdogParams.debugStallMode = Watchdog_DEBUG_STALL_ON;
    watchdogParams.resetMode = Watchdog_RESET_ON;

    watchdogHandle = Watchdog_open(Board_WATCHDOG0, &watchdogParams);
    if (watchdogHandle == NULL) {
         /* Error opening Watchdog */
        Display_printf(display, 0, 0, "Error opening Watchdog");
    }
    //GPIO_write(CC1310_LAUNCHXL_GPIO_RELAY3, 1);
  //  reloadValue = Watchdog_convertMsToTicks(watchdogHandle, WATCHDOG_TIMEOUT_MS);

  //  if (reloadValue != 0) {
  //      Watchdog_setReload(watchdogHandle, reloadValue);
 //   }

    PWM_Params_init(&pwm1Params);
    pwm1Params.dutyUnits = PWM_DUTY_US;
    pwm1Params.dutyValue = pwm1Duty;
    pwm1Params.periodUnits = PWM_PERIOD_US;
    pwm1Params.periodValue = pwm1Period;
    pwm1 = PWM_open(Board_PWM1, &pwm1Params);

    if (pwm1 == NULL) {
        /* Board_PWM0 did not open */
        Display_printf(display, 0, 0, "Board_PWM0 did not open");
        // while (1);
    }

    PWM_start(pwm1);


    GPTimerCC26XX_start(hTimer);
    ADS1256_ReadTemperature(measuredTemp);
    Display_printf(display, 0, 0, "ADC0: %f", measuredTemp[0]);
    if (min3couter >= 17)
    {
        char buf[10];
        snprintf(buf, sizeof(buf), "%.1f%%", measuredTemp[0]);
        Display_printf(display, 0, 0, buf);
//      displayTextOnEpaper(buf);
        min3couter = 0;
    }
    else
    {
        min3couter = min3couter + 1;
    }



    while(1)
    {

        events = Event_pend(solarControllerEventHandle, 0, EVENT_RECEIVE_CMD | EVENT_MEASURE_TEMPERATURE, BIOS_WAIT_FOREVER);

        if (events & EVENT_MEASURE_TEMPERATURE)
        {
            Watchdog_clear(watchdogHandle);
            measuredTempPrev[0] = measuredTemp[0];
            measuredTempPrev[1] = measuredTemp[1];
            measuredTempPrev[2] = measuredTemp[2];
            measuredTempPrev[3] = measuredTemp[3];
            measuredTempPrev[4] = measuredTemp[4];
            measuredTempPrev[5] = measuredTemp[5];
            measuredTempPrev[6] = measuredTemp[6];
            measuredTempPrev[7] = measuredTemp[7];

            ADS1256_ReadTemperature(measuredTemp);

            measuredTemp[0] = (Adc_Value[0]*5.488e-13*Adc_Value[0]) + (Adc_Value[0]*3.092e-5) - 25.732;
            measuredTemp[1] = (Adc_Value[1]*5.445e-13*Adc_Value[1]) + (Adc_Value[1]*3.094e-5) - 25.354;
            measuredTemp[2] = (Adc_Value[2]*5.458e-13*Adc_Value[2]) + (Adc_Value[2]*3.094e-5) - 24.952;
            measuredTemp[3] = (Adc_Value[3]*5.530e-13*Adc_Value[3]) + (Adc_Value[3]*3.086e-5) - 25.032;
            measuredTemp[4] = (Adc_Value[4]*5.393e-13*Adc_Value[4]) + (Adc_Value[4]*3.082e-5) - 25.381;
            measuredTemp[5] = (Adc_Value[5]*5.414e-13*Adc_Value[5]) + (Adc_Value[5]*3.087e-5) - 25.062;
            measuredTemp[6] = (Adc_Value[6]*5.445e-13*Adc_Value[6]) + (Adc_Value[6]*3.083e-5) - 25.507;
            measuredTemp[7] = (Adc_Value[7]*5.398e-13*Adc_Value[7]) + (Adc_Value[7]*3.087e-5) - 25.498;

            deltaT_float = measuredTemp[6] - measuredTemp[4];

                                      if (updatePwm == 1)
                                      {
                                          if (((deltaT_float) > 7) && flowValue < 6)
                                          {
                                              flowValue = 6;
                                              pwm1Duty = 234;
                                              GPIO_write(CC1310_LAUNCHXL_GPIO_RELAY3, 1);
                                              PWM_setDuty(pwm1, pwm1Duty);
                                          }
                                          else if (((deltaT_float-7)*6+6 > flowValue+6) && flowValue >= 6 && flowValue <= 78)
                                          {
                                              flowValue = flowValue + 6;
                                              pwm1Duty = pwm1Duty + 54;
                                              PWM_setDuty(pwm1, pwm1Duty);
                                          }
                                          else if (((deltaT_float-7)*6 +6 < flowValue-6) && flowValue >= 12)
                                          {
                                              flowValue = flowValue - 6;
                                              pwm1Duty = pwm1Duty - 54;
                                              PWM_setDuty(pwm1, pwm1Duty);
                                          }
                                          else if (((deltaT_float) < 6) && flowValue == 6)
                                          {
                                              flowValue = 0;
                                              pwm1Duty = 0;
                                              PWM_setDuty(pwm1, pwm1Duty);
                                              GPIO_write(CC1310_LAUNCHXL_GPIO_RELAY3, 0);
                                          }

                                          updatePwm = 0;
                                      }
                                      else
                                      {
                                          updatePwm = 1;
                                      }

                                      energyGainFloat = energyGainFloat + 0.5*(measuredTempPrev[0]-measuredTempPrev[1]+measuredTemp[0]-measuredTemp[1])*(flowValue)/6;

                                      if (send_data_to_db_flag >= 11)
                                      {
                                          energyGain = energyGainFloat;

                                          /* Report only once provisioned; an unprovisioned
                                           * node stays radio-silent (JOIN on the button only)
                                           * but keeps measuring/accumulating. */
                                          if (gNodeAddress != ADDR_UNPROVISIONED)
                                          {
                                          solarMessage.id =   gNodeAddress;
                                          solarMessage.type = SOLAR_CONTROLLER;
                                          solarMessage.cmd =  SEND_DATA_TO_DB;
                                          solarMessage.payload.solarData.Tin =        measuredTemp[0];
                                          solarMessage.payload.solarData.Tout =       measuredTemp[1];
                                          solarMessage.payload.solarData.T4 =         measuredTemp[2];
                                          solarMessage.payload.solarData.T3 =         measuredTemp[3];
                                          solarMessage.payload.solarData.T2 =         measuredTemp[4];
                                          solarMessage.payload.solarData.T1 =         measuredTemp[5];
                                          solarMessage.payload.solarData.Tcol =       measuredTemp[6];
                                          solarMessage.payload.solarData.flowRate =   flowValue;
                                          solarMessage.payload.solarData.energyGain = energyGain;
                                          solarMessage.payload.solarData.pumpState = GPIO_read(CC1310_LAUNCHXL_GPIO_RELAY1);
                                          solarMessage.length = 4 + sizeof(solarMessage.payload.solarData);
                                          mq_send(radioQueue, (char *) &solarMessage, solarMessage.length, 0);
                                          Event_post(radioEventHandle, EVENT_SEND_PACKET);
                                          }
                                          energyGainFloat = 0;
                                          send_data_to_db_flag = 0;
                                      }
                                      else
                                      {
                                          send_data_to_db_flag = send_data_to_db_flag + 1;
                                      }


            if (min3couter >= 17)
            {
                Display_printf(display, 0, 0, "ADC0: %f", measuredTemp[0]);
                char buf[10];
                snprintf(buf, sizeof(buf), "%.1f%%", measuredTemp[0]);
                Display_printf(display, 0, 0, buf);
//                displayTextOnEpaper(buf);
                min3couter = 0;
            }
            else
            {
                min3couter = min3couter + 1;
            }

        }
        else if (events & EVENT_RECEIVE_CMD)
        {
            MessageStruct command;
            while (mq_receive(solarControllerQueue, (char *)&command, sizeof(MessageStruct), NULL) != -1)
            {
                /* JOIN_ACCEPT: adopt the gateway-assigned address, persist it, and confirm
                 * at once (first telemetry from the new address flips the gateway to
                 * 'active'). rfEchoTx already verified the frame factory_id is ours. */
                if (command.cmd == CMD_JOIN_ACCEPT)
                {
                    gNodeAddress = command.payload.joinAcceptData.assigned_addr;
                    identity_persist();
                    Display_printf(display, 0, 0, "[Solar] JOIN_ACCEPT: address 0x%02x adopted", gNodeAddress);
                    solarMessage.id   = gNodeAddress;
                    solarMessage.type = SOLAR_CONTROLLER;
                    solarMessage.cmd  = SEND_PUMP_STATUS;
                    solarMessage.payload.pumpData.pumpState = GPIO_read(CC1310_LAUNCHXL_GPIO_RELAY1);
                    solarMessage.length = 4 + sizeof(solarMessage.payload.pumpData);
                    mq_send(radioQueue, (char *) &solarMessage, solarMessage.length, 0);
                    Event_post(radioEventHandle, EVENT_SEND_PACKET);
                }
                /* REMOVE (user deleted) / UNREGISTERED (identity mismatch): drop the
                 * address, persist, go silent until the next JOIN. */
                else if (command.cmd == CMD_REMOVE || command.cmd == CMD_UNREGISTERED)
                {
                    gNodeAddress = ADDR_UNPROVISIONED;
                    identity_persist();
                    Display_printf(display, 0, 0, "[Solar] %s -> unprovisioned (silent)",
                                   (command.cmd == CMD_REMOVE) ? "REMOVE" : "UNREGISTERED");
                }
                else if (command.type == SOLAR_CONTROLLER && command.cmd == TURN_PUMP_ON_OFF)
                {
                    GPIO_write(CC1310_LAUNCHXL_GPIO_RELAY1, command.payload.pumpData.pumpState);
                    solarMessage.id = gNodeAddress;
                    solarMessage.type = SOLAR_CONTROLLER;
                    solarMessage.cmd =  SEND_PUMP_STATUS;
                    solarMessage.payload.pumpData.pumpState = GPIO_read(CC1310_LAUNCHXL_GPIO_RELAY1);
                    solarMessage.length = 4 + sizeof(solarMessage.payload.pumpData);
                    mq_send(radioQueue, (char *) &solarMessage, solarMessage.length, 0);
                    Event_post(radioEventHandle, EVENT_SEND_PACKET);
                }
            }
        }
    }

}
void solarControllerTaskInit()
{
    // Tworzenie mechanizmu event�w dla taska
    Event_Params evParams;
    Event_Params_init(&evParams);
    Event_construct(&solarControllerEventStruct, &evParams);
    solarControllerEventHandle = Event_handle(&solarControllerEventStruct);

    // Tworzenie kolejki dla Tasku
    struct mq_attr qattr;
    qattr.mq_flags = 0;
    qattr.mq_maxmsg = 10;
    qattr.mq_msgsize = SOLAR_CONTROLLER_QUEUE_MAX_MSG_SIZE;
    qattr.mq_curmsgs = 0;

    solarControllerQueue = mq_open(SOLAR_CONTROLLER_QUEUE_NAME, O_CREAT | O_RDWR | O_NONBLOCK, 0, &qattr);

    if (solarControllerQueue == (mqd_t)-1) {
        Display_printf(display, 0, 0, "[Solar Task] Failed to open message queue\n");
    }

    // Tworzenie tasku
    Task_Params params;
    Task_Params_init(&params);
    params.stackSize = SOLAR_CONTROLLER_TASK_STACK_SIZE;
    params.stack = &solarControllerTaskStack;
    params.priority = SOLAR_CONTROLLER_TASK_PRIORITY;

    Task_construct(&solarControllerTaskStruct, solarControllerTaskFunction, &params, NULL);
}



