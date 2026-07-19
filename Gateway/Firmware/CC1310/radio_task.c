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

#define RADIO_TASK_STACK_SIZE 2048
#define RADIO_TASK_PRIORITY   2
#define RADIO_QUEUE_NAME      "/radioQueue"
#define MAX_MSG_SIZE          70
#define EVENT_SEND_PACKET     (1 << 0)
#define EVENT_RADIO_RX        (1 << 1)
#define EVENT_ACK_SENT        (1 << 2)
#define SEND_DATA_TO_SLAVE    (1 << 3)
#define EVENT_RESTART_RADIO   (1 << 4)
#define CONCENTRATOR_ADDRESS  0x00   /* gen2 gateway = 0x00 (gen1 stays 0xF0); the RX filter below drops gen1's 0xF0 traffic so the two networks coexist during dev */

/* Passive sniffing of gen1 (temporary, until the solar/bufor nodes are reflashed
 * to target gen2). Frames addressed to gen1 are collected for telemetry but are
 * NEVER ACKed: that conversation belongs to gen1 and a second ACK would collide
 * with gen1's, breaking a working installation. RX is filtered in software (the
 * radio hands us every frame), so listening in costs nothing on air. */
#define GEN1_CONCENTRATOR_ADDRESS  0xF0
#define NODE_ADDRESS          0xF1
#define NUM_DATA_ENTRIES      2
#define NUM_APPENDED_BYTES    2
#define ACK_IDENTIFIER        "ACK:"
#define MAX_RETRIES            2
#define ACK_TIMEOUT_TICKS     (11 * (4000000 / 1000)) //11ms
#define RX_TIMEOUT            (10 * (4000000 / 1000)) //10ms
#define RETRY_TIMEOUT         (100 * (100000 / 1000)) //100ms dla clocka systemowego (100 kHz)

#define RX_CB_EVENTS ( RF_EventRxEntryDone       \
                     | RF_EventRxCollisionDetected  \
                     | RF_EventInternalError    \
                     | RF_EventRxAborted    \
                     | RF_EventRxNOk )

Clock_Handle timeoutClock;

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

/* NodeFrame: chip identity + message. Mirrors Shared/Protocol/node_protocol.h
 * (factory_id[8] + MessageStruct[44] = 52 B) so the SPI payload matches the M4F/Go
 * side. factory_id all-zero => legacy/unknown -> the RF frame uses tag 'D' (no
 * factory_id); a set factory_id -> tag 'E' (carries it). See Docs/NODE-MANAGEMENT §12.2. */
typedef struct {
    uint8_t       factory_id[8];
    MessageStruct message;
} NodeFrame;

typedef struct {
    NodeFrame frame;
    uint8_t   retryCounter;
} RadioMessageStruct;



static Task_Struct radioTaskStruct;
static uint8_t radioTaskStack[RADIO_TASK_STACK_SIZE];

extern Display_Handle display;

#define PAYLOAD_LENGTH         64   /* was 50; gen2 'E' frames add factory_id[8] -> solar telemetry is 57 B */
#define TX_DELAY             (uint32_t)(4000000*0.1f)
#define NUM_DATA_ENTRIES       2
#define NUM_APPENDED_BYTES     2

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

static Event_Struct radioEventStruct;
Event_Handle radioEventHandle;
extern Event_Handle spiMasterEventHandle;
mqd_t radioQueue;
extern mqd_t spiQueue;
static uint8_t lastChecksum = 0;
static uint8_t txSequenceNumber = 0;
static volatile uint32_t lastButtonTick = 0;
static bool acked = false;

char rxMsg[MAX_MSG_SIZE];
char txMsg[MAX_MSG_SIZE];

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
        uint32_t now = Clock_getTicks();
        if (now - lastButtonTick < Clock_tickPeriod * 2000)  // min 600ms odst�p (debounce)
               return;
        lastButtonTick = now;
    //    MessageStruct txtmsg;
    //    txtmsg.id = NODE_ADDRESS;
    //    txtmsg.type = SOLAR_CONTROLLER;
   //     txtmsg.cmd = SEND_TEXT_MSG;
    //    snprintf(txtmsg.payload.textData.text, sizeof(txtmsg.payload.textData.text), "Hello from Concentrator");
   //     txtmsg.length = sizeof(txtmsg.payload.textData) + 4;
   //     mq_send(radioQueue, (char *) &txtmsg, txtmsg.length, 0);
   //     Event_post(radioEventHandle, EVENT_SEND_PACKET);
}
void timeoutClockFxn(UArg arg)
{
    Event_post(radioEventHandle, EVENT_SEND_PACKET);
}

static void radioTaskFunction(UArg arg0, UArg arg1)
{
    Event_Params evParams;
        Event_Params_init(&evParams);
        Event_construct(&radioEventStruct, &evParams);
        radioEventHandle = Event_handle(&radioEventStruct);

        Clock_Params clkParams;
        Clock_Params_init(&clkParams);
        clkParams.startFlag = FALSE; // r�cznie wystartujesz p�niej
        timeoutClock = Clock_create(timeoutClockFxn, RETRY_TIMEOUT, &clkParams, NULL);

        RF_Params rfParams;
        RF_Params_init(&rfParams);

        if (RFQueue_defineQueue(&dataQueue, rxDataEntryBuffer, sizeof(rxDataEntryBuffer), NUM_DATA_ENTRIES, PAYLOAD_LENGTH + NUM_APPENDED_BYTES)) {
            Display_printf(display, 0, 0, "RFQueue_defineQueue failed");
            while (1);
        }

        /* Modify CMD_PROP_TX and CMD_PROP_RX commands for application needs */
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
        //RF_cmdPropRx.endTrigger.triggerType = TRIG_NEVER;
        RF_cmdPropRx.endTime = RX_TIMEOUT;


        rfHandle = RF_open(&rfObject, &RF_prop, (RF_RadioSetup*)&RF_cmdPropRadioDivSetup, &rfParams);
        rxCmdHandle = RF_runCmd(rfHandle, (RF_Op*)&RF_cmdFs, RF_PriorityNormal, NULL, 0);

        while (1) {
            RF_cmdPropRx.endTrigger.triggerType = TRIG_NEVER;
          //  if (RF_getCmdStatus(rfHandle, rxCmdHandle) != RF_StatCmdStarted) {
         //       rxCmdHandle = RF_postCmd(rfHandle, (RF_Op*)&RF_cmdPropRx, RF_PriorityNormal, echoCallback, RF_EventRxEntryDone); // RX nieaktywne, mo�na wystartowa� ponownie
         //   }
         //   if (rxCmdHandle == RF_ALLOC_ERROR) {
          //      rxCmdHandle = RF_postCmd(rfHandle, (RF_Op*)&RF_cmdPropRx, RF_PriorityNormal, echoCallback, RF_EventRxEntryDone);
         //   }

         //       if (rxCmdHandle != RF_ALLOC_ERROR) {
         //           RF_cancelCmd(rfHandle, rxCmdHandle, 0);
         //           RF_pendCmd(rfHandle, rxCmdHandle, RF_EventCmdCancelled);
         //           rxCmdHandle = RF_ALLOC_ERROR;
         //       }
            rxCmdHandle = RF_postCmd(rfHandle, (RF_Op*)&RF_cmdPropRx, RF_PriorityNormal, echoCallback, RF_EventRxEntryDone | RF_EventRxNOk);

            uint32_t events = Event_pend(radioEventHandle, 0, EVENT_SEND_PACKET | EVENT_RADIO_RX | EVENT_RESTART_RADIO, BIOS_WAIT_FOREVER);

            if (events & EVENT_SEND_PACKET) {
                if (rxCmdHandle != RF_ALLOC_ERROR) {
                    RF_cancelCmd(rfHandle, rxCmdHandle, 0);
                    RF_pendCmd(rfHandle, rxCmdHandle, RF_EventCmdCancelled);
                    rxCmdHandle = RF_ALLOC_ERROR;
                    }
                //MessageStruct tempMsg;
                RadioMessageStruct job;
                //while (mq_receive(radioQueue, (char *) &tempMsg, MAX_MSG_SIZE, NULL) != -1)
                while (mq_receive(radioQueue, (char *) &job, MAX_MSG_SIZE, NULL) != -1)
                {
                    MessageStruct *tempMsg = &job.frame.message;
                    char frame[MAX_MSG_SIZE];
                    uint8_t k, hasFactory = 0, hdr, seqIdx, crcIdx;
                    for (k = 0; k < 8; k++) { if (job.frame.factory_id[k]) { hasFactory = 1; break; } }
                    frame[0] = tempMsg->id;   /* dest = node address from the message (multi-node + JOIN_ACCEPT to 0xFF) */
                    frame[2] = CONCENTRATOR_ADDRESS;   /* src = gateway 0x00 */
                    if (hasFactory) {
                        frame[1] = 'E';   /* v2: [dest][E][src][factory_id:8][msg][seq][crc] */
                        memcpy(&frame[3], job.frame.factory_id, 8);
                        hdr = 11;
                    } else {
                        frame[1] = 'D';   /* legacy: [dest][D][src][msg][seq][crc] */
                        hdr = 3;
                    }
                    memcpy(&frame[hdr], tempMsg, tempMsg->length);
                    seqIdx = hdr + tempMsg->length;
                    crcIdx = seqIdx + 1;
                    txSequenceNumber = txSequenceNumber + 1;
                    frame[seqIdx] = txSequenceNumber;
                    /* checksum over src + (factory_id) + msg + seq = (hdr-2)+length+1 bytes */
                    frame[crcIdx] = calcChecksum(&frame[2], (hdr - 2) + tempMsg->length + 1);

                    uint8_t destAdress = frame[0];
                    uint8_t expectedCRC = frame[crcIdx];
                    bool acked = false;

                    RF_cmdPropTx.pktLen = crcIdx + 1;
                    RF_cmdPropTx.pPkt = (uint8_t*)frame;
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
                        if (rxMsg[0] == CONCENTRATOR_ADDRESS)
                        {
                            if (rxMsg[1] == 'A' && rxMsg[2] == destAdress && rxMsg[3] == expectedCRC)
                            {
                                acked = true;
                            }
                        }
                        RFQueue_nextEntry(); // udost�pniamy ten bufor RF core
                        currentDataEntry = RFQueue_getDataEntry(); // kolejny wpis
                    }
                 //   size_t tempStrLen = tempMsg.length-4;
                 //   char tempStr[MAX_MSG_SIZE];
                 //   memcpy(tempStr, &tempMsg.payload.textData.text[0], tempStrLen);
                 //   tempStr[tempStrLen] = '\0';
                    if (!acked)
                    {

                            Display_printf(display, 0, 0, "[Gateway RF TX] Sent Message Not ACKED: %02x|%d|%d", frame[0], frame[seqIdx], frame[crcIdx]);
                            if (job.retryCounter <= MAX_RETRIES)
                            {
                                job.retryCounter = job.retryCounter + 1;
                                mq_send(radioQueue, (char *)&job, sizeof(job), 0);
                                Clock_start(timeoutClock);
                                break;
                            }
                        }
                        else
                        {
                            Display_printf(display, 0, 0, "[Gateway RF TX] Sent Message ACKED: %02x|%d|%d", frame[0], frame[seqIdx], frame[crcIdx]);
                            Display_printf(display, 0, 0, "[Gateway RF TX] Received ACK: %02x|%c|%02x|%d", rxMsg[0], rxMsg[1], rxMsg[2], rxMsg[3]);

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
                    int8_t lastPktRssi =rxStatistics.lastRssi;
                    /* Accept our own traffic (0x00) AND sniff gen1's (0xF0). Only the
                     * former gets an ACK - see GEN1_CONCENTRATOR_ADDRESS. */
                    bool addressedToUs = (rxMsg[0] == CONCENTRATOR_ADDRESS);
                    bool sniffGen1     = (rxMsg[0] == GEN1_CONCENTRATOR_ADDRESS);
                    if (addressedToUs || sniffGen1)
                    {
                        if ((rxMsg[1] == 'D' || rxMsg[1] == 'E') && rxMsg[packetLength-1] == calcChecksum(&rxMsg[2], packetLength - 3))
                        {
                          if (addressedToUs)
                          {
                            txMsg[0] = rxMsg[2];
                            txMsg[1] = 'A';
                            txMsg[2] = rxMsg[0];
                            txMsg[3] = rxMsg[packetLength-1];
                            txMsg[4] = '\0';
                            RF_cmdPropTx.pktLen = 4;    //Mo�na u�y� strlen(txMsg) ale tak jest szybciej
                            RF_cmdPropTx.pPkt = (uint8_t*)txMsg;
                            RF_cmdPropTx.startTrigger.triggerType = TRIG_NOW;
                            RF_cmdPropTx.startTrigger.pastTrig = 1;
                            RF_cmdPropTx.startTime = 0;
                            RF_cmdPropTx.condition.rule = COND_NEVER;
                            //RF_cmdPropTx.condition.rule = COND_ALWAYS;
                            //RF_cmdPropRx.endTrigger.triggerType = TRIG_NEVER;
                            rxCmdHandle = RF_postCmd(rfHandle, (RF_Op*)&RF_cmdPropTx, RF_PriorityNormal, echoCallbackACK, RF_EventCmdDone);
                            events = Event_pend(radioEventHandle, 0, EVENT_ACK_SENT, ACK_TIMEOUT_TICKS);
                          }
                          else
                          {
                            /* sniffed gen1 frame: stay off the air, let gen1 ACK it */
                            events = 0;
                          }

                         //   size_t tempStrLen = strlen(&rxMsg[3])-2;
                         //   char tempStr[MAX_MSG_SIZE];
                         //   memcpy(tempStr, &rxMsg[3], tempStrLen);
                         //   tempStr[tempStrLen] = '\0';
                            //size_t tempStrLen = strlen(&message[3])-2;
                            /* Extract the identity-tagged frame: 'E' carries factory_id
                             * (msg at offset 11), 'D' is legacy (msg at offset 3, factory_id 0). */
                            NodeFrame nf;
                            memset(&nf, 0, sizeof(nf));
                            if (rxMsg[1] == 'E' && packetLength >= 13) {
                                memcpy(nf.factory_id, &rxMsg[3], 8);
                                memcpy(&nf.message, &rxMsg[11], packetLength - 13);
                            } else {
                                memcpy(&nf.message, &rxMsg[3], packetLength - 5);
                            }
                            mq_send(spiQueue, (char *) &nf, sizeof(nf), 0);
                            Event_post(spiMasterEventHandle, SEND_DATA_TO_SLAVE);

                            if (sniffGen1)
                            {
                                Display_printf(display, 0, 0, "[Gateway RF RX] SNIFF gen1 (no ACK): src %02x|seq %d|RSSI: %d", rxMsg[2], rxMsg[packetLength-2], lastPktRssi);
                            }
                            else if (events & EVENT_ACK_SENT)
                            {
                                Display_printf(display, 0, 0, "[Gateway RF TX] Received data from Node: %02x|%d|%d|RSSI: %d", rxMsg[2], rxMsg[packetLength-2], rxMsg[packetLength-1], lastPktRssi);
                                Display_printf(display, 0, 0, "[Gateway RF TX] Successfully send ACK");

                            }
                            else
                            {
                                Display_printf(display, 0, 0, "[Gateway RF TX] Received data from Node: %02x|%s|%d|%d", rxMsg[2], rxMsg[packetLength-2], rxMsg[packetLength-1]);
                                Display_printf(display, 0, 0, "[Gateway RF TX] Failed to send ACK");
                            }
                        }
                    }
                    RFQueue_nextEntry(); // udost�pniamy ten bufor RF core
                    currentDataEntry = RFQueue_getDataEntry(); // kolejny wpis
                }


            } else if (events & EVENT_RESTART_RADIO) {
                Display_printf(display, 0, 0, "Kolizja trzeba restartowa� radio!!!");
            }
        }

}

static void echoCallback(RF_Handle h, RF_CmdHandle ch, RF_EventMask e)
{
    if (e & RF_EventRxEntryDone) {
        //currentDataEntry = RFQueue_getDataEntry();
        //packetLength = *(uint8_t *)(&(currentDataEntry->data));
       // packetDataPointer = (uint8_t *)(&(currentDataEntry->data) + 1);
       // memcpy(txPacket, packetDataPointer, packetLength);
       // RFQueue_nextEntry();

        Event_post(radioEventHandle, EVENT_RADIO_RX);
    }
    else if (e & RF_EventRxNOk)
    {
        Event_post(radioEventHandle, EVENT_RESTART_RADIO);
    }
}
static void echoCallbackACK(RF_Handle h, RF_CmdHandle ch, RF_EventMask e)
{
    if (e & RF_EventLastCmdDone) {
        //currentDataEntry = RFQueue_getDataEntry();
        //packetLength = *(uint8_t *)(&(currentDataEntry->data));
       // packetDataPointer = (uint8_t *)(&(currentDataEntry->data) + 1);
       // memcpy(txPacket, packetDataPointer, packetLength);
       // RFQueue_nextEntry();

        Event_post(radioEventHandle, EVENT_ACK_SENT);
    }
}

void radioEnqueueMessage(const char* msg)
{
    if (mq_send(radioQueue, msg, strlen(msg) + 1, 0) == 0) {
        Event_post(radioEventHandle, EVENT_SEND_PACKET);
    }
}

void radioTaskInit()
{
    struct mq_attr qattr;
    qattr.mq_flags = 0;
    qattr.mq_maxmsg = 10;
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
