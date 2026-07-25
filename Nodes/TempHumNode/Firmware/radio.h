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
 * hold our node (source) address.
 *   factoryId: our 8-byte FCFG id -> frame tag 'E' (carries it, so the gateway can
 *              key its §14 pending-command table). NULL -> legacy 'D' frame (JOIN).
 *   nackCmd:   out. If the gateway replies with a NACK instead of an ACK, *nackCmd
 *              gets the delivered CMD_* (e.g. CMD_UNREGISTERED); 0 on a normal ACK.
 *              Pass NULL if you don't handle NACKs.
 * Returns true if the gateway replied (ACK or NACK), false if no reply after retries. */
bool radio_send_message(const MessageStruct *msg, uint8_t destAddr,
                        const uint8_t *factoryId, uint8_t *nackCmd);

/* After sending a JOIN_REQUEST, listen up to timeoutMs for the gateway's
 * JOIN_ACCEPT for THIS chip (an 'E' downlink to dest 0xFF carrying our factory_id).
 * On success sets *assignedAddr to the gateway-assigned RF address, ACKs the
 * gateway, and returns true. factoryId = our 8-byte FCFG id. */
bool radio_wait_join_accept(const uint8_t *factoryId, uint8_t *assignedAddr, uint32_t timeoutMs);

#endif /* RADIO_H */
