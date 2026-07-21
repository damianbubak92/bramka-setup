/**
 * node_protocol.h - Node <-> gateway data model (shared M4F C / Go cgo).
 *
 * Ported from gen1 messageProtocol.h (CC3235/CC1310). Carried over for PARITY
 * (D6) so the unchanged CC1310 RF firmware interoperates byte-for-byte.
 *
 * WIRE COMPATIBILITY:
 *   - MessageStruct crosses TWO links: M4F <-> CC1310 (SPI) and M4F <-> A53 (RPMsg).
 *   - Both M4F (ARM32) and A53 (AArch64) are little-endian; layout matches because
 *     the struct uses only uint8_t / float / int32_t (no enums-as-fields, no longs,
 *     no pointers) -> identical size & alignment on both ABIs and on CC1310.
 *   - Node types / commands are #define constants (NOT C enums) so a struct field
 *     is never enum-sized (TI clang may use -fshort-enums; gcc uses int) -> never
 *     embed an enum in a wire struct.
 *   - Layout is naturally padding-free here (4x uint8_t header, then a 4-aligned
 *     union of float/int32_t members), so it matches gen1 with int -> int32_t only.
 */
#ifndef NODE_PROTOCOL_H
#define NODE_PROTOCOL_H

#include <stdint.h>

/* ========================================================================= *
 * NODE TYPES (wire-stable values - NEVER renumber; CC1310 RF depends on them)
 * ========================================================================= */
#define NODE_SOLAR_CONTROLLER        0u
#define NODE_BUFOR_CONTROLLER        1u
#define NODE_CURTAINS_CONTROLLER     2u
#define NODE_LIGHT_CONTROLLER        3u
#define NODE_VENTILATION_CONTROLLER  4u
#define NODE_SMARTPHONE              5u
#define NODE_TH_SENSOR               6u  /* gen2: ambient temperature + humidity */

/* ========================================================================= *
 * COMMANDS (solar controller; extend per node type as the model grows)
 * ========================================================================= */
#define CMD_SEND_DATA_TO_DB    0u
#define CMD_SEND_PUMP_STATUS   1u
#define CMD_TURN_PUMP_ON_OFF   2u
#define CMD_SEND_TEXT_MSG      3u
/* --- provisioning (gen2) --- */
#define CMD_JOIN_REQUEST       4u  /* node->gw: src 0xFF, type=node type, payload.joinData       */
#define CMD_JOIN_ACCEPT        5u  /* gw->node: dest 0xFF, payload.joinAcceptData (matched by factory_id) */
#define CMD_REMOVE             6u  /* gw->node: dest = node addr; node erases its address -> unprovisioned (0xFF). User-initiated (device removed from the app). */
#define CMD_UNREGISTERED       7u  /* gw->node: dest = node addr; REACTIVE - the (addr, factory_id) in an uplink did not match the gateway's binding. Same node action as REMOVE (erase address, go silent); distinct code so the reason is visible (impersonation/stale chip vs. user delete). Node acts only if the frame factory_id == its own FCFG. */

#define NODE_TEXT_MAX  30u  /* textData.text capacity (gen1) */
#define NODE_FACTORY_ID_LEN 8u  /* CC1310 FCFG IEEE address, identifies a chip pre-provisioning */

/* ========================================================================= *
 * ADDRESSES (1-byte). The gen2 gateway is 0x00; the legacy gen1 gateway stays
 * 0xF0, so the CC1310 RX address filter (rxMsg[0] == CONCENTRATOR_ADDRESS) keeps
 * the two networks isolated during dev: gen2 nodes target 0x00, the old
 * fixed-address nodes (solar 0xF1, bufor 0xF2) still talk to gen1 on 0xF0 until
 * reflashed. NOTE: the wire gateway address lives ONLY in the CC1310 firmware
 * (CONCENTRATOR_ADDRESS in radio_task.c / rfEchoTx.c), never in MessageStruct.id;
 * this constant is for documentation + future M4F/Go use. Provisioning assigns
 * node addresses from the pool below.
 * ========================================================================= */
#define ADDR_GATEWAY        0x00u  /* gen2 concentrator (gen1 legacy = 0xF0) */
#define ADDR_UNPROVISIONED  0xFFu  /* a node with no assigned address yet (JOIN src/dest) */
#define ADDR_POOL_FIRST     0x10u  /* first assignable node address           */
#define ADDR_POOL_LAST      0xEFu  /* last  assignable node address (~224 nodes) */

/* ========================================================================= *
 * RF FRAME TAG (byte [1] of the over-the-air frame). Versions the wire so old
 * and new nodes coexist without a flag day (Docs/NODE-MANAGEMENT.md §12.2):
 *   'D' (legacy): [dest]['D'][src][MessageStruct:len][seq][crc8]        - no factory_id
 *   'E' (v2):     [dest]['E'][src][factory_id:8][MessageStruct:len][seq][crc8]
 * The gateway branches on this byte: 'D' -> factory_id = all-zero (unvalidated:
 * gen1 sniff or a not-yet-upgraded node); 'E' -> the frame carries the chip id
 * of the node it concerns (sender on uplink, target on downlink).
 * ========================================================================= */
#define RF_FRAME_TAG_LEGACY  'D'
#define RF_FRAME_TAG_V2      'E'

/* ========================================================================= *
 * MessageStruct - one node<->gateway message.
 * Layout MUST stay identical to gen1 (CC1310 interop). `int` -> `int32_t` only.
 * ========================================================================= */
typedef struct {
    uint8_t id;
    uint8_t type;     /* NODE_*  */
    uint8_t cmd;      /* CMD_*   */
    uint8_t length;
    union {
        struct {
            float   Tin;
            float   Tout;
            float   T4;
            float   T3;
            float   T2;
            float   T1;
            float   Tcol;
            int32_t energyGain;
            int32_t flowRate;
            uint8_t pumpState;
        } solarData;

        struct {
            uint8_t pumpState;
        } pumpData;

        struct {
            float sBuforTemp;
        } buforData;

        struct {
            float    temperature;   /* deg C  */
            float    humidity;      /* %RH    */
            uint16_t batt_mv;       /* BQ35100 Voltage(), mV (0 = n/a)              */
            uint8_t  soh_pct;       /* BQ35100 StateOfHealth, % (SOH mode; 0 = n/a) */
            int32_t  acc_uah;       /* BQ35100 AccumulatedCapacity, cumulative used uAh (ACC mode) */
        } thData;

        /* provisioning: node -> gateway (CMD_JOIN_REQUEST). The node's type is in
         * MessageStruct.type; factory_id identifies the physical chip pre-address.
         * capabilities = bitmask of node-executable actions the node supports (bit =
         * NODE_CAP(ACTION_*), see automation.h). Stored on approve as node.capabilities
         * and drives the app's Action editor; opaque here (bit meanings live in
         * automation.h). 0 = a sensor-only node (no actions). */
        struct {
            uint8_t  factory_id[NODE_FACTORY_ID_LEN];
            uint32_t capabilities;
        } joinData;

        /* provisioning: gateway -> node (CMD_JOIN_ACCEPT), sent to ADDR_UNPROVISIONED;
         * the node acts only if factory_id matches its own, then stores assigned_addr. */
        struct {
            uint8_t factory_id[NODE_FACTORY_ID_LEN];
            uint8_t assigned_addr;
        } joinAcceptData;

        struct {
            char text[NODE_TEXT_MAX];
        } textData;
    } payload;
} MessageStruct;

/* ========================================================================= *
 * NodeFrame - the identity-tagged envelope that carries a MessageStruct across
 * the internal links (CC1310 <-> M4F over SPI, M4F <-> A53 over RPMsg) once the
 * RF frame has been de-framed. `factory_id` = the chip this frame concerns:
 * the SENDER on uplink (which node reported), the TARGET on downlink (which node
 * a command is for). All-zero = legacy/unknown (a gen1-sniff frame or an old 'D'
 * node) -> the gateway does NOT validate or react. MessageStruct stays 44 B; the
 * id travels here, alongside it, not inside it. 8 (align 1) + 44 (align 4) at
 * offset 8 -> no padding, sizeof = 52.
 * ========================================================================= */
typedef struct {
    uint8_t       factory_id[NODE_FACTORY_ID_LEN];  /* [0..7]  chip id, 0 = unknown/legacy */
    MessageStruct msg;                              /* [8..51] the message */
} NodeFrame;

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(NodeFrame) == 52, "NodeFrame must be 52 bytes (8 + 44)");
#endif

/* ========================================================================= *
 * NodesData - authoritative live snapshot held by the engine (M4F).
 * Sent to Linux as MSG_NODE_STATE for phone/monitoring. M4F<->A53 only
 * (not over SPI), so explicit padding is added for a deterministic layout.
 * ========================================================================= */
typedef struct {
    float   Tin;
    float   Tout;
    float   T4;
    float   T3;
    float   T2;
    float   T1;
    float   Tcol;
    int32_t energyGain;
    int32_t flowRate;
    float   sBuforTemp;
    uint8_t pumpState;
    uint8_t _pad[3];   /* keep total size 4-aligned / deterministic */
} NodesData;

#endif /* NODE_PROTOCOL_H */
