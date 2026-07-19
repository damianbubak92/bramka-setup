/**
 * engine_rpmsg.h - RPMsg glue between the Linux Go service and the engine.
 *
 * Decodes the gen2 control messages (MSG_RULE_*, MSG_NODE_CMD) into engine
 * calls, and provides the M4F->Linux reporters (NODE_TELEMETRY/STATE/RULE_FIRED).
 *
 * Decoupled from the concrete transmit code (ipc_rpmsg_echo.c) via function
 * pointers so the engine glue stays testable and the verified RPMsg stack is
 * untouched. See docs/ENGINE-INTEGRATION.md.
 */
#ifndef ENGINE_RPMSG_H
#define ENGINE_RPMSG_H

#include <stdint.h>
#include <stdbool.h>
#include "automation.h"
#include "node_protocol.h"

/* Reliable M4F->Linux send (seq owned by the impl; ACK+retry tracked).
 * Maps to ipc_rpmsg_echo.c sendReliable(). Returns 0 on success. */
typedef int (*RpmsgEventTxFn)(uint8_t msg_type, const uint8_t *payload,
                              uint16_t payload_len);

/* Send a bare reply (no payload) echoing a received seq: MSG_ACK on success or
 * MSG_ERROR on rejection. Fire-and-forget (no retry). Maps to sendReply(). */
typedef void (*RpmsgReplyFn)(uint8_t msg_type, uint16_t seq);

/* Deliver a command to a node (over SPI to CC1310). The NodeFrame carries the
 * target chip's factory_id alongside the message; a zero factory_id => the CC1310
 * sends a legacy 'D' frame (no identity), otherwise a v2 'E' frame. */
typedef void (*NodeTxFn)(const NodeFrame *frame);

void engine_rpmsg_init(RpmsgEventTxFn tx_event,
                       RpmsgReplyFn   reply,
                       NodeTxFn       tx_node);

/* Handle a gen2 control message (0x30..0x34). Returns true if consumed.
 * Replies MSG_ACK (echoing seq) on success, or MSG_ERROR (echoing seq) on
 * rejection so Linux can correlate immediately and resync. Call from the
 * RPMsg dispatch switch. */
bool engine_rpmsg_dispatch(uint8_t msg_type, uint16_t seq,
                           const uint8_t *payload, uint16_t payload_len);

/* M4F->Linux reporters (reliable EVENT-style path). */
void engine_rpmsg_report_telemetry(const NodeFrame *frame);       /* MSG_NODE_TELEMETRY (factory_id + msg) */
void engine_rpmsg_report_state(void);                             /* MSG_NODE_STATE */
void engine_rpmsg_report_rule_fired(uint16_t ruleIndex,
                                    const RuleAction *action);     /* MSG_RULE_FIRED */

#endif /* ENGINE_RPMSG_H */
