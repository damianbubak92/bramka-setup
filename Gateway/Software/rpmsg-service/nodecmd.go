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
	// MessageStruct must be byte-identical to the M4F/CC1310 struct (44 B).
	if got := int(unsafe.Sizeof(C.MessageStruct{})); got != 44 {
		abiOK = false
		log.Printf("[nodecmd] ABI MISMATCH MessageStruct: C header=%d, expected=44 - node cmd DISABLED", got)
	}
}

// encodeNodeText builds the wire image of a MessageStruct holding a text command.
func encodeNodeText(nodeType, cmd uint8, text string) ([]byte, error) {
	if len(text) >= int(C.NODE_TEXT_MAX) {
		return nil, fmt.Errorf("node text too long (%d, max %d)", len(text), int(C.NODE_TEXT_MAX)-1)
	}
	var m C.MessageStruct
	ctext := C.CString(text)
	defer C.free(unsafe.Pointer(ctext))
	C.msg_make_text(&m, C.uint8_t(nodeType), C.uint8_t(cmd), ctext)
	return C.GoBytes(unsafe.Pointer(&m), C.int(unsafe.Sizeof(m))), nil
}

// SendNodeText relays a text command to a node via MSG_NODE_CMD (reliable: M4F
// ACKs after queueing it for SPI -> CC1310 -> RF).
func (p *Protocol) SendNodeText(nodeType, cmd uint8, text string) error {
	if !abiOK {
		return fmt.Errorf("node cmd disabled: MessageStruct ABI mismatch (see startup log)")
	}
	b, err := encodeNodeText(nodeType, cmd, text)
	if err != nil {
		return err
	}
	return p.sendReliableTyped(MsgNodeCmd, b)
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
	b := C.GoBytes(unsafe.Pointer(&m), C.int(unsafe.Sizeof(m)))
	return p.sendReliableTyped(MsgNodeCmd, b)
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
	b := C.GoBytes(unsafe.Pointer(&m), C.int(unsafe.Sizeof(m)))
	return p.sendReliableTyped(MsgNodeCmd, b)
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
	b := C.GoBytes(unsafe.Pointer(&m), C.int(unsafe.Sizeof(m)))
	return p.sendReliableTyped(MsgNodeCmd, b)
}
