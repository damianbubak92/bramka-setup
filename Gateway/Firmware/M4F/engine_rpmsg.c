/**
 * engine_rpmsg.c - RPMsg glue for the automation engine. See engine_rpmsg.h.
 */
#include "engine_rpmsg.h"
#include "engine.h"
#include "protocol.h"
#include <string.h>

static RpmsgEventTxFn g_tx_event = NULL;
static RpmsgReplyFn   g_reply    = NULL;
static NodeTxFn       g_tx_node  = NULL;

void engine_rpmsg_init(RpmsgEventTxFn tx_event,
                       RpmsgReplyFn   reply,
                       NodeTxFn       tx_node)
{
    g_tx_event = tx_event;
    g_reply    = reply;
    g_tx_node  = tx_node;
}

static void ackIf(uint16_t seq)
{
    if (g_reply != NULL) {
        g_reply(MSG_ACK, seq);
    }
}

/* Reject the message identified by seq (echo MSG_ERROR with the same seq). */
static void sendError(uint16_t seq)
{
    if (g_reply != NULL) {
        g_reply(MSG_ERROR, seq);
    }
}

/* ========================================================================= *
 * INBOUND: Linux -> M4F (0x30..0x33)
 * ========================================================================= */
bool engine_rpmsg_dispatch(uint8_t msg_type, uint16_t seq,
                           const uint8_t *payload, uint16_t payload_len)
{
    switch (msg_type) {

        case MSG_RULE_BEGIN: {
            /* payload: u16 ruleCount (BE) */
            if (payload_len < 2u) { sendError(seq); return true; }
            uint16_t count = protocol_get_u16_be(payload);
            if (engine_rules_begin(count)) {
                ackIf(seq);
            } else {
                sendError(seq);  /* count > MAX_RULES */
            }
            return true;
        }

        case MSG_RULE_ITEM: {
            /* payload: u16 index (BE) + AutomationRule */
            if (payload_len != (uint16_t)(2u + sizeof(AutomationRule))) {
                sendError(seq);
                return true;
            }
            uint16_t index = protocol_get_u16_be(payload);
            AutomationRule rule;
            memcpy(&rule, payload + 2, sizeof(AutomationRule));
            if (engine_rules_item(index, &rule)) {
                ackIf(seq);
            } else {
                sendError(seq);
            }
            return true;
        }

        case MSG_RULE_COMMIT: {
            /* payload: u16 expectedCount (BE) + u32 crc32 (BE) */
            if (payload_len != 6u) { sendError(seq); return true; }
            uint16_t expected = protocol_get_u16_be(payload);
            uint32_t crc = ((uint32_t)payload[2] << 24) |
                           ((uint32_t)payload[3] << 16) |
                           ((uint32_t)payload[4] << 8)  |
                           ((uint32_t)payload[5]);
            if (engine_rules_commit(expected, crc)) {
                ackIf(seq);  /* swap done -> ACK */
            } else {
                sendError(seq); /* rejected -> old set kept, Linux should resync */
            }
            return true;
        }

        case MSG_NODE_CMD: {
            /* payload: MessageStruct - relay a phone command to a node */
            if (payload_len != (uint16_t)sizeof(MessageStruct)) {
                sendError(seq);
                return true;
            }
            MessageStruct msg;
            memcpy(&msg, payload, sizeof(MessageStruct));
            if (g_tx_node != NULL) {
                g_tx_node(&msg);
            }
            ackIf(seq);
            return true;
        }

        case MSG_TIME_SYNC: {
            /* payload: u8 hour, u8 minute, [u8 second] - sets the engine
             * wall-clock so COND_TIME rules evaluate and the minute tick aligns
             * to :00 (M4F has no RTC/NTP). Seconds optional for back-compat. */
            if (payload_len < 2u) { sendError(seq); return true; }
            uint8_t sec = (payload_len >= 3u) ? payload[2] : 0u;
            engine_set_time(payload[0], payload[1], sec);
            ackIf(seq);
            return true;
        }

        default:
            return false;  /* not ours */
    }
}

/* ========================================================================= *
 * OUTBOUND: M4F -> Linux (reliable EVENT-style path)
 * ========================================================================= */
void engine_rpmsg_report_telemetry(const MessageStruct *msg)
{
    if (g_tx_event == NULL || msg == NULL) {
        return;
    }
    g_tx_event(MSG_NODE_TELEMETRY, (const uint8_t *)msg, (uint16_t)sizeof(*msg));
}

void engine_rpmsg_report_state(void)
{
    const NodesData *st;
    if (g_tx_event == NULL) {
        return;
    }
    st = engine_get_state();
    g_tx_event(MSG_NODE_STATE, (const uint8_t *)st, (uint16_t)sizeof(*st));
}

void engine_rpmsg_report_rule_fired(uint16_t ruleIndex, const RuleAction *action)
{
    /* payload: u16 ruleIndex (BE) + RuleAction */
    uint8_t buf[2 + sizeof(RuleAction)];
    if (g_tx_event == NULL || action == NULL) {
        return;
    }
    protocol_put_u16_be(buf, ruleIndex);
    memcpy(buf + 2, action, sizeof(RuleAction));
    g_tx_event(MSG_RULE_FIRED, buf, (uint16_t)sizeof(buf));
}
