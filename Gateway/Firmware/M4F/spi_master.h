/**
 * spi_master.h - M4F SPI master link to the CC1310 (SPI slave).
 *
 * MCU_SPI0 (MCSPI) in INTERRUPT + CALLBACK mode (non-blocking), plus a 2-line
 * GPIO handshake (MASTER_READY out, SLAVE_READY in/IRQ). Carries 128-byte frames
 * (shared/spi_frame.h). Ports the gen1 choreography (CC1310 master + CC3235
 * slave), roles reversed. See docs/ARCHITECTURE-GEN2.md sec.3.
 *
 * Wiring into the engine: received node readings (FRAME_NODE_DATA) are posted to
 * the engine input queue; commands to nodes (FRAME_NODE_CMD) are queued via
 * spi_master_post_cmd() (called from the comms-side nodeTxSink).
 */
#ifndef SPI_MASTER_H
#define SPI_MASTER_H

#include <stdbool.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "node_protocol.h"

/* Bring up MCSPI + handshake GPIO + spawn the SPI task. Received node readings
 * are posted to nodeInQueue (consumed by the engine task). Call once after
 * Drivers_open(). */
void spi_master_init(QueueHandle_t nodeInQueue);

/* Queue a command MessageStruct to send to a node over SPI (FRAME_NODE_CMD).
 * Non-blocking; returns false if the out queue is full. Called from the comms
 * task (nodeTxSink). */
bool spi_master_post_cmd(const MessageStruct *msg);

/* Graceful shutdown: wake the SPI task so it exits promptly (it tears down the
 * SLAVE_READY GPIO-IRQ - HwiP + bank intr + Sciclient introuter route - on exit),
 * then block until it has fully stopped or timeoutMs elapses. MUST be called
 * (from the comms task) BEFORE Drivers_close()/System_deinit(): a live NVIC Hwi
 * or DMSC-held introuter route during driver teardown can wedge the remoteproc
 * stop ("M4F won't stop"). Assumes gbShutdown is already set. */
void spi_master_shutdown(uint32_t timeoutMs);

#endif /* SPI_MASTER_H */
