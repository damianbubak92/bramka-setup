/*
 * th_sense.h - rev-2 T&H node sensing facade: SHT35 (temp/hum) + MCP3421 (battery
 * voltage -> SoC), both on the shared I2C bus, gated by PERIPH_EN.
 *
 * Owns the single I2C_Handle (SHT35 + MCP3421 share SDA/SCL) and the PERIPH_EN
 * line, which powers the SHT35 rail AND the ADC divider (both off in sleep). Usage
 * per wake:
 *     th_sense_periph(true);  th_sense_settle();
 *     th_sense_read_th(&t,&rh);                  // every wake
 *     th_sense_read_batt(&mv,&soc,&low);         // every Nth wake, pre-RF
 *     th_sense_periph(false);
 *
 * SHT35 is the proven rev-1 path; only BQ35100 was dropped (rev-2 has no gauge).
 */
#ifndef TH_SENSE_H
#define TH_SENSE_H

#include <stdbool.h>
#include <stdint.h>

bool th_sense_init(void);    /* I2C_open + PERIPH_EN GPIO config. false on I2C fail. */
void th_sense_close(void);

void th_sense_periph(bool on);   /* drive PERIPH_EN (SHT35 rail + ADC divider) */
void th_sense_settle(void);      /* blocking settle after enabling PERIPH_EN */

bool th_sense_read_th(float *tempC, float *rh);  /* SHT35 single-shot */

/* Battery: MCP3421 -> mV, LUT -> SoC %, low-battery flag. Read PERIPH_EN-on and at a
 * quiescent point (before the RF pulse). false on I2C error. */
bool th_sense_read_batt(uint16_t *mv, uint8_t *soc_pct, bool *low);

bool th_sense_charging(void);    /* nCHRGSTAT: true = charger active (STAT low) */

#endif /* TH_SENSE_H */
