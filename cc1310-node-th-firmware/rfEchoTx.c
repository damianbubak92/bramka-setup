#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <mqueue.h>
#include <stdlib.h>

#include <ti/drivers/rf/RF.h>
#include <ti/display/Display.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Event.h>
#include <ti/drivers/GPIO.h>
#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/rf_prop_mailbox.h)

#include "Board.h"

#include "smartrf_settings/smartrf_settings.h"
#include "RFQueue.h"

#define RADIO_TASK_STACK_SIZE 1536
#define RADIO_TASK_PRIORITY   2
#define RADIO_QUEUE_NAME      "/radioQueue"
#define MAX_MSG_SIZE          70
#define EVENT_SEND_PACKET     (1 << 0)
#define EVENT_RADIO_RX        (1 << 1)
#define EVENT_ACK_SENT        (1 << 2)
#define CONCENTRATOR_ADDRESS  0x00   /* gen2 gateway = 0x00 (was 0xF0); old gen1 gateway ignores us, so no cross-talk / duplicates */
#define NODE_ADDRESS          0xF3   /* gen2 T/H node (Phase 0 fixed addr; provisioning assigns a pool addr later) */
#define NUM_APPENDED_BYTES    2

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
    SEND_TEXT_MSG             = 3,
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
    } payload;
} MessageStruct;


static Task_Struct radioTaskStruct;
static uint8_t radioTaskStack[RADIO_TASK_STACK_SIZE];

extern Display_Handle display;

#define PAYLOAD_LENGTH         50
#define TX_DELAY             (uint32_t)(4000000*0.1f)
#define NUM_DATA_ENTRIES       4
#define NUM_APPENDED_BYTES     2
#define MAX_RETRIES            2
#define ACK_TIMEOUT_TICKS      (50 * (4000000 / 1000)) //50ms
#define RX_TIMEOUT             (50 * (4000000 / 1000)) //50ms
#define EVENT_RECEIVE_CMD     (1 << 1)

static void echoCallback(RF_Handle h, RF_CmdHandle ch, RF_EventMask e);
static void echoCallbackACK(RF_Handle h, RF_CmdHandle ch, RF_EventMask e);

static RF_Object rfObject;
static RF_Handle rfHandle;

#pragma DATA_ALIGN(rxDataEntryBuffer, 4)
static uint8_t rxDataEntryBuffer[RF_QUEUE_DATA_ENTRY_BUFFER_SIZE(NUM_DATA_ENTRIES, PAYLOAD_LENGTH, NUM_APPENDED_BYTES)];
static rfc_propRxOutput_t rxStatistics;
static RF_CmdHandle rxCmdHandle = RF_ALLOC_ERROR;
static uint8_t txPacket[PAYLOAD_LENGTH];
static dataQueue_t dataQueue;
static rfc_dataEntryGeneral_t* currentDataEntry;
static uint8_t packetLength;
static uint8_t* packetDataPointer;
static rfc_propRxOutput_t rxStatistics;

static Event_Struct radioEventStruct;
Event_Handle radioEventHandle;

mqd_t radioQueue;
static uint8_t txSequenceNumber = 0;

static uint8_t lastAckChecksum = 0;

char rxMsg[MAX_MSG_SIZE];
char txMsg[MAX_MSG_SIZE];

extern Event_Handle thEventHandle;     /* th_sensor_task.c */
#define EVENT_TH_FORCE   (1 << 1)      /* (test) immediate TH send */
#define EVENT_TH_JOIN    (1 << 2)      /* button -> send a JOIN request */

uint8_t calcChecksum(const char* msg, size_t len) {
    uint8_t crc = 0;
    size_t i;
    for (i = 0; i < len; i++) {
        crc ^= msg[i];
    }
    return crc;
}

void buttonCallback2(uint_least8_t index)
{
    /* Button = JOIN: the node announces itself for provisioning. */
    Event_post(thEventHandle, EVENT_TH_JOIN);
}

static void radioTaskFunction(UArg arg0, UArg arg1)
{
    Event_Params evParams;
    Event_Params_init(&evParams);
    Event_construct(&radioEventStruct, &evParams);
    radioEventHandle = (&radioEventStruct);

    RF_Params rfParams;
    RF_Params_init(&rfParams);

    if (RFQueue_defineQueue(&dataQueue, rxDataEntryBuffer, sizeof(rxDataEntryBuffer), NUM_DATA_ENTRIES, PAYLOAD_LENGTH + NUM_APPENDED_BYTES)) {
        Display_printf(display, 0, 0, "RFQueue_defineQueue failed");
        while (1);
    }

            RF_cmdPropTx.pktLen = PAYLOAD_LENGTH; // robimy p�niej
            RF_cmdPropTx.pPkt = txPacket; //robimy p�niej
            RF_cmdPropTx.startTrigger.triggerType = TRIG_ABSTIME;
            RF_cmdPropTx.startTrigger.pastTrig = 1;
            RF_cmdPropTx.startTime = 0;
            RF_cmdPropTx.pNextOp = (rfc_radioOp_t *)&RF_cmdPropRx;
            /* Only run the RX command if TX is successful */
            RF_cmdPropTx.condition.rule = COND_STOP_ON_FALSE;

            /* Set the Data Entity queue for received data */
            RF_cmdPropRx.pQueue = &dataQueue;
            /* Discard ignored packets from Rx queue */
            RF_cmdPropRx.rxConf.bAutoFlushIgnored = 1;
            /* Discard packets with CRC error from Rx queue */
            RF_cmdPropRx.rxConf.bAutoFlushCrcErr = 1;
            /* Implement packet length filtering to avoid PROP_ERROR_RXBUF */
            RF_cmdPropRx.maxPktLen = PAYLOAD_LENGTH;
            RF_cmdPropRx.pktConf.bRepeatOk = 0;
            RF_cmdPropRx.pktConf.bRepeatNok = 0;
            RF_cmdPropRx.pOutput = (uint8_t *)&rxStatistics;
            /* Receive operation will end RX_TIMEOUT ms after command starts */
            RF_cmdPropRx.endTrigger.triggerType = TRIG_REL_PREVEND;
            RF_cmdPropRx.endTime = RX_TIMEOUT;



    rfHandle = RF_open(&rfObject, &RF_prop, (RF_RadioSetup*)&RF_cmdPropRadioDivSetup, &rfParams);
    rxCmdHandle = RF_postCmd(rfHandle, (RF_Op*)&RF_cmdFs, RF_PriorityNormal, NULL, 0);

    while (1) {
        RF_cmdPropRx.endTrigger.triggerType = TRIG_NEVER;
        rxCmdHandle = RF_postCmd(rfHandle, (RF_Op*)&RF_cmdPropRx, RF_PriorityNormal, echoCallback, RF_EventRxEntryDone);

        uint32_t events = Event_pend(radioEventHandle, 0, EVENT_SEND_PACKET | EVENT_RADIO_RX, BIOS_WAIT_FOREVER);

        if (events & EVENT_SEND_PACKET) {
            if (rxCmdHandle != RF_ALLOC_ERROR) {
                RF_cancelCmd(rfHandle, rxCmdHandle, 0);
                RF_pendCmd(rfHandle, rxCmdHandle, RF_EventCmdCancelled);
                rxCmdHandle = RF_ALLOC_ERROR;
            }

            MessageStruct tempMsg;
            while (mq_receive(radioQueue, (char *) &tempMsg, MAX_MSG_SIZE, NULL) != -1)
            {
                char message[MAX_MSG_SIZE];
                message[0] = CONCENTRATOR_ADDRESS;
                message[1] = 'D';
                message[2] = tempMsg.id;   /* src = our address from the message (0xFF for JOIN, assigned addr otherwise) */

                memcpy(&message[3], &tempMsg, tempMsg.length);
                txSequenceNumber = txSequenceNumber + 1;
                message[tempMsg.length + 3] = txSequenceNumber;
                message[tempMsg.length + 4] = calcChecksum(&message[2], tempMsg.length + 2);


                uint8_t destAdress = message[0];
                uint8_t expectedCRC = message[tempMsg.length + 4];
                bool acked = false;

                int retry;
                for (retry = 0; retry <= MAX_RETRIES; retry++)
                {
                    //RF_cmdPropTx.pktLen = strlen(message);
                    RF_cmdPropTx.pktLen = tempMsg.length + 5;
                    RF_cmdPropTx.pPkt = (uint8_t*)message;
                    RF_cmdPropTx.startTrigger.triggerType = TRIG_NOW;
                    RF_cmdPropTx.startTrigger.pastTrig = 1;
                    RF_cmdPropTx.condition.rule = COND_STOP_ON_FALSE;
                    RF_cmdPropRx.endTrigger.triggerType = TRIG_REL_PREVEND;

                    rxCmdHandle = RF_postCmd(rfHandle, (RF_Op*)&RF_cmdPropTx, RF_PriorityNormal, echoCallbackACK, RF_EventRxEntryDone);
                    events = Event_pend(radioEventHandle, 0, EVENT_ACK_SENT, ACK_TIMEOUT_TICKS);

                    currentDataEntry = RFQueue_getDataEntry();

                    while (currentDataEntry->status == DATA_ENTRY_FINISHED)
                    {
                        // Dane gotowe � przetwarzamy
                        packetLength = *(uint8_t*)&(currentDataEntry->data);
                        packetDataPointer = (uint8_t*)&(currentDataEntry->data) + 1;
                        memcpy(rxMsg, packetDataPointer, packetLength);
                        rxMsg[packetLength] = '\0';
                        //Display_printf(display, 0, 0, "[Node RF] Received ACK: %02x|%c|%02x|%c%c%c%c%c%c%c%c%c%c%c%c|%d", rxMsg[0], rxMsg[1], rxMsg[2], rxMsg[3], rxMsg[4], rxMsg[5], rxMsg[6],rxMsg[7], rxMsg[8], rxMsg[9], rxMsg[10],rxMsg[11], rxMsg[12], rxMsg[13], rxMsg[14], rxMsg[packetLength-1]);
                        if (rxMsg[0] == NODE_ADDRESS || rxMsg[0] == 0xFF) //nasz adres albo JOIN (unprovisioned)
                        {
                            if (rxMsg[1] == 'A' & rxMsg[2] == destAdress & rxMsg[3] == expectedCRC)
                            {
                                acked = true;
                            }
                        }
                        RFQueue_nextEntry(); // udost�pniamy ten bufor RF core
                        currentDataEntry = RFQueue_getDataEntry(); // kolejny wpis
                    }
                    size_t tempStrLen = tempMsg.length-4;
                    char tempStr[MAX_MSG_SIZE];
                    memcpy(tempStr, &tempMsg.payload.textData.text[0], tempStrLen);
                    tempStr[tempStrLen] = '\0';
                    if (!acked)
                    {
                        Display_printf(display, 0, 0, "[Node RF] Sent Message Not ACKED: %02x|%c|%02x|%s|%d|%d", message[0], message[1], message[2], tempStr, message[tempMsg.length + 3], message[tempMsg.length + 4]);

                    }
                    else
                    {
                        Display_printf(display, 0, 0, "[Node RF] Sent Message ACKED: %02x|%c|%02x|%s|%d|%d", message[0], message[1], message[2], tempStr, message[tempMsg.length + 3], message[tempMsg.length + 4]);
                        Display_printf(display, 0, 0, "[Node RF] Received ACK: %02x|%c|%02x|%d", rxMsg[0], rxMsg[1], rxMsg[2], rxMsg[3]);
                        break;
                    }
                }
            }
        } else if (events & EVENT_RADIO_RX) {
            currentDataEntry = RFQueue_getDataEntry();

            while (currentDataEntry->status == DATA_ENTRY_FINISHED)
            {
                // Dane gotowe � przetwarzamy

                packetLength = *(uint8_t*)&(currentDataEntry->data);
                packetDataPointer = (uint8_t*)&(currentDataEntry->data) + 1;
                memcpy(rxMsg, packetDataPointer, packetLength);
                rxMsg[packetLength] = '\0';

                if (rxMsg[0] == NODE_ADDRESS || rxMsg[0] == 0xFF) //nasz adres albo JOIN (unprovisioned)
                {
                    //Display_printf(display, 0, 0, "[Node RF] Received data: %02x|%c|%02x|%c%c%c%c%c%c%c%c%c%c%c%c|%d(%d)", rxMsg[0], rxMsg[1], rxMsg[2], rxMsg[3], rxMsg[4], rxMsg[5], rxMsg[6],rxMsg[7], rxMsg[8], rxMsg[9], rxMsg[10],rxMsg[11], rxMsg[12], rxMsg[13], rxMsg[14], rxMsg[packetLength-1], calcChecksum(&rxMsg[2], packetLength - 3));
                    if (rxMsg[1] == 'D' & rxMsg[packetLength-1] == calcChecksum(&rxMsg[2], packetLength - 3))
                    {
                        txMsg[0] = rxMsg[2];
                        txMsg[1] = 'A';
                        txMsg[2] = rxMsg[0];
                        txMsg[3] = rxMsg[packetLength-1];
                        txMsg[4] = '\0';
                        RF_cmdPropTx.pktLen = 4;    //Mo�na u�y� strlen(txMsg) ale tak jest szybciej
                        RF_cmdPropTx.pPkt = (uint8_t*)txMsg;
                       // RF_cmdPropTx.startTrigger.triggerType = TRIG_NOW;
                       // RF_cmdPropTx.startTrigger.pastTrig = 1;
                        RF_cmdPropTx.startTrigger.triggerType = TRIG_NOW;
                        RF_cmdPropTx.startTrigger.pastTrig = 1;
                        RF_cmdPropTx.startTime = 0;
                        RF_cmdPropTx.condition.rule = COND_NEVER;

                        rxCmdHandle = RF_postCmd(rfHandle, (RF_Op*)&RF_cmdPropTx, RF_PriorityNormal, echoCallbackACK, RF_EventCmdDone);
                        events = Event_pend(radioEventHandle, 0, EVENT_ACK_SENT, ACK_TIMEOUT_TICKS);

                        //size_t tempStrLen = strlen(&rxMsg[3])-2;
                        //char tempStr[MAX_MSG_SIZE];
                        //memcpy(tempStr, &rxMsg[3], tempStrLen);
                        //tempStr[tempStrLen] = '\0';

                        /* Phase 0: the T/H node has no command handler. We still
                         * ACK the gateway (TX posted above) but drop the payload. */
                        if (events & EVENT_ACK_SENT)
                            Display_printf(display, 0, 0, "[Node RF] Gateway cmd received, ACKed (ignored)");
                        else
                            Display_printf(display, 0, 0, "[Node RF] Gateway cmd received, ACK failed");
                    }
                }
                RFQueue_nextEntry(); // udost�pniamy ten bufor RF core
                currentDataEntry = RFQueue_getDataEntry(); // kolejny wpis
            }


        }
    }

}

static void echoCallback(RF_Handle h, RF_CmdHandle ch, RF_EventMask e)
{
    if (e & RF_EventRxEntryDone) {
        //currentDataEntry = RFQueue_getDataEntry();
        //packetLength = *(uint8_t *)(&(currentDataEntry->data));
        //packetDataPointer = (uint8_t *)(&(currentDataEntry->data) + 1);

        //memcpy(txPacket, packetDataPointer, packetLength);
        //RFQueue_nextEntry();
        Event_post(radioEventHandle, EVENT_RADIO_RX);
    }
}

static void echoCallbackACK(RF_Handle h, RF_CmdHandle ch, RF_EventMask e)
{
    if (e & e & RF_EventLastCmdDone) {
        //currentDataEntry = RFQueue_getDataEntry();
        //packetLength = *(uint8_t *)(&(currentDataEntry->data));
       // packetDataPointer = (uint8_t *)(&(currentDataEntry->data) + 1);
       // memcpy(txPacket, packetDataPointer, packetLength);
       // RFQueue_nextEntry();

        Event_post(radioEventHandle, EVENT_ACK_SENT);
    }
}
void radioTaskInit()
{
    struct mq_attr qattr;
    qattr.mq_flags = 0;
    qattr.mq_maxmsg = 10;
    //qattr.mq_msgsize = sizeof(MessageStruct);//MAX_MSG_SIZE;
    qattr.mq_msgsize = MAX_MSG_SIZE;
    qattr.mq_curmsgs = 0;

    radioQueue = mq_open(RADIO_QUEUE_NAME, O_CREAT | O_RDWR | O_NONBLOCK, 0, &qattr);

    if (radioQueue == (mqd_t)-1) {
        Display_printf(display, 0, 0, "[Radio Task] Failed to open message queue\n");
    }

    GPIO_setConfig(Board_GPIO_BUTTON1, GPIO_CFG_IN_PU | GPIO_CFG_IN_INT_FALLING);
    GPIO_setCallback(Board_GPIO_BUTTON1, buttonCallback2);
    GPIO_enableInt(Board_GPIO_BUTTON1);

    Task_Params params;
    Task_Params_init(&params);
    params.stackSize = RADIO_TASK_STACK_SIZE;
    params.stack = &radioTaskStack;
    params.priority = RADIO_TASK_PRIORITY;

    Task_construct(&radioTaskStruct, radioTaskFunction, &params, NULL);
}
