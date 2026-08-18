package com.aitronic.smarthome.util

import kotlin.math.roundToInt

/** Aktualny czas w sekundach Unix (zegar lokalny urządzenia). */
expect fun nowEpochSeconds(): Long

/** Lokalna data/godzina z epoch-sekund w formacie "HH:mm dd-MM-yyyy". */
expect fun formatLocalDateTime(epochSeconds: Long): String

/** Lokalna godzina z epoch-sekund w formacie "HH:mm" (etykiety osi wykresu). */
expect fun formatLocalHm(epochSeconds: Long): String

/**
 * Etykieta "Ostatnia aktualizacja: …" wg reguł usera:
 *  - < 1 min           → "teraz"
 *  - 1..60 min         → "N min temu"
 *  - > 60 min          → konkretna godzina i data, np. "12:45 24-07-2026"
 * Brak stempla (ts<=0) → null (UI nie pokazuje wiersza).
 */
fun lastUpdateLabel(tsSeconds: Long, nowSeconds: Long = nowEpochSeconds()): String? =
    relativeTimeLabel(tsSeconds, nowSeconds)?.let { "Ostatnia aktualizacja: $it" }

/**
 * Bare „kiedy" bez prefiksu — dla statusu połączenia („Offline — dane z kopii: <to>"):
 *  teraz / N min temu / "HH:mm dd-MM-yyyy". null gdy brak stempla (ts<=0).
 */
fun relativeTimeLabel(tsSeconds: Long, nowSeconds: Long = nowEpochSeconds()): String? {
    if (tsSeconds <= 0L) return null
    val diff = nowSeconds - tsSeconds
    return when {
        diff < 60L    -> "teraz"
        diff <= 3600L -> "${diff / 60L} min temu"
        else          -> formatLocalDateTime(tsSeconds)
    }
}

/**
 * Przybliżony SoC ogniwa LiFePO4 z napięcia [mV] (płaska krzywa → grubo na plateau,
 * dokładniej na kolanach). Wartość poglądowa; docelowo zastąpi ją skalibrowany
 * `soh_pct` liczony na nodzie (battery_soc.c). Zwraca 0..100.
 */
fun lfpSocPct(mv: Int): Int {
    val lut = intArrayOf(
        2800, 0,  3000, 5,   3100, 10,  3150, 20,  3200, 35,
        3230, 50, 3260, 65,  3290, 80,  3320, 90,  3350, 97,  3450, 100,
    )
    if (mv <= lut[0]) return 0
    if (mv >= lut[lut.size - 2]) return 100
    var i = 2
    while (i < lut.size) {
        val v2 = lut[i]
        if (mv < v2) {
            val v1 = lut[i - 2]; val p1 = lut[i - 1]; val p2 = lut[i + 1]
            val f = (mv - v1).toDouble() / (v2 - v1)
            return (p1 + f * (p2 - p1)).roundToInt().coerceIn(0, 100)
        }
        i += 2
    }
    return 100
}
