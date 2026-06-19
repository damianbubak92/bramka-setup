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
