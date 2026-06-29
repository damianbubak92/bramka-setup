/**
 * spi_frame.h - SPI transport frame between the M4F (SPI master) and the CC1310
 * (SPI slave). SHARED layout: keep this file byte-identical on both sides
 * (M4F: m4f-firmware via repo; CC1310: cc1310-firmware copied into the CCS
 * project). See docs/ARCHITECTURE-GEN2.md sec.3.
 *
 * Fixed 128-byte frame, half-duplex per direction (the passive side sends a NOP
 * frame). The payload carries a MessageStruct (node_protocol.h). Transport is
 * agnostic to the payload contents - it only frames + CRCs raw bytes.
 *
 * Wire conventions (match the RPMsg side): CRC16-CCITT, init 0xFFFF, poly
 * 0x1021, stored big-endian in the last two bytes, computed over bytes [0..125]
 * (header + payload, excluding the CRC field itself). Each side computes the CRC
 * with its own routine (M4F: protocol_crc16(); CC1310: local copy) - this header
 * defines only the layout + constants, no implementation, so it has zero deps.
 */
#ifndef SPI_FRAME_H
#define SPI_FRAME_H

#include <stdint.h>

#define SPI_FRAME_SIZE         128u
#define SPI_FRAME_MAGIC        0xA5u
#define SPI_FRAME_HDR_LEN      8u    /* bytes [0..7] */
#define SPI_FRAME_CRC_LEN      2u    /* bytes [126..127], big-endian */
#define SPI_FRAME_PAYLOAD_MAX  (SPI_FRAME_SIZE - SPI_FRAME_HDR_LEN - SPI_FRAME_CRC_LEN) /* 118 */
#define SPI_FRAME_CRC_OFFSET   (SPI_FRAME_SIZE - SPI_FRAME_CRC_LEN)                     /* 126 */

/* Frame types (byte [1]). NOP = idle filler the passive side clocks out. */
#define SPI_FRAME_NOP          0x00u  /* no payload; keeps the link clocking */
#define SPI_FRAME_NODE_DATA    0x01u  /* CC1310 -> M4F: a node reading (MessageStruct) */
#define SPI_FRAME_NODE_CMD     0x02u  /* M4F -> CC1310: a command for a node (MessageStruct) */
#define SPI_FRAME_ACK          0x03u  /* acknowledgement of seq */

/* 128-byte on-wire frame. All fields are byte-granular so the struct is exactly
 * 128 bytes with no padding on either toolchain. Do NOT add wider fields. */
typedef struct {
    uint8_t magic;       /* [0]   SPI_FRAME_MAGIC (0xA5) */
    uint8_t type;        /* [1]   SPI_FRAME_* */
    uint8_t seq;         /* [2]   sender sequence (for ACK / dedup) */
    uint8_t pending;     /* [3]   frames still queued at sender -> master drains until 0 */
    uint8_t len;         /* [4]   payload length, <= SPI_FRAME_PAYLOAD_MAX */
    uint8_t flags;       /* [5]   reserved (0) */
    uint8_t reserved0;   /* [6]   reserved (0) */
    uint8_t reserved1;   /* [7]   reserved (0) */
    uint8_t payload[SPI_FRAME_PAYLOAD_MAX];  /* [8..125] */
    uint8_t crc_be[SPI_FRAME_CRC_LEN];       /* [126..127] CRC16-CCITT, big-endian */
} SpiFrame;

/* The frame must be exactly 128 bytes on both toolchains (all byte fields ->
 * no padding). Guarded so it compiles on pre-C11 too. */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(SpiFrame) == SPI_FRAME_SIZE, "SpiFrame must be 128 bytes");
#endif

#endif /* SPI_FRAME_H */
