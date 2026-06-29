/*
 * sensors.h - I2C sensors on the T&H node: SHT35 (temp/hum) + BQ35100 (battery).
 * Power-cycled node: each cold boot we open I2C, take one snapshot, close.
 */
#ifndef SENSORS_H
#define SENSORS_H

#include <stdbool.h>
#include <stdint.h>

/* Open I2C (Board_I2C0 @ 100 kHz). Returns false on failure. */
bool sensors_init(void);

/* One SHT35 high-repeatability measurement. tempC in °C, rh in %RH. */
bool sensors_read_th(float *tempC, float *rh);

/* BQ35100 battery voltage snapshot (mV). NOTE: with full power-off between
 * cycles the gauge can't coulomb-count; this is just the instantaneous voltage. */
bool sensors_read_batt_mv(uint16_t *mv);

void sensors_close(void);

#endif /* SENSORS_H */
