package com.aitronic.smarthome.data

import com.aitronic.smarthome.data.net.NodeTypes
import com.aitronic.smarthome.data.net.Params
import com.aitronic.smarthome.domain.model.SolarState
import kotlin.math.round

/**
 * Mapowanie live telemetrii (WS) na modele UI.
 * Semantyka pól — zweryfikowana w node_protocol.h / telemetry.go / store.go:
 *  - `flowRate`  = pompa obiegowa o zmiennej prędkości (%), READ-ONLY
 *  - `pumpState` = pompa dodatkowa (przekaźnik) — TĄ sterujemy PUMP_ON/PUMP_OFF
 *  - `energyGain`= uzysk **narastająco w ciągu doby**, w kWh × 10000
 *  - temperatury zbiornika: T4 (góra) … T1 (dół); `Tcol` = kolektor; `sBuforTemp` = zbiornik dodatkowy (node bufora)
 */

/** Uzysk dzienny [kWh] z narastającego energyGain. */
fun GatewayState.solarDailyYieldKwh(): Double? =
    param(NodeTypes.SOLAR, Params.ENERGY_GAIN)?.let { it / 10_000.0 }

/**
 * Moc chwilowa [kW].
 * 1) Preferujemy własną deltę z dwóch kolejnych odczytów WS (najświeższa).
 * 2) Zanim ją uzbieramy — bierzemy wartość policzoną przez bramkę (VIEW solar_state,
 *    30*energy_gain_delta/10000 z ostatniego rekordu historii). Dzięki temu moc jest
 *    realna od razu po otwarciu apki; "—" zostaje tylko gdy baza jest naprawdę pusta
 *    (nowy node, brak historii).
 */
fun GatewayState.solarPowerKw(): Double? {
    val cur = firstOfType(NodeTypes.SOLAR) ?: return null
    val addr = telemetry.entries.firstOrNull { it.value === cur }?.key ?: return null
    val computed = run {
        val now = cur.params[Params.ENERGY_GAIN] ?: return@run null
        val (prev, _) = prevEnergyGain[addr] ?: return@run null
        val delta = now - prev
        if (delta < 0) null else 30.0 * delta / 10_000.0 // delta<0 = reset o północy
    }
    return computed ?: powerKwHint[addr]
}

/** Pełny stan instalacji solarnej z live telemetrii (null = brak danych z bramki). */
fun GatewayState.solarState(): SolarState? {
    val solar = firstOfType(NodeTypes.SOLAR) ?: return null
    val p = solar.params
    val aux = param(NodeTypes.BUFOR, Params.SBUF_TEMP)
    val flow = p[Params.FLOW_RATE] ?: 0.0
    return SolarState(
        powerKw = solarPowerKw()?.let { "${fmt2(it)} kW" } ?: "— kW",
        collectorC = p[Params.TCOL] ?: 0.0,
        // pozycyjnie od góry: T4, T3, T2, T1
        mainTankTemps = listOf(
            p[Params.T4] ?: 0.0, p[Params.T3] ?: 0.0, p[Params.T2] ?: 0.0, p[Params.T1] ?: 0.0,
        ),
        auxTankC = aux ?: 0.0,
        collectorPumpPct = flow.toInt(),
        collectorPumpOn = flow > 0.0,
        auxPumpOn = (p[Params.PUMP_STATE] ?: 0.0) >= 0.5,
    )
}

/** Format PL z 2 miejscami (moc). */
private fun fmt2(v: Double): String {
    val r = round(v * 100).toLong()
    val whole = r / 100
    val frac = ((if (r < 0) -r else r) % 100).toString().padStart(2, '0')
    return "$whole,$frac"
}
