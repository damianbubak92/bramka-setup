package com.aitronic.smarthome.data

import com.aitronic.smarthome.data.net.SolarSeriesDto
import com.aitronic.smarthome.domain.model.AxisTick
import com.aitronic.smarthome.domain.model.SolarPeriod
import com.aitronic.smarthome.domain.model.SolarRange

/**
 * Mapowanie odpowiedzi bramki (command=history, [SolarSeriesDto]) na model wykresu
 * [SolarPeriod]. Bramka daje same liczby + gotową etykietę (PL) — apka dokłada
 * skalę osi X (stała dla zakresu) i formatuje sumy pod locale.
 *
 * Osie X to skala, nie dane: pozycje i podpisy są zaszyte tak jak w handoffie,
 * niezależnie od tego, ile słupków realnie przyszło.
 */

private fun dayTicks() = listOf(0f to "6", 0.2f to "9", 0.4f to "12", 0.6f to "15", 0.8f to "18", 1f to "21")
    .map { AxisTick(it.first, it.second) }
private fun monthTicks() = listOf(0f to "1", 0.33f to "10", 0.66f to "20", 1f to "30")
    .map { AxisTick(it.first, it.second) }
private fun yearTicks() = listOf(0f to "Sty", 0.27f to "Kwi", 0.55f to "Lip", 0.82f to "Paź", 1f to "Gru")
    .map { AxisTick(it.first, it.second) }

/** Podpis osi X dla "Całkowite": po jednym roku na słupek, z bucketów serii. */
private fun totalTicks(dto: SolarSeriesDto): List<AxisTick> {
    val n = dto.bars.size
    if (n <= 1) return listOf(AxisTick(0f, "—"))
    return dto.bars.mapIndexed { i, b ->
        AxisTick(i.toFloat() / (n - 1), yearOf(b.bucket).toString().takeLast(2))
    }
}

/** Rok z uniksowego czasu (bucket = początek roku, więc UTC wystarcza — bez tz-mathu). */
private fun yearOf(unixS: Long): Int = (1970 + unixS / 31_556_952L).toInt()

/** "23,7 kWh" / "612 kWh": bez ułamka gdy duże wartości, przecinek dziesiętny (PL). */
private fun kwh(v: Double): String {
    val s = if (v >= 100) v.toLong().toString() else ((v * 10).toLong() / 10.0).toString().replace('.', ',')
    return "$s kWh"
}

/** "10 h 34 min" z minut; przy dużych sumach same godziny ("295 h"). */
private fun runtime(minutes: Long): String {
    val h = minutes / 60
    val m = minutes % 60
    return when {
        h >= 100 -> "$h h"
        h > 0 -> "$h h $m min"
        else -> "$m min"
    }
}

fun SolarSeriesDto.toPeriod(range: SolarRange): SolarPeriod {
    val ticks = when (range) {
        SolarRange.Day -> dayTicks()
        SolarRange.Month -> monthTicks()
        SolarRange.Year -> yearTicks()
        SolarRange.Total -> totalTicks(this)
    }
    return SolarPeriod(
        label = label.ifEmpty { "—" },
        bars = bars.map { it.energyKwh },
        xTicks = ticks,
        unit = "kWh",
        pumpRuntime = runtime(pumpMinutes),
        energyYield = kwh(energyKwh),
    )
}

fun List<SolarSeriesDto>.toPeriods(range: SolarRange): List<SolarPeriod> = map { it.toPeriod(range) }
