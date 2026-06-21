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

#define NODE_TEXT_MAX  30u  /* textData.text capacity (gen1) */

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
            float temperature;   /* deg C  */
            float humidity;      /* %RH    */
        } thData;

        struct {
            char text[NODE_TEXT_MAX];
        } textData;
    } payload;
} MessageStruct;

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
