/*
 * mcp3421.h - MCP3421 18-bit delta-sigma ADC (I2C) battery-voltage reader.
 *
 * rev-2 T&H node: SoC is voltage-based (no gauge IC). The LFP cell (2.5-3.6 V)
 * exceeds both the MCP3421's 2.048 V reference and the 3.3 V rail, so it is read
 * through a gated 1:2 resistive divider (1 MΩ/1 MΩ). Vbat = 2 * Vadc (* cal).
 *
 * The divider + MCP3421 are powered by PERIPH_EN (see th_sense.c) - enable, settle,
 * read, disable. Read at a quiescent point (before the RF TX pulse) and only every
 * Nth wake: SoC moves slowly, so decouple it from the per-wake T/H cadence.
 *
 * Addr 0x68 (fixed on the base part). One-shot 18-bit (15.625 µV/LSB, PGA=1).
 */
#ifndef MCP3421_H
#define MCP3421_H

#include <stdbool.h>
#include <stdint.h>
#include <ti/drivers/I2C.h>

/* 1-point calibration: measured Vbat / reported Vbat (folds the divider's real
 * ratio + reference error into one gain). 1.0 until calibrated on the board. */
#ifndef MCP3421_CAL
#define MCP3421_CAL   1.0f
#endif

/* One 18-bit one-shot conversion -> ADC input microvolts (already signed).
 * Blocks up to ~350 ms polling the RDY flag. false on I2C error / timeout. */
bool mcp3421_read_uv(I2C_Handle i2c, int32_t *out_uV);

/* Battery voltage in mV: averages `samples` conversions, applies the 1:2 divider
 * and MCP3421_CAL. `samples` clamped to 1..16. false on any conversion failure. */
bool mcp3421_read_batt_mv(I2C_Handle i2c, uint16_t *out_mV, uint8_t samples);

#endif /* MCP3421_H */
