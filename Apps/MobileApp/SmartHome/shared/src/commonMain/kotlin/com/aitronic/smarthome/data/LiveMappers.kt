package com.aitronic.smarthome.data

import com.aitronic.smarthome.data.net.NodeTypes
import com.aitronic.smarthome.data.net.Params
import com.aitronic.smarthome.domain.model.ClimateState
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

/**
 * Uzysk dzienny [kWh]. UWAGA: `energyGain` w telemetrii to surowy przyrost 2-min
 * (kWh×10000), NIE wartość narastająca — więc nie da się go policzyć z jednego
 * odczytu. Bramka akumuluje go w dobie (solar_history.energy_gain) i podaje gotowy
 * przez `energyDayKwh` (state → VIEW solar_state). "—" gdy brak danych.
 */
fun GatewayState.solarDailyYieldKwh(): Double? =
    firstOfType(NodeTypes.SOLAR)?.let { cur ->
        telemetry.entries.firstOrNull { it.value === cur }?.key?.let { energyDayKwh[it] }
    }

/**
 * Moc chwilowa [kW] = 30 × przyrost_2min / 10000 — DOKŁADNIE ta sama formuła co
 * VIEW solar_state na bramce. `energyGain` w telemetrii to już ten przyrost, więc
 * liczymy wprost z BIEŻĄCEGO odczytu. (Wcześniej różnicowaliśmy dwa kolejne odczyty
 * `now - prev` — a że oba to przyrosty ~1000, różnica ~30 dawała fałszywe ~0,1 kW.)
 * Przyrost jest zawsze ≥ 0, reset o północy dotyczy akumulatu, nie przyrostu.
 */
fun GatewayState.solarPowerKw(): Double? {
    val cur = firstOfType(NodeTypes.SOLAR) ?: return null
    cur.params[Params.ENERGY_GAIN]?.let { return 30.0 * it / 10_000.0 }
    // zanim przyjdzie 1. telemetria — wartość policzona przez bramkę (state → VIEW)
    val addr = telemetry.entries.firstOrNull { it.value === cur }?.key ?: return null
    return powerKwHint[addr]
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

// ---------------- Per-node (dashboard: jedna karta na noda) ----------------
// Wersje adresowane konkretnego noda — dashboard iteruje realne nody i renderuje kartę
// każdego osobno (gen1 i gen2 tego samego typu = osobne karty), zamiast agregować przez
// firstOfType. `telemetry` jest kluczowane adresem RF.

/** Moc chwilowa [kW] konkretnego noda solarnego (adres). */
fun GatewayState.solarPowerKwFor(address: Int): Double? {
    val t = telemetry[address] ?: return null
    t.params[Params.ENERGY_GAIN]?.let { return 30.0 * it / 10_000.0 }
    return powerKwHint[address] // zanim przyjdzie 1. telemetria (state → VIEW)
}

/** Uzysk dzienny [kWh] konkretnego noda solarnego (bramka akumuluje → energyDayKwh). */
fun GatewayState.solarDailyYieldKwhFor(address: Int): Double? = energyDayKwh[address]

/**
 * Stan solarny konkretnego noda. `injectAux` = true tylko dla gen1 (legacy): drugi bufor
 * bierzemy z legacy-noda bufora (sniff 242, wtopiony w kartę gen1). Dla gen2 injectAux=false
 * → `auxTankC = NaN` → UI rysuje "—" (źródło 2. bufora będzie konfigurowalne w settings karty).
 */
fun GatewayState.solarStateFor(address: Int, injectAux: Boolean): SolarState? {
    val t = telemetry[address] ?: return null
    val p = t.params
    val flow = p[Params.FLOW_RATE] ?: 0.0
    val aux = if (injectAux) param(NodeTypes.BUFOR, Params.SBUF_TEMP) else null
    return SolarState(
        powerKw = solarPowerKwFor(address)?.let { "${fmt2(it)} kW" } ?: "— kW",
        collectorC = p[Params.TCOL] ?: 0.0,
        mainTankTemps = listOf(
            p[Params.T4] ?: 0.0, p[Params.T3] ?: 0.0, p[Params.T2] ?: 0.0, p[Params.T1] ?: 0.0,
        ),
        auxTankC = aux ?: Double.NaN, // NaN → "—" w UI
        collectorPumpPct = flow.toInt(),
        collectorPumpOn = flow > 0.0,
        auxPumpOn = (p[Params.PUMP_STATE] ?: 0.0) >= 0.5,
    )
}

/** Stan klimatu konkretnego noda T&H (adres). NaN/-1 → UI rysuje "—". */
fun GatewayState.climateStateFor(address: Int): ClimateState? {
    val t = telemetry[address] ?: return null
    val p = t.params
    val hum = p[Params.HUMIDITY]
    return ClimateState(
        tempC = p[Params.TEMPERATURE] ?: Double.NaN,
        humidity = hum?.toInt() ?: -1,
        batteryPct = -1,  // batt_mv → % wymaga LUT (rev2) — na razie "—"
        lastMeasuredLabel = "",
        intervalMin = 0,  // brak komendy interwału w bramce — "—"
    )
}

/** Format PL z 2 miejscami (moc). */
private fun fmt2(v: Double): String {
    val r = round(v * 100).toLong()
    val whole = r / 100
    val frac = ((if (r < 0) -r else r) % 100).toString().padStart(2, '0')
    return "$whole,$frac"
}
