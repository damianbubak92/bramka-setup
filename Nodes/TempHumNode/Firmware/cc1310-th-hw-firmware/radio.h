/*
 * radio.h - one-shot RF send compatible with the gen2 gateway.
 * Frame (matches cc1310 gateway radio_task.c / gen2 node rfEchoTx.c):
 *   [dest][ 'D' ][src][ MessageStruct (length B) ][seq][crc8-xor]
 * PHY = smartrf_settings (same as the gateway). Blocking: TX then a short RX
 * window for the ACK, with retries. Designed for a power-cycled node: open RF,
 * send, close - no event loop.
 */
#ifndef RADIO_H
#define RADIO_H

#include <stdbool.h>
#include <stdint.h>
#include "node_protocol.h"

/* Send one MessageStruct to destAddr (gateway = ADDR_GATEWAY 0x00). msg->id must
 * hold our node (source) address. Returns true if the gateway ACKed. */
bool radio_send_message(const MessageStruct *msg, uint8_t destAddr);

#endif /* RADIO_H */
