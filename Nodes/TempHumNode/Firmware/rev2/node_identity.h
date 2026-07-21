/*
 * node_identity.h - gen2 identity + provisioning for the rev-2 T&H sensor node.
 *
 * Ported verbatim from SolarControllerNode; the ONLY node-specific line is
 * NODE_CAPABILITIES = 0 (a T&H node only measures/reports - it executes no
 * actions, so it is never offered as an automation ACTION target; it can still be
 * a condition SOURCE, e.g. feedback for a heating rule).
 *
 * Identity = factory_id (FCFG IEEE MAC, immutable) + NVS-persisted RF address
 * (0x10-0xEF; 0xFF = unprovisioned -> silent, JOIN on the button only).
 * Constants MUST match Shared/Protocol/node_protocol.h + automation.h + the gateway.
 */
#ifndef NODE_IDENTITY_H
#define NODE_IDENTITY_H

#include <stdint.h>
#include <stdbool.h>

/* Addresses (1 byte). */
#define ADDR_GATEWAY         0x00u
#define ADDR_UNPROVISIONED   0xFFu
#define ADDR_POOL_FIRST      0x10u
#define ADDR_POOL_LAST       0xEFu

#define NODE_FACTORY_ID_LEN  8u

/* RF frame tag (byte [1]). 'E' carries factory_id; 'D' = legacy no-id. */
#define RF_FRAME_TAG_LEGACY  'D'
#define RF_FRAME_TAG_V2      'E'

/* Provisioning commands (MessageStruct.cmd) - match node_protocol.h. */
#define CMD_JOIN_REQUEST     4u
#define CMD_JOIN_ACCEPT      5u
#define CMD_REMOVE           6u
#define CMD_UNREGISTERED     7u

/* Capability bitmask declared in joinData.capabilities at JOIN (NODE_CAP(a)=1<<a),
 * match automation.h. A T&H sensor has NO executable actions -> 0. */
#define ACTION_SET_RELAY     0u
#define NODE_CAP(action)     (1u << (action))
#define NODE_CAPABILITIES    0u   /* sensor-only: no actions */

/* This node's identity. gNodeAddress is NVS-backed; gFactoryId is read from FCFG. */
extern uint8_t gNodeAddress;
extern uint8_t gFactoryId[NODE_FACTORY_ID_LEN];

void identity_init(void);       /* read FCFG id + load NVS address (call at startup) */
bool identity_persist(void);    /* write gNodeAddress to NVS (after JOIN_ACCEPT/REMOVE) */
bool factory_is_mine(const uint8_t *fid);  /* downlink "is this for me?" check */

#endif /* NODE_IDENTITY_H */
