/*
 * sensors.h - SHT35 (temp/hum) + BQ35100 (battery, SOH mode) over I2C.
 *
 * BQ35100 in ACCUMULATOR mode (coulomb) - used here to MEASURE per-cycle current
 * consumption (AccumulatedCapacity = used uAh). Commissioning sets ACC mode +
 * design capacity + NEW_BATTERY; each wake runs a gauging session bracketing the
 * RF load. Constants/sequence from the Pavel Slama mbed BQ35100 lib + u-blox driver.
 *
 * Per-wake order: init -> commission(once) -> gauge_begin -> reads
 *                 -> (RF send = load, by caller) -> gauge_end -> close
 */
#ifndef SENSORS_H
#define SENSORS_H

#include <stdbool.h>
#include <stdint.h>

bool sensors_init(void);
void sensors_close(void);

/* One-time gauge setup: SOH mode + design capacity + NEW_BATTERY. Idempotent
 * (no-op once DesignCapacity already reads the target). Returns true if the
 * gauge is configured (verified by read-back). */
bool sensors_commission(void);

/* Gauging session that must bracket the load pulse (RF TX). */
bool sensors_gauge_begin(void);   /* GAUGE_START + wait GA=1     */
void sensors_gauge_end(void);     /* GAUGE_STOP  + wait G_DONE   */

bool sensors_read_th(float *tempC, float *rh);
bool sensors_read_mcp3421_mv(uint16_t *mv);   /* rev-2 battery: MCP3421 + 1:2 divider -> mV */
bool sensors_read_batt_mv(uint16_t *mv);     /* Voltage() 0x08, mV                */
bool sensors_read_soh(uint8_t *soh_pct);     /* StateOfHealth 0x2E, %            */
bool sensors_read_used_uah(int32_t *used_uah);/* AccumulatedCapacity 0x02, used uAh (ACC) */
bool sensors_read_opconfig(uint8_t *oc);     /* DIAG: Operation Config A byte (GMSEL=bits[1:0]) */

#endif /* SENSORS_H */
