package main

// Provisioning (gen2) — the gateway side of node commissioning. Phase 1 step 3:
// a node presses its JOIN button -> CMD_JOIN_REQUEST rides the NODE_TELEMETRY
// path up to here; we record it (dedup by factory id) so the phone can later
// list + approve it (step 4). See the project memory [[provisioning-model]].

import (
	"encoding/hex"
	"sync"
)

// CmdJoinRequest mirrors CMD_JOIN_REQUEST (node_protocol.h). Plain Go const so the
// non-cgo drain can demux provisioning frames without importing C.
const CmdJoinRequest uint8 = 4

// pendingJoin is a node that pressed JOIN and awaits user approval. JSON tags
// shape the listjoins response the phone reads in step 4.
type pendingJoin struct {
	FactoryID string `json:"factory"`   // hex of the 8-byte CC1310 factory id (the chip identity)
	NodeType  uint8  `json:"type"`      // NODE_* (node_protocol.h)
	FirstSeen int64  `json:"firstSeen"` // unix s
	LastSeen  int64  `json:"lastSeen"`
	Count     int    `json:"count"` // JOINs seen (the node retransmits until provisioned)
}

// joinRegistry holds JOIN requests awaiting approval, keyed by factory id.
type joinRegistry struct {
	mu      sync.Mutex
	pending map[string]*pendingJoin
}

func newJoinRegistry() *joinRegistry {
	return &joinRegistry{pending: map[string]*pendingJoin{}}
}

// Add records a JOIN (dedup by factory id). Returns true the first time a given
// chip is seen, so the caller notifies the user only once per node.
func (r *joinRegistry) Add(factoryID [8]byte, nodeType uint8, ts int64) (first bool) {
	key := hex.EncodeToString(factoryID[:])
	r.mu.Lock()
	defer r.mu.Unlock()
	if p := r.pending[key]; p != nil {
		p.LastSeen = ts
		p.Count++
		return false
	}
	r.pending[key] = &pendingJoin{
		FactoryID: key, NodeType: nodeType, FirstSeen: ts, LastSeen: ts, Count: 1,
	}
	return true
}

// List returns a snapshot of pending joins (for the phone API in step 4).
func (r *joinRegistry) List() []pendingJoin {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]pendingJoin, 0, len(r.pending))
	for _, p := range r.pending {
		out = append(out, *p)
	}
	return out
}

// Get looks up a pending join by factory id (hex).
func (r *joinRegistry) Get(factoryID string) (pendingJoin, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if p := r.pending[factoryID]; p != nil {
		return *p, true
	}
	return pendingJoin{}, false
}

// Remove drops a join from the pending set (call after it is provisioned).
func (r *joinRegistry) Remove(factoryID string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.pending, factoryID)
}

// factoryHexToBytes parses the 16-char hex factory id into the 8-byte form the
// JOIN_ACCEPT wire struct carries.
func factoryHexToBytes(s string) ([8]byte, bool) {
	var out [8]byte
	b, err := hex.DecodeString(s)
	if err != nil || len(b) != 8 {
		return out, false
	}
	copy(out[:], b)
	return out, true
}
