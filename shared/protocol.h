/**
 * protocol.h - Reliable RPMsg protocol between Linux Go service and M4F MCU.
 *
 * Single source of truth for both sides. Used via cgo on Go side.
 *
 * Wire format (big-endian for portability):
 *   [1 byte: type] [2 bytes: seq] [2 bytes: payload_len] [N bytes: payload] [2 bytes: CRC16]
 *
 * Header is __packed__ for binary compatibility.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* ========================================================================= *
 * VERSION
 * ========================================================================= */

#define PROTOCOL_VERSION   1u

/* ========================================================================= *
 * LIMITS
 * ========================================================================= */

/* Max payload size in single message. RPMsg max is ~496 bytes total,
 * minus header (5) and CRC (2) = 489. Round down for safety. */
#define MAX_PAYLOAD_SIZE   480u

/* Max simultaneous unacknowledged messages on either side.
 * Each entry uses payload buffer + metadata. 8 × 480B = 3.84 KB per side. */
#define MAX_PENDING_ACKS   8u

/* Maximum retransmissions of a DATA/EVENT message before giving up */
#define MAX_RETRIES        3u

/* ========================================================================= *
 * TIMEOUTS (milliseconds)
 * ========================================================================= */

/* Time to wait for ACK before retrying */
#define ACK_TIMEOUT_MS         1000u

/* Idle time before sending PING (smart heartbeat) */
#define HEARTBEAT_IDLE_MS      5000u

/* Time without ANY message from peer = connection dead */
#define HEARTBEAT_DEAD_MS     15000u

/* ========================================================================= *
 * MESSAGE TYPES
 * ========================================================================= */

#define MSG_HELLO         0x01u  /* Linux -> M4F: connection request */
#define MSG_HELLO_ACK     0x02u  /* M4F -> Linux: connection accepted */
#define MSG_PING          0x03u  /* Bidir: heartbeat probe */
#define MSG_PONG          0x04u  /* Bidir: heartbeat reply */
#define MSG_DATA          0x10u  /* Bidir: application data (needs ACK) */
#define MSG_ACK           0x11u  /* Bidir: ack for DATA/EVENT seq=N */
#define MSG_EVENT         0x20u  /* M4F -> Linux: async event (needs ACK) */
#define MSG_ERROR         0xFFu  /* Bidir: protocol error notification */

/* ========================================================================= *
 * CONNECTION STATES
 * ========================================================================= */

#define CONN_NOT_CONNECTED   0u
#define CONN_CONNECTED       1u
#define CONN_DEAD            2u

/* ========================================================================= *
 * WIRE FORMAT
 * ========================================================================= */

/* On-wire message header.
 * IMPORTANT: __packed__ ensures no compiler padding.
 * Sequence numbers and length are big-endian on the wire for portability. */
typedef struct __attribute__((packed)) {
    uint8_t  type;          /* MSG_* constants */
    uint16_t seq;           /* sequence number (BE on wire) */
    uint16_t payload_len;   /* length of payload that follows (BE on wire) */
    /* uint8_t payload[payload_len];  follows */
    /* uint16_t crc16;                follows after payload (BE on wire) */
} msg_header_t;

/* Fixed sizes derived from struct */
#define MSG_HEADER_SIZE   5u   /* sizeof(msg_header_t) */
#define MSG_CRC_SIZE      2u   /* trailing CRC16 */
#define MSG_OVERHEAD      (MSG_HEADER_SIZE + MSG_CRC_SIZE)  /* 7 bytes total */
#define MSG_MAX_TOTAL     (MAX_PAYLOAD_SIZE + MSG_OVERHEAD) /* 487 bytes */

/* ========================================================================= *
 * CRC16-CCITT (polynomial 0x1021, init 0xFFFF, no reflect, no xorout)
 * Compatible with XMODEM CRC. Identical implementation both sides.
 * Inline so Go cgo can use the same function without extra .c file.
 * ========================================================================= */

static inline uint16_t protocol_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFFu;
    size_t i;
    int b;

    for (i = 0; i < len; i++) {
        crc ^= ((uint16_t)data[i]) << 8;
        for (b = 0; b < 8; b++) {
            if (crc & 0x8000u) {
                crc = (crc << 1) ^ 0x1021u;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

/* ========================================================================= *
 * BIG-ENDIAN HELPERS (portable, no platform headers needed)
 * ========================================================================= */

static inline uint16_t protocol_get_u16_be(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline void protocol_put_u16_be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

/* ========================================================================= *
 * ENCODE / DECODE
 *
 * encode: builds wire message into buffer. Returns total length or 0 on error.
 * decode: parses wire message. Returns 0 on success, negative on error.
 * ========================================================================= */

/* Build wire message in buf. Caller provides buffer >= MSG_MAX_TOTAL bytes.
 * Returns number of bytes written, or 0 if payload too large. */
static inline size_t protocol_encode(
    uint8_t *buf,
    uint8_t type,
    uint16_t seq,
    const uint8_t *payload,
    uint16_t payload_len)
{
    if (payload_len > MAX_PAYLOAD_SIZE) {
        return 0u;
    }

    /* Header */
    buf[0] = type;
    protocol_put_u16_be(&buf[1], seq);
    protocol_put_u16_be(&buf[3], payload_len);

    /* Payload */
    if (payload_len > 0u && payload != NULL) {
        size_t i;
        for (i = 0; i < payload_len; i++) {
            buf[MSG_HEADER_SIZE + i] = payload[i];
        }
    }

    /* CRC over header + payload */
    uint16_t crc = protocol_crc16(buf, MSG_HEADER_SIZE + payload_len);
    protocol_put_u16_be(&buf[MSG_HEADER_SIZE + payload_len], crc);

    return MSG_HEADER_SIZE + (size_t)payload_len + MSG_CRC_SIZE;
}

/* Decode error codes (returned as negative values) */
#define PROTOCOL_ERR_TOO_SHORT     -1
#define PROTOCOL_ERR_BAD_LENGTH    -2
#define PROTOCOL_ERR_BAD_CRC       -3

/* Parse wire message. Returns 0 on success, negative on error.
 * On success, *out_type, *out_seq, *out_payload, *out_payload_len are filled.
 * out_payload points INSIDE the buffer (zero-copy). */
static inline int protocol_decode(
    const uint8_t *buf,
    size_t buf_len,
    uint8_t *out_type,
    uint16_t *out_seq,
    const uint8_t **out_payload,
    uint16_t *out_payload_len)
{
    if (buf_len < MSG_OVERHEAD) {
        return PROTOCOL_ERR_TOO_SHORT;
    }

    uint8_t  type = buf[0];
    uint16_t seq  = protocol_get_u16_be(&buf[1]);
    uint16_t plen = protocol_get_u16_be(&buf[3]);

    if (plen > MAX_PAYLOAD_SIZE) {
        return PROTOCOL_ERR_BAD_LENGTH;
    }
    if (buf_len != (size_t)(MSG_HEADER_SIZE + plen + MSG_CRC_SIZE)) {
        return PROTOCOL_ERR_BAD_LENGTH;
    }

    /* Verify CRC */
    uint16_t crc_calc = protocol_crc16(buf, MSG_HEADER_SIZE + plen);
    uint16_t crc_recv = protocol_get_u16_be(&buf[MSG_HEADER_SIZE + plen]);
    if (crc_calc != crc_recv) {
        return PROTOCOL_ERR_BAD_CRC;
    }

    *out_type = type;
    *out_seq = seq;
    *out_payload = (plen > 0u) ? &buf[MSG_HEADER_SIZE] : NULL;
    *out_payload_len = plen;
    return 0;
}

#endif /* PROTOCOL_H */