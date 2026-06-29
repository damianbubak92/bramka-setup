/*
 * radio.c - one-shot blocking RF send for the power-cycled T&H node.
 * Reuses the proven gen2 PHY (smartrf_settings) + frame format so the existing
 * gen2 gateway receives it unchanged. TX is chained to a short RX window to catch
 * the gateway ACK; retried a few times. No tasks/events - linear, then RF_close.
 */
#include "radio.h"

#include <string.h>
#include <ti/drivers/rf/RF.h>
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/rf_prop_mailbox.h)

#include "smartrf_settings/smartrf_settings.h"
#include "RFQueue.h"

#define MAX_RETRIES         3
#define PAYLOAD_LENGTH      50
#define NUM_DATA_ENTRIES    2
#define NUM_APPENDED_BYTES  2
/* RF core runs at 4 MHz -> 4 ticks/us. ACK window after TX. */
#define ACK_WINDOW_US       50000
#define ACK_WINDOW_TICKS    (ACK_WINDOW_US * 4)

static RF_Object rfObject;
static RF_Handle rfHandle;

#pragma DATA_ALIGN(rxDataEntryBuffer, 4)
static uint8_t rxDataEntryBuffer[RF_QUEUE_DATA_ENTRY_BUFFER_SIZE(
        NUM_DATA_ENTRIES, PAYLOAD_LENGTH, NUM_APPENDED_BYTES)];
static dataQueue_t dataQueue;
static rfc_dataEntryGeneral_t *currentDataEntry;
static rfc_propRxOutput_t rxStatistics;

static uint8_t txSeq = 0;

static uint8_t calcChecksum(const uint8_t *d, uint8_t len)
{
    uint8_t crc = 0, i;
    for (i = 0; i < len; i++) crc ^= d[i];
    return crc;
}

bool radio_send_message(const MessageStruct *msg, uint8_t destAddr)
{
    RF_Params rfParams;
    RF_Params_init(&rfParams);

    if (RFQueue_defineQueue(&dataQueue, rxDataEntryBuffer, sizeof(rxDataEntryBuffer),
                            NUM_DATA_ENTRIES, PAYLOAD_LENGTH + NUM_APPENDED_BYTES)) {
        return false;
    }

    /* RX (ACK window), chained after TX */
    RF_cmdPropRx.pQueue = &dataQueue;
    RF_cmdPropRx.rxConf.bAutoFlushIgnored = 1;
    RF_cmdPropRx.rxConf.bAutoFlushCrcErr  = 1;
    RF_cmdPropRx.maxPktLen = PAYLOAD_LENGTH;
    RF_cmdPropRx.pktConf.bRepeatOk  = 0;
    RF_cmdPropRx.pktConf.bRepeatNok = 0;
    RF_cmdPropRx.pOutput = (uint8_t *)&rxStatistics;
    RF_cmdPropRx.endTrigger.triggerType = TRIG_REL_PREVEND;
    RF_cmdPropRx.endTime = ACK_WINDOW_TICKS;

    RF_cmdPropTx.startTrigger.triggerType = TRIG_NOW;
    RF_cmdPropTx.startTrigger.pastTrig    = 1;
    RF_cmdPropTx.pNextOp   = (rfc_radioOp_t *)&RF_cmdPropRx;
    RF_cmdPropTx.condition.rule = COND_STOP_ON_FALSE;  /* RX only if TX ok */

    rfHandle = RF_open(&rfObject, &RF_prop, (RF_RadioSetup *)&RF_cmdPropRadioDivSetup, &rfParams);
    if (rfHandle == NULL) return false;
    RF_runCmd(rfHandle, (RF_Op *)&RF_cmdFs, RF_PriorityNormal, NULL, 0);

    /* Build frame: [dest]['D'][src][MessageStruct][seq][crc8] */
    uint8_t frame[3 + sizeof(MessageStruct) + 2];
    uint8_t len = msg->length;
    frame[0] = destAddr;
    frame[1] = 'D';
    frame[2] = msg->id;
    memcpy(&frame[3], msg, len);
    txSeq++;
    frame[len + 3] = txSeq;
    frame[len + 4] = calcChecksum(&frame[2], len + 2);
    uint8_t expectedCRC = frame[len + 4];

    bool acked = false;
    int retry;
    for (retry = 0; retry <= MAX_RETRIES && !acked; retry++) {
        RF_cmdPropTx.pPkt   = frame;
        RF_cmdPropTx.pktLen = len + 5;
        /* Blocks through TX and the chained RX (ACK) window. */
        RF_runCmd(rfHandle, (RF_Op *)&RF_cmdPropTx, RF_PriorityNormal, NULL, 0);

        currentDataEntry = RFQueue_getDataEntry();
        while (currentDataEntry->status == DATA_ENTRY_FINISHED) {
            uint8_t  plen  = *(uint8_t *)(&currentDataEntry->data);
            uint8_t *pdata = (uint8_t *)(&currentDataEntry->data) + 1;
            /* ACK: [our addr]['A'][dest we used][expectedCRC] */
            if (plen >= 4 && pdata[0] == msg->id && pdata[1] == 'A' &&
                pdata[2] == destAddr && pdata[3] == expectedCRC) {
                acked = true;
            }
            RFQueue_nextEntry();
            currentDataEntry = RFQueue_getDataEntry();
        }
    }

    RF_close(rfHandle);
    return acked;
}
