package com.aitronic.smarthome.data

import com.aitronic.smarthome.domain.model.*

/**
 * Abstrakcja źródła danych apki. Docelowo implementacja robi kaskadę:
 *   1. bramka po LAN (WiFi, dom) — PRIMARY
 *   2. bramka zdalnie (port-forward teraz / relay VPS później)
 *   3. mirror MySQL przez PHP API — tylko gdy bramka całkowicie nieosiągalna (Premium)
 * Zapisy (reguły/urządzenia/config) zawsze celują w bramkę (źródło prawdy + aktuator).
 *
 * Na razie: SampleRepository (dane z design handoffu) — pozwala budować UI bez sieci.
 * Stage 2 podmieni impl na Ktor bez zmiany UI.
 */
interface SmartHomeRepository {
    fun dashboard(): DashboardData

    fun climate(): ClimateState
    fun climateSeries(metric: ClimateMetric, range: HistoryRange): Series

    fun solar(): SolarState
    fun solarPeriods(range: SolarRange): List<SolarPeriod>

    fun rules(): List<Rule>
    fun devices(): List<Device>
    fun rooms(): List<String>
    fun deviceTypes(): List<DeviceType>
}
