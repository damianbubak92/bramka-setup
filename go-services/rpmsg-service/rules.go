package main

/*
#include <stdlib.h>
#include <string.h>
#include "protocol.h"
#include "automation.h"

// Rule builders: fill a C AutomationRule from flat scalars. The C COMPILER owns
// the struct layout (same shared/automation.h as the M4F), so the bytes Go ships
// are byte-identical to what the M4F memcpy's into its struct - no chance of a
// Go-side offset/padding bug. Go then copies the raw struct bytes onto the wire.

static void rule_reset(AutomationRule *r, const char *name, uint8_t condCount) {
    memset(r, 0, sizeof(*r));
    strncpy(r->name, name, MAX_RULE_NAME_LEN - 1);
    r->conditionCount = condCount;
}
static void rule_set_time(AutomationRule *r, int i,
                          uint8_t hs, uint8_t ms, uint8_t he, uint8_t me) {
    RuleCondition *c = &r->conditions[i];
    c->conditionType = COND_TIME;
    c->u.time.hourStart = hs; c->u.time.minStart = ms;
    c->u.time.hourEnd   = he; c->u.time.minEnd   = me;
}
static void rule_set_param(AutomationRule *r, int i,
                           uint8_t dev, uint8_t par, uint8_t op, float mn, float mx) {
    RuleCondition *c = &r->conditions[i];
    c->conditionType = COND_PARAMETER;
    c->u.parameter.device = dev; c->u.parameter.parameter = par; c->u.parameter.op = op;
    c->u.parameter.thresholdMin = mn; c->u.parameter.thresholdMax = mx;
}
static void rule_set_delta(AutomationRule *r, int i,
                           uint8_t d1, uint8_t p1, uint8_t d2, uint8_t p2,
                           uint8_t op, float mn, float mx) {
    RuleCondition *c = &r->conditions[i];
    c->conditionType = COND_PARAMETER_DELTA;
    c->u.parameterDelta.device1 = d1; c->u.parameterDelta.parameter1 = p1;
    c->u.parameterDelta.device2 = d2; c->u.parameterDelta.parameter2 = p2;
    c->u.parameterDelta.deltaOp = op;
    c->u.parameterDelta.deltaMin = mn; c->u.parameterDelta.deltaMax = mx;
}
static void rule_set_relay(AutomationRule *r, uint8_t target, uint8_t value) {
    r->action.actionType = ACTION_SET_RELAY; r->action.target = target;
    r->action.data.relay.value = value;
}
static void rule_set_message(AutomationRule *r, uint8_t target, const char *msg) {
    r->action.actionType = ACTION_SEND_MESSAGE; r->action.target = target;
    strncpy(r->action.data.sendMessage.message, msg, MAX_MESSAGE_LEN - 1);
}
*/
import "C"

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"log"
	"unsafe"
)

// Wire codes - mirror shared/automation.h (#define values, stable).
const (
	DevSolarController  = 0
	DevBufferController = 1
	DevSmartphone       = 2

	ParamT1       = 0
	ParamT2       = 1
	ParamT3       = 2
	ParamT4       = 3
	ParamSbufTemp = 4

	CondTime           = 0
	CondParameter      = 1
	CondParameterDelta = 2

	OpLessThan = 0
	OpMoreThan = 1
	OpBetween  = 2

	ActionSetRelay    = 0
	ActionSendMessage = 1

	ValueOff = 0
	ValueOn  = 1

	MaxRules       = 100
	MaxConditions  = 3
	MaxRuleNameLen = 64 // incl. null terminator
	MaxMessageLen  = 64 // incl. null terminator
)

// abiOK is false if the C header layout drifts from what we expect; rule push
// is then refused (corruption is worse than a missing feature).
var abiOK = true

func init() {
	for _, c := range []struct {
		name      string
		got, want int
	}{
		{"AutomationRule", int(unsafe.Sizeof(C.AutomationRule{})), 196},
		{"RuleCondition", int(unsafe.Sizeof(C.RuleCondition{})), 20},
		{"RuleAction", int(unsafe.Sizeof(C.RuleAction{})), 68},
	} {
		if c.got != c.want {
			abiOK = false
			log.Printf("[rules] ABI MISMATCH %s: C header=%d, expected=%d - rule push DISABLED",
				c.name, c.got, c.want)
		}
	}
}

// --- High-level rule model (ergonomic; mirrors shared/automation.h) ---

type TimeCond struct{ HourStart, MinStart, HourEnd, MinEnd uint8 }

type ParamCond struct {
	Device, Parameter, Op uint8
	Min, Max              float32
}

type DeltaCond struct {
	Device1, Parameter1, Device2, Parameter2, Op uint8
	Min, Max                                     float32
}

type Condition struct {
	Type  uint8
	Time  TimeCond  // when Type == CondTime
	Param ParamCond // when Type == CondParameter
	Delta DeltaCond // when Type == CondParameterDelta
}

type Action struct {
	Type       uint8
	Target     uint8
	RelayValue uint8  // ACTION_SET_RELAY
	Message    string // ACTION_SEND_MESSAGE (<= 63 chars)
}

type Rule struct {
	Name       string
	Conditions []Condition
	Action     Action
}

// encodeRule serializes a Rule to the exact AutomationRule wire image (196 B)
// via the C builders, so the bytes match the M4F struct layout exactly.
func encodeRule(r Rule) ([]byte, error) {
	if len(r.Conditions) > MaxConditions {
		return nil, fmt.Errorf("too many conditions (%d, max %d)", len(r.Conditions), MaxConditions)
	}
	if len(r.Name) >= MaxRuleNameLen {
		return nil, fmt.Errorf("name too long (%d, max %d)", len(r.Name), MaxRuleNameLen-1)
	}

	var cr C.AutomationRule
	cname := C.CString(r.Name)
	defer C.free(unsafe.Pointer(cname))
	C.rule_reset(&cr, cname, C.uint8_t(len(r.Conditions)))

	for i, c := range r.Conditions {
		switch c.Type {
		case CondTime:
			C.rule_set_time(&cr, C.int(i),
				C.uint8_t(c.Time.HourStart), C.uint8_t(c.Time.MinStart),
				C.uint8_t(c.Time.HourEnd), C.uint8_t(c.Time.MinEnd))
		case CondParameter:
			C.rule_set_param(&cr, C.int(i),
				C.uint8_t(c.Param.Device), C.uint8_t(c.Param.Parameter), C.uint8_t(c.Param.Op),
				C.float(c.Param.Min), C.float(c.Param.Max))
		case CondParameterDelta:
			C.rule_set_delta(&cr, C.int(i),
				C.uint8_t(c.Delta.Device1), C.uint8_t(c.Delta.Parameter1),
				C.uint8_t(c.Delta.Device2), C.uint8_t(c.Delta.Parameter2),
				C.uint8_t(c.Delta.Op), C.float(c.Delta.Min), C.float(c.Delta.Max))
		default:
			return nil, fmt.Errorf("condition %d: unknown type %d", i, c.Type)
		}
	}

	switch r.Action.Type {
	case ActionSetRelay:
		C.rule_set_relay(&cr, C.uint8_t(r.Action.Target), C.uint8_t(r.Action.RelayValue))
	case ActionSendMessage:
		if len(r.Action.Message) >= MaxMessageLen {
			return nil, fmt.Errorf("message too long (%d, max %d)", len(r.Action.Message), MaxMessageLen-1)
		}
		cmsg := C.CString(r.Action.Message)
		defer C.free(unsafe.Pointer(cmsg))
		C.rule_set_message(&cr, C.uint8_t(r.Action.Target), cmsg)
	default:
		return nil, fmt.Errorf("unknown action type %d", r.Action.Type)
	}

	return C.GoBytes(unsafe.Pointer(&cr), C.int(unsafe.Sizeof(cr))), nil
}

// PushRules uploads a ruleset to the M4F: RULE_BEGIN -> RULE_ITEM* -> RULE_COMMIT.
// The M4F builds a shadow set and atomically swaps it on a valid COMMIT (count +
// CRC32 match); a rejected COMMIT (MSG_ERROR) leaves the old set active. crc32 is
// IEEE over the concatenated AutomationRule images in index order (matches the
// M4F crc32_calc over its shadow array).
func (p *Protocol) PushRules(rules []Rule) error {
	if !abiOK {
		return fmt.Errorf("rule push disabled: AutomationRule ABI mismatch (see startup log)")
	}
	if len(rules) > MaxRules {
		return fmt.Errorf("too many rules (%d, max %d)", len(rules), MaxRules)
	}

	encoded := make([][]byte, len(rules))
	h := crc32.NewIEEE()
	for i, r := range rules {
		b, err := encodeRule(r)
		if err != nil {
			return fmt.Errorf("rule[%d] %q: %w", i, r.Name, err)
		}
		encoded[i] = b
		h.Write(b)
	}
	crc := h.Sum32()

	begin := make([]byte, 2)
	binary.BigEndian.PutUint16(begin, uint16(len(rules)))
	if err := p.sendReliableTyped(C.MSG_RULE_BEGIN, begin); err != nil {
		return fmt.Errorf("RULE_BEGIN: %w", err)
	}

	for i, b := range encoded {
		item := make([]byte, 2+len(b))
		binary.BigEndian.PutUint16(item, uint16(i))
		copy(item[2:], b)
		if err := p.sendReliableTyped(C.MSG_RULE_ITEM, item); err != nil {
			return fmt.Errorf("RULE_ITEM[%d]: %w", i, err)
		}
	}

	commit := make([]byte, 6)
	binary.BigEndian.PutUint16(commit, uint16(len(rules)))
	binary.BigEndian.PutUint32(commit[2:], crc)
	if err := p.sendReliableTyped(C.MSG_RULE_COMMIT, commit); err != nil {
		return fmt.Errorf("RULE_COMMIT (swap rejected?): %w", err)
	}

	log.Printf("[rules] pushed %d rules (crc32=0x%08X) - M4F committed", len(rules), crc)
	return nil
}

// exampleRules mirrors gen1 initExampleRules() (coreTask.c) for the push test.
func exampleRules() []Rule {
	return []Rule{
		{
			Name:       "PumpControl",
			Conditions: []Condition{{Type: CondTime, Time: TimeCond{HourStart: 0, MinStart: 7, HourEnd: 0, MinEnd: 10}}},
			Action:     Action{Type: ActionSetRelay, Target: DevSolarController, RelayValue: ValueOn},
		},
		{
			Name: "DeltaTempAlert",
			Conditions: []Condition{{Type: CondParameterDelta, Delta: DeltaCond{
				Device1: DevSolarController, Parameter1: ParamT1,
				Device2: DevBufferController, Parameter2: ParamSbufTemp,
				Op: OpMoreThan, Min: 5.0,
			}}},
			Action: Action{Type: ActionSendMessage, Target: DevSmartphone, Message: "ALERT"},
		},
		{
			Name:       "PumpControl2",
			Conditions: []Condition{{Type: CondTime, Time: TimeCond{HourStart: 0, MinStart: 11, HourEnd: 0, MinEnd: 30}}},
			Action:     Action{Type: ActionSetRelay, Target: DevSolarController, RelayValue: ValueOff},
		},
	}
}
