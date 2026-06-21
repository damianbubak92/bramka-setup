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

// pendingJoin is a node that pressed JOIN and awaits user approval.
type pendingJoin struct {
	FactoryID string // hex of the 8-byte CC1310 factory id (the chip identity)
	NodeType  uint8
	FirstSeen int64 // unix s
	LastSeen  int64
	Count     int // JOINs seen (the node retransmits until provisioned)
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
