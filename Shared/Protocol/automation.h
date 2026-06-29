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

/* Devices (condition sources / action targets) */
#define DEV_SOLAR_CONTROLLER   0u
#define DEV_BUFFER_CONTROLLER  1u
#define DEV_SMARTPHONE         2u

/* Parameters (sensor values) */
#define PARAM_T1         0u
#define PARAM_T2         1u
#define PARAM_T3         2u
#define PARAM_T4         3u
#define PARAM_SBUF_TEMP  4u
#define PARAM_UNKNOWN    5u

/* Condition types */
#define COND_TIME             0u
#define COND_PARAMETER        1u
#define COND_PARAMETER_DELTA  2u

/* Comparison operators */
#define OP_LESS_THAN  0u
#define OP_MORE_THAN  1u
#define OP_BETWEEN    2u

/* Action types */
#define ACTION_SET_RELAY     0u
#define ACTION_SEND_MESSAGE  1u

/* Relay values */
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
    uint8_t device;     /* DEV_*   */
    uint8_t parameter;  /* PARAM_* */
    uint8_t op;         /* OP_*    */
    uint8_t _pad;       /* align floats to 4 */
    float   thresholdMin;  /* MORE_THAN / BETWEEN */
    float   thresholdMax;  /* LESS_THAN / BETWEEN */
} ParameterCondition;

typedef struct {
    uint8_t device1;
    uint8_t parameter1;
    uint8_t device2;
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
    uint8_t target;      /* DEV_* (target node) */
    uint8_t _pad[2];
    union {
        struct {
            uint8_t value;   /* VALUE_* */
        } relay;
        struct {
            char message[MAX_MESSAGE_LEN];
        } sendMessage;
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
