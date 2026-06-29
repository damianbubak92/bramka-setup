/**
 * engine.h - Automation engine core (M4F).
 *
 * RTOS-AGNOSTIC by design: no SDK / FreeRTOS / RPMsg / SPI dependencies. This is
 * the ported gen1 evaluator (automationRules.c) plus the gen1 coreTask data
 * folding (NodesData) and the gen2 atomic ruleset swap (replaces FRAM dual-slot).
 *
 * The engine does NO I/O. Rule firings and node commands leave through caller-
 * supplied callbacks (parity with gen1's spiTaskSend() decoupling). This keeps
 * the engine portable and unit-testable, and lets it run unchanged from either
 * the current NoRTOS cooperative loop or a future FreeRTOS task (D1).
 *
 * Wiring: see engine_rpmsg.h (RPMsg glue) and docs/ENGINE-INTEGRATION.md.
 */
#ifndef ENGINE_H
#define ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "automation.h"
#include "node_protocol.h"

/* ========================================================================= *
 * TIME (for CONDITION_TIME)
 *
 * M4F has no NTP. Wall-clock is injected by the integration (Linux time-sync
 * over RPMsg, or a carrier RTC - TBD, see ARCHITECTURE-GEN2.md). Until a valid
 * time is set, TIME conditions evaluate FALSE (fail-safe).
 *
 * The platform also injects a monotonic microsecond clock (EngineClockFn at
 * init). The engine advances the synced wall-time by the monotonic delta, so
 * COND_TIME stays accurate between syncs and the minute tick can align to :00.
 * ========================================================================= */
typedef struct {
    uint8_t hour;     /* 0..23 */
    uint8_t minute;   /* 0..59 */
    uint8_t second;   /* 0..59 */
    bool    valid;
} EngineTime;

/* Monotonic microsecond clock supplied by the platform (e.g. ClockP_getTimeUsec
 * on the M4F). Kept as a callback so engine.c stays SDK/RTOS-agnostic. */
typedef uint64_t (*EngineClockFn)(void);

/* ========================================================================= *
 * ACTION SINK
 *
 * Called once per rule whose conditions are all met. `msg` is the MessageStruct
 * destined for the target node (delivered over SPI to CC1310 - later roadmap).
 * `ruleIndex` is the active-set index that fired; `action` is the source action
 * (for the MSG_RULE_FIRED audit trail). The engine performs no I/O itself.
 * ========================================================================= */
typedef void (*EngineActionFn)(const MessageStruct *msg,
                               uint16_t ruleIndex,
                               const RuleAction *action,
                               void *ctx);

/* ========================================================================= *
 * LIFECYCLE
 * ========================================================================= */
void engine_init(EngineActionFn action_fn, void *action_ctx,
                 EngineClockFn clock_fn);

/* ========================================================================= *
 * LIVE STATE (NodesData) - gen1 coreTask folding, ported
 *
 * engine_update_node(): fold one inbound node reading into NodesData.
 *   Returns true if the reading was recognized and applied (telemetry-worthy).
 * engine_get_state(): pointer to the authoritative NodesData snapshot
 *   (for MSG_NODE_STATE). Stable for the engine's lifetime.
 * ========================================================================= */
bool             engine_update_node(const MessageStruct *msg);
const NodesData *engine_get_state(void);

void engine_set_time(uint8_t hour, uint8_t minute, uint8_t second);  /* mark time valid */
void engine_clear_time(void);                                        /* mark time invalid */

/* ========================================================================= *
 * EVALUATION (hybrid cadence)
 *
 * Rules are bucketed by their conditions so each trigger only touches the rules
 * it can affect (no wasted CPU):
 *   ENGINE_EVAL_TIME - call on the wall-clock minute tick; evaluates rules that
 *                      carry a COND_TIME (and zero-condition "always" rules).
 *   ENGINE_EVAL_NODE - call on node-data arrival; evaluates rules that carry a
 *                      COND_PARAMETER / COND_PARAMETER_DELTA.
 * A rule with both time and data conditions is evaluated in BOTH passes.
 *
 * Firing is LEVEL-triggered: while a rule's conditions hold it fires every pass.
 * Spam is suppressed by node-state feedback (the action is skipped once the node
 * already reports the desired state - see the solar pumpState guard), not by an
 * edge latch, so a lost first command is retried until the node confirms.
 * ========================================================================= */
typedef enum {
    ENGINE_EVAL_TIME = 0,  /* minute tick: COND_TIME (and zero-condition) rules */
    ENGINE_EVAL_NODE = 1,  /* node-data arrival: COND_PARAMETER(_DELTA) rules */
} EngineEvalScope;

void engine_evaluate(EngineEvalScope scope);

/* Milliseconds (1..60000) until the next wall-clock minute boundary - used to
 * align the time tick to :00. Sub-second precision on purpose: integer-second
 * math jitters right at the boundary (a tick landing a hair early reads
 * second=59 -> a spurious 1s micro-sleep -> double fire). Returns 60000 when no
 * valid wall-clock is set. */
uint32_t engine_ms_to_next_minute(void);

/* ========================================================================= *
 * RULESET UPLOAD - atomic swap (fed by engine_rpmsg)
 *
 * Build a shadow set, then commit atomically. A rejected/partial commit leaves
 * the active set untouched (gen1 FRAM dual-slot reliability pattern, D9).
 *
 * begin(count):   start a shadow build (count <= MAX_RULES). Returns false if
 *                 count too large.
 * item(i, rule):  store rule at shadow index i (0..count-1). Returns false if
 *                 no build in progress or index out of range.
 * commit(n, crc): verify filled==n and CRC32 over the shadow rules == crc, then
 *                 swap active<-shadow. Returns false (and keeps the old set) on
 *                 any mismatch. crc is the IEEE CRC32 of the n AutomationRule
 *                 structs back-to-back, in index order (matches Go hash/crc32).
 * ========================================================================= */
bool     engine_rules_begin(uint16_t count);
bool     engine_rules_item(uint16_t index, const AutomationRule *rule);
bool     engine_rules_commit(uint16_t expectedCount, uint32_t crc32);
uint16_t engine_rules_active_count(void);

#endif /* ENGINE_H */
