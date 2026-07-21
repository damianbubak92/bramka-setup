/*
 * battery_soc.h - LiFePO4 state-of-charge estimate from resting cell voltage.
 *
 * No gauge IC (rev-2 dropped the BQ35100): SoC comes from a voltage->% lookup on
 * the LFP discharge curve. LFP is a hard case - a long flat plateau (~3.20-3.30 V)
 * over most of the usable range, with sharp knees at both ends. So the LUT is fine
 * near full/empty and deliberately coarse on the plateau: enough for "full /
 * dropping / replace soon", which is all a room sensor needs.
 *
 * Single branch (discharge only): the USB charge is rare (~once/2-3 yr) and we read
 * at a quiescent point, so there is no harvest hysteresis to model.
 *
 * ⚠️ CALIBRATE: the table below is a generic LFP curve. Replace it with the real
 * cell's RESTING discharge points (measure V at known SoC by coulomb counting, NOT
 * OCV sweeping which is meaningless on the flat plateau). See rev2-battery-architecture.
 */
#ifndef BATTERY_SOC_H
#define BATTERY_SOC_H

#include <stdint.h>

/* Resting battery voltage (mV) -> state of charge (0..100 %), linearly
 * interpolated between the LUT knees. Clamped to [0,100]. */
uint8_t battery_soc_pct(uint16_t batt_mv);

/* true once the pack is below the firmware low-battery warning threshold (report a
 * "low battery" flag to the gateway BEFORE the hard UVP in the pack PCM trips). */
bool battery_is_low(uint16_t batt_mv);

#endif /* BATTERY_SOC_H */
