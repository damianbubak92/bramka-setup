/*
 * battery_soc.c - LiFePO4 voltage -> SoC LUT (see battery_soc.h).
 */
#include "battery_soc.h"
#include <stdbool.h>

/* Generic LFP resting-discharge knees (mV, %), high -> low. The plateau
 * (~3.20-3.30 V) is intentionally coarse; the knees are dense so "full" and
 * "replace soon" resolve sharply. CALIBRATE against the real cell. */
typedef struct { uint16_t mv; uint8_t pct; } SocKnee;

static const SocKnee SOC_LUT[] = {
    { 3400, 100 },
    { 3350,  98 },
    { 3320,  90 },
    { 3300,  80 },
    { 3280,  65 },
    { 3260,  50 },
    { 3240,  35 },
    { 3220,  22 },
    { 3200,  15 },
    { 3150,  10 },
    { 3100,   6 },
    { 3000,   3 },
    { 2900,   1 },
    { 2500,   0 },
};
#define SOC_LUT_N  (int)(sizeof(SOC_LUT) / sizeof(SOC_LUT[0]))

/* Below this, flag "low battery" (still above the pack UVP ~2.0-2.4 V). */
#define SOC_LOW_MV  3050

uint8_t battery_soc_pct(uint16_t batt_mv)
{
    if (batt_mv >= SOC_LUT[0].mv) return 100;
    if (batt_mv <= SOC_LUT[SOC_LUT_N - 1].mv) return 0;

    int i;
    for (i = 0; i < SOC_LUT_N - 1; i++) {
        uint16_t vHi = SOC_LUT[i].mv,     vLo = SOC_LUT[i + 1].mv;
        if (batt_mv <= vHi && batt_mv >= vLo) {
            uint8_t pHi = SOC_LUT[i].pct,  pLo = SOC_LUT[i + 1].pct;
            /* linear interp between (vLo,pLo) and (vHi,pHi) */
            int32_t span = (int32_t)vHi - (int32_t)vLo;   /* > 0 */
            int32_t num  = (int32_t)(batt_mv - vLo) * ((int32_t)pHi - (int32_t)pLo);
            return (uint8_t)((int32_t)pLo + num / span);
        }
    }
    return 0;   /* unreachable (covered by the clamps above) */
}

bool battery_is_low(uint16_t batt_mv)
{
    return batt_mv != 0 && batt_mv < SOC_LOW_MV;
}
