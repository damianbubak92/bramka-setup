/*
 * node_identity.h - gen2 node identity + provisioning for the solar controller.
 *
 * A node is identified by two things:
 *   - factory_id: the chip's IEEE MAC from FCFG (8 bytes, unique per chip, immutable).
 *   - address:    the gateway-assigned RF address (0x10-0xEF), NVS-persisted; 0xFF =
 *                 unprovisioned (the node is silent, only sends JOIN on the button).
 *
 * Constants MUST match Shared/Protocol/node_protocol.h and the gateway. See
 * Docs/NODE-MANAGEMENT.md (§5 wire contract, §6 lifecycle).
 */
#ifndef NODE_IDENTITY_H
#define NODE_IDENTITY_H

#include <stdint.h>
#include <stdbool.h>

/* Addresses (1 byte). */
#define ADDR_GATEWAY         0x00u   /* gen2 concentrator (gen1 legacy = 0xF0) */
#define ADDR_UNPROVISIONED   0xFFu   /* no address yet -> JOIN src/dest, node stays silent */
#define ADDR_POOL_FIRST      0x10u
#define ADDR_POOL_LAST       0xEFu

#define NODE_FACTORY_ID_LEN  8u

/* RF frame tag (byte [1]). 'E' carries factory_id, 'D' is the legacy no-id format. */
#define RF_FRAME_TAG_LEGACY  'D'
#define RF_FRAME_TAG_V2      'E'

/* Provisioning commands (MessageStruct.cmd) - match node_protocol.h. */
#define CMD_JOIN_REQUEST     4u   /* node->gw: src 0xFF, joinData.factory_id */
#define CMD_JOIN_ACCEPT      5u   /* gw->node: dest 0xFF, joinAcceptData (assigned addr) */
#define CMD_REMOVE           6u   /* gw->node: user removed the device -> erase address */
#define CMD_UNREGISTERED     7u   /* gw->node: (addr,factory_id) mismatch -> erase address */

/* This node's identity. gNodeAddress is NVS-backed; gFactoryId is read from FCFG. */
extern uint8_t gNodeAddress;
extern uint8_t gFactoryId[NODE_FACTORY_ID_LEN];

/* Read the FCFG factory id and load the persisted address from NVS (0xFF if none).
 * Call once at startup (radioTaskInit) before the RF task runs. */
void identity_init(void);

/* Persist gNodeAddress to NVS. Call after adopting (JOIN_ACCEPT) or clearing
 * (REMOVE / UNREGISTERED) the address. Returns true on success. */
bool identity_persist(void);

/* true iff fid equals this chip's FCFG id - the downlink "is this command for me?"
 * check (a v2 'E' frame carries the target factory_id). */
bool factory_is_mine(const uint8_t *fid);

#endif /* NODE_IDENTITY_H */
