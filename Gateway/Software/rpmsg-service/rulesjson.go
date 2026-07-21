package main

// App JSON (KMP app) <-> engine rules. The app keys conditions/actions by the STABLE
// node_id (survives address reuse / re-pair / rename); Go resolves node_id -> the node's
// CURRENT RF address only when pushing to the M4F engine. The raw app JSON (node_id-based)
// is what the DB stores and getrules returns; address resolution never touches the DB.
//   [{"name","enabled","conds":[{type,...}],"action":{node,actionType,value,msg}}]
// Conditions: TIME{hS,mS,hE,mE} PARAMETER{node,p,op,mn,mx} DELTA{node1,p1,node2,p2,op,mn,mx}.

import (
	"encoding/json"
	"fmt"
	"log"
)

type appAction struct {
	Node       int64   `json:"node"`       // node_id of the target (0 = none, e.g. SEND_MESSAGE)
	ActionType uint8   `json:"actionType"` // ACTION_*
	Value      float64 `json:"value"`      // 0/1 (on/off), 0-100 (%), setpoint...
	Msg        string  `json:"msg"`        // SEND_MESSAGE text
}

// Permissive input: absent keys decode to 0 (fine - we read only the ones relevant to
// the cond's type). Each JSON key is its own field (Go tags can't list multiple keys).
type appCond struct {
	Type  uint8   `json:"type"`
	HS    uint8   `json:"hS"`
	MS    uint8   `json:"mS"`
	HE    uint8   `json:"hE"`
	ME    uint8   `json:"mE"`
	Node  int64   `json:"node"` // PARAMETER: source node_id
	P     uint8   `json:"p"`
	Node1 int64   `json:"node1"` // DELTA
	P1    uint8   `json:"p1"`
	Node2 int64   `json:"node2"`
	P2    uint8   `json:"p2"`
	Op    uint8   `json:"op"`
	Mn    float64 `json:"mn"`
	Mx    float64 `json:"mx"`
}

type appRule struct {
	Name    string    `json:"name"`
	Enabled bool      `json:"enabled"` // disabled rules are stored but NOT pushed to the engine
	Conds   []appCond `json:"conds"`
	Action  appAction `json:"action"`
}

// parseAppRules decodes the app's rules JSON (node_id-based) and light-validates the
// condition/action codes. Returns the app-level rules (not yet address-resolved).
func parseAppRules(data string) ([]appRule, error) {
	var in []appRule
	if err := json.Unmarshal([]byte(data), &in); err != nil {
		return nil, fmt.Errorf("rules JSON: %w", err)
	}
	for _, r := range in {
		for _, c := range r.Conds {
			if c.Type != CondTime && c.Type != CondParameter && c.Type != CondParameterDelta {
				return nil, fmt.Errorf("rule %q: unknown condition type %d", r.Name, c.Type)
			}
		}
		if r.Action.ActionType != ActionSetRelay && r.Action.ActionType != ActionSendMessage {
			return nil, fmt.Errorf("rule %q: unknown action type %d", r.Name, r.Action.ActionType)
		}
	}
	return in, nil
}

// appRulesToWire resolves each rule's node_id references to CURRENT RF addresses and
// keeps only ENABLED rules whose EVERY referenced node resolves (has an address). A rule
// referencing a detached/removed node is skipped (logged) -> inactive until the node is
// re-paired; the DB still holds it (getrules shows it, app marks it "unavailable").
func appRulesToWire(in []appRule, store *Store) []Rule {
	addr := func(id int64) (uint8, bool) {
		a, ok, err := store.addressForNode(id)
		if err != nil || !ok {
			return 0, false
		}
		return a, true
	}
	out := make([]Rule, 0, len(in))
	for _, r := range in {
		if !r.Enabled {
			continue
		}
		rule := Rule{Name: r.Name}
		ok := true
		for _, c := range r.Conds {
			cond := Condition{Type: c.Type}
			switch c.Type {
			case CondTime:
				cond.Time = TimeCond{HourStart: c.HS, MinStart: c.MS, HourEnd: c.HE, MinEnd: c.ME}
			case CondParameter:
				a, aok := addr(c.Node)
				if !aok {
					ok = false
				}
				cond.Param = ParamCond{NodeAddr: a, Parameter: c.P, Op: c.Op, Min: float32(c.Mn), Max: float32(c.Mx)}
			case CondParameterDelta:
				a1, ok1 := addr(c.Node1)
				a2, ok2 := addr(c.Node2)
				if !ok1 || !ok2 {
					ok = false
				}
				cond.Delta = DeltaCond{NodeAddr1: a1, Parameter1: c.P1, NodeAddr2: a2, Parameter2: c.P2, Op: c.Op, Min: float32(c.Mn), Max: float32(c.Mx)}
			}
			if !ok {
				break
			}
			rule.Conditions = append(rule.Conditions, cond)
		}
		if ok {
			switch r.Action.ActionType {
			case ActionSetRelay:
				a, aok := addr(r.Action.Node)
				if !aok {
					ok = false
				}
				rule.Action = Action{Type: ActionSetRelay, NodeAddr: a, Value: float32(r.Action.Value)}
			case ActionSendMessage:
				rule.Action = Action{Type: ActionSendMessage, Message: r.Action.Msg}
			}
		}
		if !ok {
			log.Printf("[rules] skip %q - references an unavailable node (detached/removed)", r.Name)
			continue
		}
		out = append(out, rule)
	}
	return out
}
