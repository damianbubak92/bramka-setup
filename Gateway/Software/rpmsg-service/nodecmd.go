package main

/*
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "protocol.h"
#include "node_protocol.h"

// Build a MessageStruct carrying a text command (e.g. "PUMP_ON") for a node.
// The C COMPILER owns the layout (shared/node_protocol.h, identical on M4F and
// CC1310), so the bytes Go ships are byte-identical across RPMsg and SPI - no
// Go-side offset/padding risk.
static void msg_make_text(MessageStruct *m, uint8_t type, uint8_t cmd, const char *text) {
    memset(m, 0, sizeof(*m));
    m->type = type;
    m->cmd  = cmd;
    strncpy(m->payload.textData.text, text, NODE_TEXT_MAX - 1);
}

// Pump on/off, built EXACTLY like gen1 coreTask.c translated a phone "PUMP_ON"
// into a real node command: id=0xF1, SOLAR_CONTROLLER / TURN_PUMP_ON_OFF, with
// pumpState=1/0 and length = sizeof(pumpData)+4 (=5). The node acts only on this
// form (it ignores a raw SMARTPHONE/SEND_TEXT_MSG frame).
static void msg_make_pump(MessageStruct *m, uint8_t nodeId, uint8_t on) {
    memset(m, 0, sizeof(*m));
    m->id   = nodeId;
    m->type = NODE_SOLAR_CONTROLLER;
    m->cmd  = CMD_TURN_PUMP_ON_OFF;
    m->payload.pumpData.pumpState = on ? 1u : 0u;
    m->length = (uint8_t)(sizeof(m->payload.pumpData) + 4u);
}

// Provisioning REMOVE (gateway -> node). Addressed to the node's current address;
// the node erases its stored address (-> unprovisioned 0xFF) if factory_id matches.
// Reuses joinData (factory_id) as the payload.
static void msg_make_remove(MessageStruct *m, const uint8_t *factory_id,
                            uint8_t node_type, uint8_t dest_addr) {
    memset(m, 0, sizeof(*m));
    m->id   = dest_addr;
    m->type = node_type;
    m->cmd  = CMD_REMOVE;
    memcpy(m->payload.joinData.factory_id, factory_id, NODE_FACTORY_ID_LEN);
    m->length = (uint8_t)(sizeof(m->payload.joinData) + 4u);
}

// Provisioning JOIN_ACCEPT (gateway -> node, step 4). Addressed to
// ADDR_UNPROVISIONED (0xFF) since the node has no address yet; the unprovisioned
// node acts only if factory_id matches its own, then stores assigned_addr (step 5).
// type carries the node's type so the CC1310 RX/Go demux stays consistent.
static void msg_make_join_accept(MessageStruct *m, const uint8_t *factory_id,
                                 uint8_t node_type, uint8_t assigned_addr) {
    memset(m, 0, sizeof(*m));
    m->id   = ADDR_UNPROVISIONED;
    m->type = node_type;
    m->cmd  = CMD_JOIN_ACCEPT;
    memcpy(m->payload.joinAcceptData.factory_id, factory_id, NODE_FACTORY_ID_LEN);
    m->payload.joinAcceptData.assigned_addr = assigned_addr;
    m->length = (uint8_t)(sizeof(m->payload.joinAcceptData) + 4u);
}

// Reactive UNREGISTERED (gateway -> node). Addressed to the node's current address;
// the node erases its stored address (-> unprovisioned) if the FRAME factory_id
// matches its own FCFG. Same node action as REMOVE, distinct cmd for log clarity.
// The target factory_id travels at the NodeFrame level (frame_wrap), not in payload.
static void msg_make_unregister(MessageStruct *m, uint8_t node_type, uint8_t dest_addr) {
    memset(m, 0, sizeof(*m));
    m->id   = dest_addr;
    m->type = node_type;
    m->cmd  = CMD_UNREGISTERED;
    m->length = 4u;   // header only; identity is the frame-level factory_id
}

// frame_wrap builds the identity-tagged envelope (NodeFrame) the internal links now
// carry: factory_id (the chip this command targets; NULL/zero => legacy 'D' node,
// the CC1310 then sends the old no-factory_id frame) followed by the MessageStruct.
static void frame_wrap(NodeFrame *f, const uint8_t *factory_id, const MessageStruct *m) {
    memset(f, 0, sizeof(*f));
    if (factory_id != NULL) memcpy(f->factory_id, factory_id, NODE_FACTORY_ID_LEN);
    f->msg = *m;
}
*/
import "C"

import (
	"fmt"
	"log"
	"unsafe"
)

// MsgNodeCmd mirrors protocol.h MSG_NODE_CMD (Linux -> M4F: relay a phone
// command to a node). Plain Go const so non-cgo files can reference it too.
const MsgNodeCmd = 0x33

func init() {
	// MessageStruct must be byte-identical to the M4F/CC1310 struct (44 B), and the
	// NodeFrame envelope 52 B (8 factory_id + 44 msg). A mismatch disables node cmds.
	if got := int(unsafe.Sizeof(C.MessageStruct{})); got != 44 {
		abiOK = false
		log.Printf("[nodecmd] ABI MISMATCH MessageStruct: C header=%d, expected=44 - node cmd DISABLED", got)
	}
	if got := int(unsafe.Sizeof(C.NodeFrame{})); got != 52 {
		abiOK = false
		log.Printf("[nodecmd] ABI MISMATCH NodeFrame: C header=%d, expected=52 - node cmd DISABLED", got)
	}
}

// nodeFrameBytes wraps a MessageStruct in the NodeFrame envelope with the target
// factory_id (nil => all-zero => a legacy 'D' node) and returns the 52-byte wire
// image that MSG_NODE_CMD now carries.
func nodeFrameBytes(factoryID *[8]byte, m *C.MessageStruct) []byte {
	var f C.NodeFrame
	var fidPtr *C.uint8_t
	if factoryID != nil {
		fidPtr = (*C.uint8_t)(unsafe.Pointer(&factoryID[0]))
	}
	C.frame_wrap(&f, fidPtr, m)
	return C.GoBytes(unsafe.Pointer(&f), C.int(unsafe.Sizeof(f)))
}

// SendNodeText relays a text command to a node via MSG_NODE_CMD (reliable: M4F
// ACKs after queueing it for SPI -> CC1310 -> RF). Generic path (no specific target
// chip) -> the NodeFrame carries a zero factory_id (legacy 'D' framing).
func (p *Protocol) SendNodeText(nodeType, cmd uint8, text string) error {
	if !abiOK {
		return fmt.Errorf("node cmd disabled: MessageStruct ABI mismatch (see startup log)")
	}
	if len(text) >= int(C.NODE_TEXT_MAX) {
		return fmt.Errorf("node text too long (%d, max %d)", len(text), int(C.NODE_TEXT_MAX)-1)
	}
	var m C.MessageStruct
	ctext := C.CString(text)
	defer C.free(unsafe.Pointer(ctext))
	C.msg_make_text(&m, C.uint8_t(nodeType), C.uint8_t(cmd), ctext)
	return p.sendReliableTyped(MsgNodeCmd, nodeFrameBytes(nil, &m))
}

// pumpNodeID is the RF node address gen1 used for the pump command (coreTask.c).
const pumpNodeID = 0xF1

// SendPump turns the pump on/off. The gateway must do the gen1 translation: the
// node acts on a SOLAR_CONTROLLER / TURN_PUMP_ON_OFF command with pumpState, NOT
// on the raw SMARTPHONE/SEND_TEXT_MSG text. Built byte-for-byte like gen1 so the
// unchanged RF node reacts.
func (p *Protocol) SendPump(on bool) error {
	if !abiOK {
		return fmt.Errorf("node cmd disabled: MessageStruct ABI mismatch (see startup log)")
	}
	var m C.MessageStruct
	onVal := C.uint8_t(0)
	if on {
		onVal = 1
	}
	C.msg_make_pump(&m, C.uint8_t(pumpNodeID), onVal)
	// The pump target (0xF1) is a legacy gen1 node - zero factory_id => 'D' framing.
	return p.sendReliableTyped(MsgNodeCmd, nodeFrameBytes(nil, &m))
}

// SendPumpTo turns a SPECIFIC (gen2) solar node's pump on/off. Targets nodeAddr and
// carries the node's factory_id so the CC1310 emits a v2 'E' frame the node validates
// (a zero factoryID falls back to a legacy 'D' frame). Same SOLAR_CONTROLLER /
// TURN_PUMP_ON_OFF payload the node acts on.
func (p *Protocol) SendPumpTo(nodeAddr uint8, factoryID [8]byte, on bool) error {
	if !abiOK {
		return fmt.Errorf("node cmd disabled: MessageStruct ABI mismatch (see startup log)")
	}
	var m C.MessageStruct
	onVal := C.uint8_t(0)
	if on {
		onVal = 1
	}
	C.msg_make_pump(&m, C.uint8_t(nodeAddr), onVal)
	return p.sendReliableTyped(MsgNodeCmd, nodeFrameBytes(&factoryID, &m))
}

// SendJoinAccept tells an approved, unprovisioned node its assigned address
// (provisioning step 4). It rides MSG_NODE_CMD like SendPump (M4F -> SPI ->
// CC1310 -> RF, addressed to 0xFF). Reliable to the M4F (it ACKs after queueing
// for SPI); RF delivery to the node is confirmed later when the node reports
// under its new address (step 5). The node retransmits JOIN until accepted.
func (p *Protocol) SendJoinAccept(factoryID [8]byte, nodeType, assignedAddr uint8) error {
	if !abiOK {
		return fmt.Errorf("node cmd disabled: MessageStruct ABI mismatch (see startup log)")
	}
	var m C.MessageStruct
	C.msg_make_join_accept(&m,
		(*C.uint8_t)(unsafe.Pointer(&factoryID[0])),
		C.uint8_t(nodeType), C.uint8_t(assignedAddr))
	return p.sendReliableTyped(MsgNodeCmd, nodeFrameBytes(&factoryID, &m))
}

// SendRemove tells a provisioned node to drop its identity (erase its stored
// address -> unprovisioned). Addressed to the node's current address; the node
// matches factory_id before acting. Best-effort to RF (reliable to the M4F).
func (p *Protocol) SendRemove(factoryID [8]byte, nodeType, nodeAddr uint8) error {
	if !abiOK {
		return fmt.Errorf("node cmd disabled: MessageStruct ABI mismatch (see startup log)")
	}
	var m C.MessageStruct
	C.msg_make_remove(&m,
		(*C.uint8_t)(unsafe.Pointer(&factoryID[0])),
		C.uint8_t(nodeType), C.uint8_t(nodeAddr))
	return p.sendReliableTyped(MsgNodeCmd, nodeFrameBytes(&factoryID, &m))
}

// SendUnregister tells a node that its (address, factory_id) did NOT match the
// gateway's binding (impersonation / stale chip on a reused address): erase the
// stored address -> unprovisioned, go silent. Reactive counterpart to SendRemove;
// the node acts only if the frame factory_id equals its own FCFG. Best-effort to RF.
func (p *Protocol) SendUnregister(factoryID [8]byte, nodeType, nodeAddr uint8) error {
	if !abiOK {
		return fmt.Errorf("node cmd disabled: MessageStruct ABI mismatch (see startup log)")
	}
	var m C.MessageStruct
	C.msg_make_unregister(&m, C.uint8_t(nodeType), C.uint8_t(nodeAddr))
	return p.sendReliableTyped(MsgNodeCmd, nodeFrameBytes(&factoryID, &m))
}
