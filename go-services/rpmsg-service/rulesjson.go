package main

// App JSON (Android GatewayCommunicator, AutomationRuleModel) <-> Rule model.
// The app's numeric enums map 1:1 to shared/automation.h codes (verified), so
// fields pass through as-is. Array shape:
//   [{"name","condCnt","conds":[{type,...}],"action":{target,actionType,value}}]
// Conditions by type: TIME{hS,mS,hE,mE} PARAMETER{d,p,op,mn,mx} DELTA{d1,p1,d2,p2,op,mn,mx}.

import (
	"encoding/json"
	"fmt"
)

type appAction struct {
	Target     uint8 `json:"target"`
	ActionType uint8 `json:"actionType"`
	Value      int   `json:"value"`
}

// Permissive input cond: absent keys decode to 0 (fine - we read only the ones
// relevant to the cond's type). Each JSON key is its own field (Go tags can't
// list multiple keys).
type appCond struct {
	Type uint8   `json:"type"`
	HS   uint8   `json:"hS"`
	MS   uint8   `json:"mS"`
	HE   uint8   `json:"hE"`
	ME   uint8   `json:"mE"`
	D    uint8   `json:"d"`
	P    uint8   `json:"p"`
	D1   uint8   `json:"d1"`
	P1   uint8   `json:"p1"`
	D2   uint8   `json:"d2"`
	P2   uint8   `json:"p2"`
	Op   uint8   `json:"op"`
	Mn   float64 `json:"mn"`
	Mx   float64 `json:"mx"`
}

type appRule struct {
	Name   string    `json:"name"`
	Conds  []appCond `json:"conds"`
	Action appAction `json:"action"`
}

// parseAppRules decodes the phone app's rules JSON array into the Rule model.
func parseAppRules(data string) ([]Rule, error) {
	var in []appRule
	if err := json.Unmarshal([]byte(data), &in); err != nil {
		return nil, fmt.Errorf("rules JSON: %w", err)
	}
	rules := make([]Rule, 0, len(in))
	for _, r := range in {
		rule := Rule{Name: r.Name}
		for _, c := range r.Conds {
			cond := Condition{Type: c.Type}
			switch c.Type {
			case CondTime:
				cond.Time = TimeCond{HourStart: c.HS, MinStart: c.MS, HourEnd: c.HE, MinEnd: c.ME}
			case CondParameter:
				cond.Param = ParamCond{Device: c.D, Parameter: c.P, Op: c.Op,
					Min: float32(c.Mn), Max: float32(c.Mx)}
			case CondParameterDelta:
				cond.Delta = DeltaCond{Device1: c.D1, Parameter1: c.P1,
					Device2: c.D2, Parameter2: c.P2, Op: c.Op,
					Min: float32(c.Mn), Max: float32(c.Mx)}
			default:
				return nil, fmt.Errorf("rule %q: unknown condition type %d", r.Name, c.Type)
			}
			rule.Conditions = append(rule.Conditions, cond)
		}
		// The app's action carries only an int "value" (no "msg"): map to the relay
		// value. SEND_MESSAGE rules from the app thus have no text (app limitation).
		rule.Action = Action{Type: r.Action.ActionType, Target: r.Action.Target,
			RelayValue: uint8(r.Action.Value)}
		rules = append(rules, rule)
	}
	return rules, nil
}

// marshalAppRules encodes the Rule model back to the app's JSON array, emitting
// only the keys relevant to each condition type (extra keys would be harmless -
// the app's fromJson ignores unknown keys - but clean output matches the app).
func marshalAppRules(rules []Rule) (string, error) {
	arr := make([]map[string]interface{}, 0, len(rules))
	for _, r := range rules {
		conds := make([]map[string]interface{}, 0, len(r.Conditions))
		for _, c := range r.Conditions {
			m := map[string]interface{}{"type": c.Type}
			switch c.Type {
			case CondTime:
				m["hS"], m["mS"], m["hE"], m["mE"] = c.Time.HourStart, c.Time.MinStart, c.Time.HourEnd, c.Time.MinEnd
			case CondParameter:
				m["d"], m["p"], m["op"] = c.Param.Device, c.Param.Parameter, c.Param.Op
				m["mn"], m["mx"] = c.Param.Min, c.Param.Max
			case CondParameterDelta:
				m["d1"], m["p1"], m["d2"], m["p2"], m["op"] = c.Delta.Device1, c.Delta.Parameter1, c.Delta.Device2, c.Delta.Parameter2, c.Delta.Op
				m["mn"], m["mx"] = c.Delta.Min, c.Delta.Max
			}
			conds = append(conds, m)
		}
		arr = append(arr, map[string]interface{}{
			"name":    r.Name,
			"condCnt": len(r.Conditions),
			"conds":   conds,
			"action": map[string]interface{}{
				"target":     r.Action.Target,
				"actionType": r.Action.Type,
				"value":      r.Action.RelayValue, // SET_RELAY value; ignored by app for SEND_MESSAGE
			},
		})
	}
	b, err := json.Marshal(arr)
	if err != nil {
		return "", err
	}
	return string(b), nil
}
