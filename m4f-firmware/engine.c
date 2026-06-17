/**
 * engine.c - Automation engine core (M4F). Ported ~1:1 from gen1
 * (automationRules.c + coreTask.c folding). RTOS-agnostic - see engine.h.
 */
#include "engine.h"
#include <string.h>

/* ========================================================================= *
 * STATE
 * ========================================================================= */

/* Authoritative live snapshot. gen1 init: temps -20, sBuforTemp -20, rest 0.
 * The negative sBuforTemp seeds the "buffer not reporting yet" guard below. */
static NodesData g_nodes = {
    .Tin = -20.0f, .Tout = -20.0f, .T4 = -20.0f, .T3 = -20.0f,
    .T2 = -20.0f, .T1 = -20.0f, .Tcol = -20.0f,
    .energyGain = 0, .flowRate = 0, .sBuforTemp = -20.0f,
    .pumpState = 0, ._pad = {0, 0, 0},
};

static EngineTime g_time = { 0, 0, false };

static EngineActionFn g_action_fn  = NULL;
static void          *g_action_ctx = NULL;

/* Double-buffered ruleset. g_active selects the live set; the other is the
 * shadow under construction. Swapping g_active is the atomic commit.
 *
 * This is large (2 * MAX_RULES * sizeof(AutomationRule) ~= 39 KB at MAX_RULES
 * =100). The M4F internal DRAM is only 64 KB, so it is placed in a dedicated
 * section the linker maps to DDR (see linker.cmd: .bss.engine_rules > M4F_DDR,
 * and docs/ENGINE-INTEGRATION.md). Uninitialized/NOLOAD is fine: rules are
 * written before activation and g_count starts at 0, so no slot is read until a
 * successful commit has filled it. (The section attribute is ignored by host
 * compilers if engine.c is ever built off-target.) */
static AutomationRule g_rules[2][MAX_RULES]
    __attribute__((section(".bss.engine_rules")));
static uint16_t       g_count[2]  = { 0, 0 };
static volatile uint8_t g_active  = 0;

/* Shadow-build state */
static bool     g_building       = false;
static uint8_t  g_shadow         = 1;     /* index being filled (= !g_active) */
static uint16_t g_shadow_expected = 0;
static uint16_t g_shadow_filled   = 0;

/* ========================================================================= *
 * CRC32 (IEEE, poly 0xEDB88320) - matches Go hash/crc32 (ChecksumIEEE).
 * crc32 of zero bytes == 0x00000000.
 * ========================================================================= */
static uint32_t crc32_calc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int b;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ========================================================================= *
 * LIFECYCLE
 * ========================================================================= */
void engine_init(EngineActionFn action_fn, void *action_ctx)
{
    g_action_fn  = action_fn;
    g_action_ctx = action_ctx;

    g_active = 0;
    g_count[0] = 0;
    g_count[1] = 0;
    g_building = false;
    g_shadow_expected = 0;
    g_shadow_filled = 0;
}

/* ========================================================================= *
 * LIVE STATE - gen1 coreTask folding, ported
 * ========================================================================= */
bool engine_update_node(const MessageStruct *msg)
{
    if (msg == NULL) {
        return false;
    }

    if (msg->type == NODE_SOLAR_CONTROLLER && msg->cmd == CMD_SEND_DATA_TO_DB) {
        g_nodes.Tin        = msg->payload.solarData.Tin;
        g_nodes.Tout       = msg->payload.solarData.Tout;
        g_nodes.T4         = msg->payload.solarData.T4;
        g_nodes.T3         = msg->payload.solarData.T3;
        g_nodes.T2         = msg->payload.solarData.T2;
        g_nodes.T1         = msg->payload.solarData.T1;
        g_nodes.Tcol       = msg->payload.solarData.Tcol;
        g_nodes.energyGain = msg->payload.solarData.energyGain;
        g_nodes.flowRate   = msg->payload.solarData.flowRate;
        g_nodes.pumpState  = msg->payload.solarData.pumpState;
        return true;
    }
    if (msg->type == NODE_SOLAR_CONTROLLER && msg->cmd == CMD_SEND_PUMP_STATUS) {
        g_nodes.pumpState = msg->payload.pumpData.pumpState;
        return true;
    }
    if (msg->type == NODE_BUFOR_CONTROLLER) {
        g_nodes.sBuforTemp = msg->payload.buforData.sBuforTemp;
        return true;
    }
    return false;
}

const NodesData *engine_get_state(void)
{
    return &g_nodes;
}

void engine_set_time(uint8_t hour, uint8_t minute)
{
    g_time.hour = hour;
    g_time.minute = minute;
    g_time.valid = true;
}

void engine_clear_time(void)
{
    g_time.valid = false;
}

/* ========================================================================= *
 * EVALUATOR - port of gen1 evaluateAutomationRules()
 * ========================================================================= */

/* gen1 getDeviceParameterValue(): ignores device, switches on parameter.
 * D6 parity. (TODO: honor `device` once the node model is generalized.) */
static float getDeviceParameterValue(uint8_t device, uint8_t parameter)
{
    (void)device;
    switch (parameter) {
        case PARAM_T1: return g_nodes.T1;
        case PARAM_T2: return g_nodes.T2;
        case PARAM_T3: return g_nodes.T3;
        case PARAM_T4: return g_nodes.T4;
        default:       return g_nodes.sBuforTemp;  /* PARAM_SBUF_TEMP / unknown */
    }
}

static bool evaluateTimeCondition(const TimeCondition *tc)
{
    bool afterStart, beforeEnd;

    if (!g_time.valid) {
        return false;  /* fail-safe: no clock => TIME never matches */
    }
    /* Range assumed not to cross midnight (gen1 behavior). */
    afterStart = (g_time.hour > tc->hourStart) ||
                 (g_time.hour == tc->hourStart && g_time.minute >= tc->minStart);
    beforeEnd  = (g_time.hour < tc->hourEnd) ||
                 (g_time.hour == tc->hourEnd && g_time.minute <= tc->minEnd);
    return afterStart && beforeEnd;
}

static bool evaluateParameterCondition(const ParameterCondition *pc, float value)
{
    switch (pc->op) {
        case OP_LESS_THAN: return value < pc->thresholdMax;
        case OP_MORE_THAN: return value > pc->thresholdMin;
        case OP_BETWEEN:   return (value >= pc->thresholdMin) &&
                                  (value <= pc->thresholdMax);
        default:           return false;
    }
}

static bool evaluateParameterDeltaCondition(const ParameterDeltaCondition *pdc,
                                            float v1, float v2)
{
    float delta = v1 - v2;
    switch (pdc->deltaOp) {
        case OP_LESS_THAN: return delta < pdc->deltaMax;
        case OP_MORE_THAN: return delta > pdc->deltaMin;
        case OP_BETWEEN:   return (delta >= pdc->deltaMin) &&
                                  (delta <= pdc->deltaMax);
        default:           return false;
    }
}

/* gen1 had separate DeviceType / TargetNodeType / nodeType enums; the action
 * target (DEV_*) must map to a wire node type (NODE_*). */
static uint8_t targetToNodeType(uint8_t target)
{
    switch (target) {
        case DEV_SOLAR_CONTROLLER:  return NODE_SOLAR_CONTROLLER;
        case DEV_BUFFER_CONTROLLER: return NODE_BUFOR_CONTROLLER;
        case DEV_SMARTPHONE:        return NODE_SMARTPHONE;
        default:                    return NODE_SMARTPHONE;
    }
}

void engine_evaluate(void)
{
    uint8_t a = g_active;
    AutomationRule *rules = g_rules[a];
    uint16_t n = g_count[a];
    uint16_t i;
    int c;

    for (i = 0; i < n; i++) {
        AutomationRule *rule = &rules[i];
        bool allConditionsMet = true;

        /* gen1 parity guards (solar relay): skip if pump already in the wanted
         * state (dedup), or if the buffer sensor hasn't reported yet (<0). */
        if (rule->action.target == DEV_SOLAR_CONTROLLER &&
            rule->action.actionType == ACTION_SET_RELAY &&
            rule->action.data.relay.value == g_nodes.pumpState) {
            continue;
        }
        if (rule->action.target == DEV_SOLAR_CONTROLLER &&
            rule->action.actionType == ACTION_SET_RELAY &&
            g_nodes.sBuforTemp < 0) {
            continue;
        }

        for (c = 0; c < rule->conditionCount; c++) {
            RuleCondition *rc = &rule->conditions[c];
            bool cond_ok = false;

            switch (rc->conditionType) {
                case COND_TIME:
                    cond_ok = evaluateTimeCondition(&rc->u.time);
                    break;
                case COND_PARAMETER: {
                    ParameterCondition *pc = &rc->u.parameter;
                    float val = getDeviceParameterValue(pc->device, pc->parameter);
                    cond_ok = evaluateParameterCondition(pc, val);
                    break;
                }
                case COND_PARAMETER_DELTA: {
                    ParameterDeltaCondition *pdc = &rc->u.parameterDelta;
                    float v1 = getDeviceParameterValue(pdc->device1, pdc->parameter1);
                    float v2 = getDeviceParameterValue(pdc->device2, pdc->parameter2);
                    cond_ok = evaluateParameterDeltaCondition(pdc, v1, v2);
                    break;
                }
                default:
                    cond_ok = false;
                    break;
            }

            if (!cond_ok) {
                allConditionsMet = false;
                break;  /* AND: one failed => rule not met */
            }
        }

        if (!allConditionsMet) {
            continue;
        }

        /* All conditions met -> build the action message (gen1 mapping). */
        MessageStruct msg;
        memset(&msg, 0, sizeof(msg));
        msg.id = 0xF1;
        msg.type = targetToNodeType(rule->action.target);

        if (rule->action.actionType == ACTION_SET_RELAY) {
            msg.cmd = CMD_TURN_PUMP_ON_OFF;
            msg.payload.pumpData.pumpState =
                (rule->action.data.relay.value == VALUE_ON) ? 1u : 0u;
            msg.length = (uint8_t)(sizeof(msg.payload.pumpData) + 4u);
        } else if (rule->action.actionType == ACTION_SEND_MESSAGE) {
            msg.cmd = CMD_SEND_TEXT_MSG;
            strncpy(msg.payload.textData.text,
                    rule->action.data.sendMessage.message,
                    sizeof(msg.payload.textData.text) - 1);
            msg.payload.textData.text[sizeof(msg.payload.textData.text) - 1] = '\0';
            msg.length = (uint8_t)(4u + strlen(msg.payload.textData.text) + 1u);
        } else {
            continue;  /* unknown action type */
        }

        if (g_action_fn != NULL) {
            g_action_fn(&msg, i, &rule->action, g_action_ctx);
        }
    }
}

/* ========================================================================= *
 * RULESET UPLOAD - atomic swap
 * ========================================================================= */
bool engine_rules_begin(uint16_t count)
{
    if (count > MAX_RULES) {
        return false;
    }
    g_shadow = (uint8_t)(g_active ^ 1u);
    g_shadow_expected = count;
    g_shadow_filled = 0;
    g_building = true;
    return true;
}

bool engine_rules_item(uint16_t index, const AutomationRule *rule)
{
    if (!g_building || rule == NULL) {
        return false;
    }
    if (index >= g_shadow_expected) {
        return false;
    }
    memcpy(&g_rules[g_shadow][index], rule, sizeof(AutomationRule));
    g_shadow_filled++;
    return true;
}

bool engine_rules_commit(uint16_t expectedCount, uint32_t crc32)
{
    uint32_t calc;

    if (!g_building) {
        return false;
    }
    g_building = false;  /* this attempt is now resolved either way */

    if (expectedCount != g_shadow_expected ||
        g_shadow_filled != expectedCount) {
        return false;  /* count mismatch -> keep active set */
    }

    calc = crc32_calc((const uint8_t *)g_rules[g_shadow],
                      (size_t)expectedCount * sizeof(AutomationRule));
    if (calc != crc32) {
        return false;  /* corrupt -> keep active set */
    }

    g_count[g_shadow] = expectedCount;
    g_active = g_shadow;  /* atomic swap */
    return true;
}

uint16_t engine_rules_active_count(void)
{
    return g_count[g_active];
}
