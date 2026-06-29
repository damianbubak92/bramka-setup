/**
 * spi_frame.h - SPI transport frame between the M4F (SPI master) and the CC1310
 * (SPI slave). SHARED layout: MUST stay byte-identical to the M4F copy
 * (bramka-setup/shared/spi_frame.h). See docs/ARCHITECTURE-GEN2.md sec.3.
 *
 * Fixed 128-byte frame, half-duplex per direction (passive side sends a NOP).
 * Payload carries a MessageStruct. CRC16-CCITT (init 0xFFFF, poly 0x1021),
 * big-endian in the last two bytes, over bytes [0..125].
 */
#ifndef SPI_FRAME_H
#define SPI_FRAME_H

#include <stdint.h>

#define SPI_FRAME_SIZE         128u
#define SPI_FRAME_MAGIC        0xA5u
#define SPI_FRAME_HDR_LEN      8u
#define SPI_FRAME_CRC_LEN      2u
#define SPI_FRAME_PAYLOAD_MAX  (SPI_FRAME_SIZE - SPI_FRAME_HDR_LEN - SPI_FRAME_CRC_LEN) /* 118 */
#define SPI_FRAME_CRC_OFFSET   (SPI_FRAME_SIZE - SPI_FRAME_CRC_LEN)                     /* 126 */

#define SPI_FRAME_NOP          0x00u
#define SPI_FRAME_NODE_DATA    0x01u  /* CC1310 -> M4F: a node reading (MessageStruct) */
#define SPI_FRAME_NODE_CMD     0x02u  /* M4F -> CC1310: a command for a node (MessageStruct) */
#define SPI_FRAME_ACK          0x03u

typedef struct {
    uint8_t magic;       /* [0]   SPI_FRAME_MAGIC */
    uint8_t type;        /* [1]   SPI_FRAME_* */
    uint8_t seq;         /* [2]   sender sequence */
    uint8_t pending;     /* [3]   frames still queued at sender */
    uint8_t len;         /* [4]   payload length, <= SPI_FRAME_PAYLOAD_MAX */
    uint8_t flags;       /* [5]   reserved (0) */
    uint8_t reserved0;   /* [6]   reserved (0) */
    uint8_t reserved1;   /* [7]   reserved (0) */
    uint8_t payload[SPI_FRAME_PAYLOAD_MAX];  /* [8..125] */
    uint8_t crc_be[SPI_FRAME_CRC_LEN];       /* [126..127] CRC16-CCITT, big-endian */
} SpiFrame;

#endif /* SPI_FRAME_H */
