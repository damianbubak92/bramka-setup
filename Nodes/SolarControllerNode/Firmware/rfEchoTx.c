#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
#include "node_identity.h"   /* gFactoryId, gNodeAddress, ADDR_*, CMD_*, RF_FRAME_TAG_* */

#define RADIO_TASK_STACK_SIZE 1536
#define RADIO_TASK_PRIORITY   2
#define RADIO_QUEUE_NAME      "/radioQueue"
#define MAX_MSG_SIZE          70
#define EVENT_SEND_PACKET     (1 << 0)
#define EVENT_RADIO_RX        (1 << 1)
#define EVENT_ACK_SENT        (1 << 2)
#define CONCENTRATOR_ADDRESS  ADDR_GATEWAY   /* gen2 gateway = 0x00 (was gen1 0xF0) */
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


static Task_Struct radioTaskStruct;
static uint8_t radioTaskStack[RADIO_TASK_STACK_SIZE];

extern Display_Handle display;

#define PAYLOAD_LENGTH         64   /* was 50; gen2 'E' frames add factory_id[8] (solar telemetry 57 B) */
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

extern Event_Handle solarControllerEventHandle;
extern mqd_t solarControllerQueue;

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
    /* JOIN: announce this chip to the gateway (src 0xFF, factory_id in payload). The
     * gateway surfaces it for user approval, then JOIN_ACCEPTs an assigned address. */
    MessageStruct joinMsg;
    (void)index;
    memset(&joinMsg, 0, sizeof(joinMsg));
    joinMsg.id   = ADDR_UNPROVISIONED;   /* src 0xFF - no address yet */
    joinMsg.type = SOLAR_CONTROLLER;
    joinMsg.cmd  = CMD_JOIN_REQUEST;
    memcpy(joinMsg.payload.joinData.factory_id, gFactoryId, NODE_FACTORY_ID_LEN);
    joinMsg.payload.joinData.capabilities = NODE_CAPABILITIES;   /* declared to the gateway */
    joinMsg.length = sizeof(joinMsg.payload.joinData) + 4;       /* now 12 + 4 = 16 */
    mq_send(radioQueue, (char *) &joinMsg, joinMsg.length, 0);
    Event_post(radioEventHandle, EVENT_SEND_PACKET);
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
    /* Task context (after BIOS_start): safe to use the blocking UART display. */
    {
        char fidStr[2 * NODE_FACTORY_ID_LEN + 1];
        int fi;
        for (fi = 0; fi < (int)NODE_FACTORY_ID_LEN; fi++) {
            snprintf(&fidStr[fi * 2], 3, "%02x", gFactoryId[fi]);
        }
        Display_printf(display, 0, 0, "[Node RF] up. factory %s, address 0x%02x%s",
            fidStr, gNodeAddress,
            (gNodeAddress == ADDR_UNPROVISIONED) ? " (unprovisioned - press JOIN)" : "");
    }

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
                uint8_t seqIdx, crcIdx;
                message[0] = CONCENTRATOR_ADDRESS;      /* dest = gateway 0x00 */
                message[1] = RF_FRAME_TAG_V2;           /* 'E' - carries factory_id */
                message[2] = tempMsg.id;                /* src = our addr (0xFF on JOIN) */
                memcpy(&message[3], gFactoryId, NODE_FACTORY_ID_LEN);
                memcpy(&message[11], &tempMsg, tempMsg.length);
                seqIdx = 11 + tempMsg.length;
                crcIdx = seqIdx + 1;
                txSequenceNumber = txSequenceNumber + 1;
                message[seqIdx] = txSequenceNumber;
                /* checksum over src + factory_id(8) + msg + seq = length + 10 */
                message[crcIdx] = calcChecksum(&message[2], tempMsg.length + 10);

                uint8_t destAdress = message[0];
                uint8_t expectedCRC = message[crcIdx];
                bool acked = false;

                int retry;
                for (retry = 0; retry <= MAX_RETRIES; retry++)
                {
                    RF_cmdPropTx.pktLen = crcIdx + 1;
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
                        if (rxMsg[0] == tempMsg.id) /* ACK addressed back to our src addr */
                        {
                            if (rxMsg[1] == 'A' && rxMsg[2] == destAdress && rxMsg[3] == expectedCRC)
                            {
                                acked = true;
                            }
                        }
                        RFQueue_nextEntry(); // udost�pniamy ten bufor RF core
                        currentDataEntry = RFQueue_getDataEntry(); // kolejny wpis
                    }
                    if (!acked)
                    {
                        Display_printf(display, 0, 0, "[Node RF] TX not ACKED: dst %02x tag %c src %02x cmd %d seq %d",
                                       message[0], message[1], message[2], tempMsg.cmd, message[seqIdx]);
                    }
                    else
                    {
                        Display_printf(display, 0, 0, "[Node RF] TX ACKED: dst %02x src %02x cmd %d seq %d",
                                       message[0], message[2], tempMsg.cmd, message[seqIdx]);
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

                /* Accept frames to our current address; 0xFF (unprovisioned) lets a
                 * JOIN_ACCEPT through. */
                if (rxMsg[0] == gNodeAddress)
                {
                    uint8_t tag = rxMsg[1];
                    if ((tag == RF_FRAME_TAG_LEGACY || tag == RF_FRAME_TAG_V2) &&
                        rxMsg[packetLength-1] == calcChecksum(&rxMsg[2], packetLength - 3))
                    {
                        /* v2 'E' carries the target factory_id at [3]; act only if it is
                         * ours (kills a stale chip acting on a reused address). Legacy 'D'
                         * (no id) is accepted for rollout back-compat. msg starts at [11]
                         * for 'E', [3] for 'D'. */
                        bool forUs = true;
                        MessageStruct receivedMessage;
                        memset(&receivedMessage, 0, sizeof(receivedMessage));
                        if (tag == RF_FRAME_TAG_V2)
                        {
                            forUs = factory_is_mine((const uint8_t *)&rxMsg[3]);
                            if (forUs && packetLength >= 13)
                            {
                                memcpy(&receivedMessage, &rxMsg[11], packetLength - 13);
                            }
                        }
                        else
                        {
                            memcpy(&receivedMessage, &rxMsg[3], packetLength - 5);
                        }

                        if (forUs)
                        {
                            /* ACK back to the gateway (dst = its src, our addr as source). */
                            txMsg[0] = rxMsg[2];
                            txMsg[1] = 'A';
                            txMsg[2] = rxMsg[0];
                            txMsg[3] = rxMsg[packetLength-1];
                            txMsg[4] = '\0';
                            RF_cmdPropTx.pktLen = 4;
                            RF_cmdPropTx.pPkt = (uint8_t*)txMsg;
                            RF_cmdPropTx.startTrigger.triggerType = TRIG_NOW;
                            RF_cmdPropTx.startTrigger.pastTrig = 1;
                            RF_cmdPropTx.startTime = 0;
                            RF_cmdPropTx.condition.rule = COND_NEVER;

                            rxCmdHandle = RF_postCmd(rfHandle, (RF_Op*)&RF_cmdPropTx, RF_PriorityNormal, echoCallbackACK, RF_EventCmdDone);
                            events = Event_pend(radioEventHandle, 0, EVENT_ACK_SENT, ACK_TIMEOUT_TICKS);

                            mq_send(solarControllerQueue, (char *)&receivedMessage, receivedMessage.length, 0);
                            Event_post(solarControllerEventHandle, EVENT_RECEIVE_CMD);

                            Display_printf(display, 0, 0, "[Node RF] RX from GW: src %02x tag %c cmd %d %s",
                                           rxMsg[2], tag, receivedMessage.cmd,
                                           (events & EVENT_ACK_SENT) ? "(ACKed)" : "(ACK fail)");
                        }
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

    /* Identity: read the FCFG factory id + load the persisted address (0xFF = new).
     * Do NOT Display_printf here: radioTaskInit runs in main() BEFORE BIOS_start(),
     * where the blocking UART display would pend on a semaphore with no scheduler to
     * release it -> partial output then hang. The identity is logged from
     * radioTaskFunction (task context) instead. */
    identity_init();

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
