/**
 * automation.h - Automation rule model (shared M4F C / Go cgo).
 *
 * Ported from gen1 automationRules.h. The ENGINE runs on M4F (RTOS); Linux owns
 * JSON + SQLite and pushes rules to M4F as binary AutomationRule[] over RPMsg
 * (MSG_RULE_BEGIN/ITEM/COMMIT). See docs/ARCHITECTURE-GEN2.md.
 *
 * WIRE COMPATIBILITY (M4F ARM32 <-> A53 AArch64, both little-endian):
 *   - Enums are #define constants, never struct fields (TI clang -fshort-enums vs
 *     gcc int would mismatch). Fields that hold a code are uint8_t.
 *   - Explicit _pad keeps floats 4-aligned and the layout deterministic across
 *     both ABIs (no reliance on compiler-inserted padding).
 *   - Values are identical to gen1 (semantic parity).
 *
 * AutomationRule is M4F<->Linux only (never goes to CC1310).
 */
#ifndef AUTOMATION_H
#define AUTOMATION_H

#include <stdint.h>

/* ========================================================================= *
 * LIMITS
 * ========================================================================= */
#define MAX_RULES          100u  /* gen1 was 5 (FRAM-limited); M4F RAM allows this */
#define MAX_RULE_NAME_LEN   64u
#define MAX_MESSAGE_LEN     64u
#define MAX_CONDITIONS       3u  /* up to 3 conditions ANDed per rule */

/* ========================================================================= *
 * CODES (wire-stable values; stored in uint8_t fields, NOT as C enums)
 * ========================================================================= */

/* Node references. A condition source / action target is an RF node ADDRESS
 * (0x10-0xEF), NOT a device-type code. The app + DB + Go key rules by the stable
 * node_id; Go resolves node_id -> the node's current address at push time, so the
 * M4F engine works purely in address space (per-node telemetry keyed by address,
 * actions routed to an address). address 0 in an action target = "no node" (e.g.
 * ACTION_SEND_MESSAGE). (gen1's DEV_SOLAR/BUFFER/SMARTPHONE codes are gone.) */

/* Parameters (condition sources) - full telemetry set + room for future node types.
 * The engine reads the addressed node's field by this code; the app offers only the
 * params valid for the selected node's type. */
#define PARAM_T1          0u
#define PARAM_T2          1u
#define PARAM_T3          2u
#define PARAM_T4          3u
#define PARAM_SBUF_TEMP   4u
#define PARAM_TCOL        5u
#define PARAM_ENERGY_GAIN 6u
#define PARAM_FLOW_RATE   7u
#define PARAM_PUMP_STATE  8u
#define PARAM_TEMPERATURE 9u   /* TH sensor */
#define PARAM_HUMIDITY    10u  /* TH sensor */
#define PARAM_BATT_MV     11u
#define PARAM_UNKNOWN     255u

/* Condition types */
#define COND_TIME             0u
#define COND_PARAMETER        1u
#define COND_PARAMETER_DELTA  2u

/* Comparison operators */
#define OP_LESS_THAN  0u
#define OP_MORE_THAN  1u
#define OP_BETWEEN    2u

/* Action types - EXTENSIBLE. A node-executable action's code doubles as its bit
 * index in node.capabilities (a node declares which it supports at JOIN, see
 * node_protocol.h joinData.capabilities). SEND_MESSAGE is a SYSTEM action (app
 * notification, no node) and is never a node capability bit. Adding an action =
 * new ACTION_* code + engine dispatch case + app catalog entry; no struct change. */
#define ACTION_SET_RELAY     0u  /* node action: relay/pump on-off; value 0/1        */
#define ACTION_SEND_MESSAGE  1u  /* system action: app notification; nodeAddr unused */
/* future node actions, declared via NODE_CAP(ACTION_*):
 *   #define ACTION_SET_PUMP_SPEED 2u   // value 0-100 (%) */

/* Node capability bit for a node-executable action code. */
#define NODE_CAP(action)  (1u << (action))

/* Relay values (SET_RELAY value is a float; these are the 0/1 semantics). */
#define VALUE_OFF  0u
#define VALUE_ON   1u

/* ========================================================================= *
 * CONDITIONS
 * ========================================================================= */

typedef struct {
    uint8_t hourStart;
    uint8_t minStart;
    uint8_t hourEnd;
    uint8_t minEnd;
} TimeCondition;

typedef struct {
    uint8_t nodeAddr;   /* RF address of the source node (Go resolves from node_id) */
    uint8_t parameter;  /* PARAM_* */
    uint8_t op;         /* OP_*    */
    uint8_t _pad;       /* align floats to 4 */
    float   thresholdMin;  /* MORE_THAN / BETWEEN */
    float   thresholdMax;  /* LESS_THAN / BETWEEN */
} ParameterCondition;

typedef struct {
    uint8_t nodeAddr1;
    uint8_t parameter1;
    uint8_t nodeAddr2;
    uint8_t parameter2;
    uint8_t deltaOp;    /* OP_* on (value1 - value2) */
    uint8_t _pad[3];    /* align floats to 4 */
    float   deltaMin;
    float   deltaMax;
} ParameterDeltaCondition;

typedef struct {
    uint8_t conditionType;  /* COND_* */
    uint8_t _pad[3];
    union {
        TimeCondition           time;
        ParameterCondition      parameter;
        ParameterDeltaCondition parameterDelta;
    } u;
} RuleCondition;

/* ========================================================================= *
 * ACTION
 * ========================================================================= */
typedef struct {
    uint8_t actionType;  /* ACTION_* */
    uint8_t nodeAddr;    /* target node RF address; 0 = none (e.g. SEND_MESSAGE) */
    uint8_t _pad[2];
    union {
        float value;                    /* node actions: SET_RELAY 0/1, SET_PUMP_SPEED 0-100, ... */
        char  message[MAX_MESSAGE_LEN]; /* SEND_MESSAGE */
    } data;
} RuleAction;

/* ========================================================================= *
 * FULL RULE
 * ========================================================================= */
typedef struct {
    char          name[MAX_RULE_NAME_LEN];
    uint8_t       conditionCount;       /* number of active conditions (<= MAX_CONDITIONS) */
    uint8_t       _pad[3];
    RuleCondition conditions[MAX_CONDITIONS];
    RuleAction    action;
} AutomationRule;

#endif /* AUTOMATION_H */
