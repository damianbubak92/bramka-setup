package main

/*
#include <string.h>
#include <stdint.h>
#include "node_protocol.h"

// Decoded "kind": which union member the wire MessageStruct actually carries.
// We choose it from type (+cmd) so we never reinterpret overlapping union bytes.
#define DEC_KIND_NONE   0
#define DEC_KIND_SOLAR  1   // full solarData snapshot
#define DEC_KIND_PUMP   2   // pumpState only (SEND_PUMP_STATUS)
#define DEC_KIND_BUFOR  3   // sBuforTemp
#define DEC_KIND_TH     4   // temperature + humidity

// Flat, Go-readable mirror of the relevant union members. The C compiler owns
// the union layout (shared/node_protocol.h), so msg_decode reads the CORRECT
// member here and Go just reads named scalars - no Go-side byte reinterpretation.
typedef struct {
    uint8_t kind;
    float   Tin, Tout, T4, T3, T2, T1, Tcol;
    int32_t energyGain, flowRate;
    uint8_t pumpState;
    float   sBuforTemp;
    float    temperature, humidity;
    uint16_t batt_mv;
} DecodedNode;

// Mirror of gen1's per-type message handling. A new node type = one more case
// here (+ param_def rows). NOTE: solar reports the full reading on
// SEND_DATA_TO_DB and just the pump state on SEND_PUMP_STATUS (pumpData).
static void msg_decode(const MessageStruct *m, DecodedNode *d) {
    memset(d, 0, sizeof(*d));
    switch (m->type) {
    case NODE_SOLAR_CONTROLLER:
        if (m->cmd == CMD_SEND_PUMP_STATUS) {
            d->kind = DEC_KIND_PUMP;
            d->pumpState = m->payload.pumpData.pumpState;
        } else {
            d->kind = DEC_KIND_SOLAR;
            d->Tin        = m->payload.solarData.Tin;
            d->Tout       = m->payload.solarData.Tout;
            d->T4         = m->payload.solarData.T4;
            d->T3         = m->payload.solarData.T3;
            d->T2         = m->payload.solarData.T2;
            d->T1         = m->payload.solarData.T1;
            d->Tcol       = m->payload.solarData.Tcol;
            d->energyGain = m->payload.solarData.energyGain;
            d->flowRate   = m->payload.solarData.flowRate;
            d->pumpState  = m->payload.solarData.pumpState;
        }
        break;
    case NODE_BUFOR_CONTROLLER:
        d->kind = DEC_KIND_BUFOR;
        d->sBuforTemp = m->payload.buforData.sBuforTemp;
        break;
    case NODE_TH_SENSOR:
        d->kind = DEC_KIND_TH;
        d->temperature = m->payload.thData.temperature;
        d->humidity    = m->payload.thData.humidity;
        d->batt_mv     = m->payload.thData.batt_mv;
        break;
    default:
        d->kind = DEC_KIND_NONE;
        break;
    }
}

// Copy a JOIN_REQUEST's factory id out of the union (Go can't address union
// members directly). The C compiler owns the layout.
static void msg_join_factory_id(const MessageStruct *m, uint8_t out[NODE_FACTORY_ID_LEN]) {
    memcpy(out, m->payload.joinData.factory_id, NODE_FACTORY_ID_LEN);
}
*/
import "C"

import "unsafe"

// NodeMsgCmd peeks the MessageStruct.cmd of a NODE_TELEMETRY payload so the drain
// can route provisioning frames (CMD_JOIN_REQUEST) away from the telemetry path.
func NodeMsgCmd(payload []byte) (cmd uint8, ok bool) {
	need := int(unsafe.Sizeof(C.MessageStruct{}))
	if len(payload) < need {
		return 0, false
	}
	var m C.MessageStruct
	C.memcpy(unsafe.Pointer(&m), unsafe.Pointer(&payload[0]), C.size_t(need))
	return uint8(m.cmd), true
}

// NodeMsgId returns the MessageStruct.id (node source address) of a
// NODE_TELEMETRY payload - used to demux a remove-confirmation by source address.
func NodeMsgId(payload []byte) (id uint8, ok bool) {
	need := int(unsafe.Sizeof(C.MessageStruct{}))
	if len(payload) < need {
		return 0, false
	}
	var m C.MessageStruct
	C.memcpy(unsafe.Pointer(&m), unsafe.Pointer(&payload[0]), C.size_t(need))
	return uint8(m.id), true
}

// DecodeJoinRequest extracts a joining node's factory id + type from a
// CMD_JOIN_REQUEST MessageStruct (id is ADDR_UNPROVISIONED on the wire).
func DecodeJoinRequest(payload []byte) (factoryID [8]byte, nodeType uint8, ok bool) {
	need := int(unsafe.Sizeof(C.MessageStruct{}))
	if len(payload) < need {
		return factoryID, 0, false
	}
	var m C.MessageStruct
	C.memcpy(unsafe.Pointer(&m), unsafe.Pointer(&payload[0]), C.size_t(need))
	if uint8(m.cmd) != uint8(C.CMD_JOIN_REQUEST) {
		return factoryID, 0, false
	}
	C.msg_join_factory_id(&m, (*C.uint8_t)(unsafe.Pointer(&factoryID[0])))
	return factoryID, uint8(m._type), true
}

// NodeParam is one decoded (key, value) reading from a node. Stored generically
// in node_param (current state) so heterogeneous node types need no schema change;
// types with derived/accumulated data also feed a dedicated table (e.g. solar).
// The param_key matches the node_protocol.h union field name and the param_def rows.
type NodeParam struct {
	Key string
	Num float64
}

// DecodeTelemetry parses a MSG_NODE_TELEMETRY payload (a wire MessageStruct) into
// node id/type + a generic param list. ok=false if the payload is too short or
// the node type is unknown (caller logs + skips).
func DecodeTelemetry(payload []byte) (nodeID, nodeType uint8, params []NodeParam, ok bool) {
	need := int(unsafe.Sizeof(C.MessageStruct{}))
	if len(payload) < need {
		return 0, 0, nil, false
	}
	var m C.MessageStruct
	C.memcpy(unsafe.Pointer(&m), unsafe.Pointer(&payload[0]), C.size_t(need))

	var d C.DecodedNode
	C.msg_decode(&m, &d)

	nodeID = uint8(m.id)
	nodeType = uint8(m._type) // cgo renames the C field `type` (a Go keyword) to _type

	switch d.kind {
	case C.DEC_KIND_SOLAR:
		params = []NodeParam{
			{"Tcol", float64(d.Tcol)},
			{"Tin", float64(d.Tin)},
			{"Tout", float64(d.Tout)},
			{"T1", float64(d.T1)},
			{"T2", float64(d.T2)},
			{"T3", float64(d.T3)},
			{"T4", float64(d.T4)},
			{"energyGain", float64(int32(d.energyGain))},
			{"flowRate", float64(int32(d.flowRate))},
			{"pumpState", float64(d.pumpState)},
		}
	case C.DEC_KIND_PUMP:
		params = []NodeParam{{"pumpState", float64(d.pumpState)}}
	case C.DEC_KIND_BUFOR:
		params = []NodeParam{{"sBuforTemp", float64(d.sBuforTemp)}}
	case C.DEC_KIND_TH:
		params = []NodeParam{
			{"temperature", float64(d.temperature)},
			{"humidity", float64(d.humidity)},
			{"batt_mv", float64(d.batt_mv)},
		}
	default:
		return nodeID, nodeType, nil, false
	}
	return nodeID, nodeType, params, true
}
