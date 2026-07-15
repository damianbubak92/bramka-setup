package com.aitronic.smarthome.data

import com.aitronic.smarthome.domain.model.*

/**
 * Dane przykładowe 1:1 z design_handoff_smart_home (CH_DATA, SOLAR, A_RULES, A_DEVLIST...).
 * Służą do budowy UI zanim podłączymy realną bramkę. NIE trafia do produkcji jako źródło.
 */
object SampleRepository : SmartHomeRepository {

    override fun dashboard() = DashboardData(
        greetingName = "Piotr",
        statusLine = "Dom · Wszystko OK",
        solar = solar(),
        solarDailyYield = "23,7 kWh",
        solarMiniBars = listOf(0.3, 0.6, 1.2, 1.9, 2.4, 2.6, 2.1, 1.4),
        climate = climate(),
        pv = PvState(powerKw = "2,84 kW", capacityKwp = "6,0 kWp"),
        rooms = listOf(
            RoomTile("Salon", "22,4°", "Grzeje · 3 światła", "heating"),
            RoomTile("Sypialnia", "19,8°", "Eco · rolety zam.", "heating"),
        ),
    )

    override fun climate() = ClimateState(
        tempC = 21.8,
        humidity = 48,
        batteryPct = 87,
        lastMeasuredLabel = "2 min temu",
        intervalMin = 5,
    )

    override fun climateSeries(metric: ClimateMetric, range: HistoryRange): Series {
        val temp = mapOf(
            HistoryRange.H24 to listOf(19.2,19.0,18.8,18.6,18.5,18.7,19.3,20.1,21.0,21.8,22.4,22.9,23.4,23.8,24.0,23.9,23.5,22.8,22.0,21.3,20.7,20.2,19.8,19.5),
            HistoryRange.D7 to listOf(20.4,21.1,22.0,21.6,20.8,19.9,21.3),
            HistoryRange.Month to listOf(18.5,19.0,19.4,20.0,20.6,21.0,21.3,21.0,20.5,20.0,19.6,20.2,21.0,21.8,22.4,22.8,22.5,21.9,21.2,20.6,20.0,19.5,19.9,20.7,21.5,22.2,22.6,22.0,21.4,20.8),
            HistoryRange.Year to listOf(3.2,4.1,7.5,12.3,16.8,20.4,22.6,22.1,17.5,12.1,7.0,3.8),
        )
        val hum = mapOf(
            HistoryRange.H24 to listOf(58.0,59.0,60.0,61.0,61.0,60.0,58.0,55.0,52.0,50.0,48.0,46.0,45.0,44.0,43.0,44.0,46.0,49.0,52.0,54.0,55.0,56.0,57.0,58.0),
            HistoryRange.D7 to listOf(52.0,49.0,47.0,50.0,54.0,57.0,51.0),
            HistoryRange.Month to listOf(55.0,54.0,52.0,50.0,49.0,51.0,53.0,55.0,57.0,56.0,54.0,52.0,50.0,48.0,47.0,49.0,51.0,53.0,55.0,54.0,52.0,50.0,48.0,50.0,52.0,54.0,56.0,55.0,53.0,51.0),
            HistoryRange.Year to listOf(72.0,70.0,65.0,60.0,55.0,52.0,50.0,53.0,60.0,66.0,71.0,74.0),
        )
        val values = if (metric == ClimateMetric.Temperature) temp.getValue(range) else hum.getValue(range)
        val unit = if (metric == ClimateMetric.Temperature) "°C" else "%"
        return Series(values, xTicks(range), unit)
    }

    private fun xTicks(range: HistoryRange): List<AxisTick> = when (range) {
        HistoryRange.H24 -> listOf(0f to "0:00", 0.25f to "6:00", 0.5f to "12:00", 0.75f to "18:00", 1f to "24:00")
        HistoryRange.D7 -> listOf(0f to "Pn", 1/6f to "Wt", 2/6f to "Śr", 3/6f to "Cz", 4/6f to "Pt", 5/6f to "So", 1f to "Nd")
        HistoryRange.Month -> listOf(0f to "1", 0.33f to "10", 0.66f to "20", 1f to "30")
        HistoryRange.Year -> listOf(0f to "Sty", 0.27f to "Kwi", 0.55f to "Lip", 0.82f to "Paź", 1f to "Gru")
    }.map { AxisTick(it.first, it.second) }

    override fun solar() = SolarState(
        powerKw = "2,84 kW",
        collectorC = 71.2,
        mainTankTemps = listOf(73.2, 64.1, 58.1, 51.5),
        auxTankC = 62.3,
        collectorPumpPct = 78,
        collectorPumpOn = true,
        auxPumpOn = true,
    )

    override fun solarPeriods(range: SolarRange): List<SolarPeriod> = when (range) {
        SolarRange.Day -> listOf(
            SolarPeriod("10 lip 2026", listOf(0.0,0.1,0.7,1.4,2.3,2.5,2.6,2.4,2.5,2.6,2.1,1.6,1.2,0.4,0.0,0.0), dayTicks(), "kWh", "9 h 58 min", "21,4 kWh"),
            SolarPeriod("11 lip 2026", listOf(0.0,0.2,0.9,1.6,2.7,2.6,2.6,2.5,2.4,2.7,2.3,1.7,1.6,0.6,0.0,0.0), dayTicks(), "kWh", "10 h 34 min", "23,7 kWh"),
            SolarPeriod("12 lip 2026", listOf(0.0,0.0,0.5,1.1,1.8,2.2,2.0,1.6,2.1,1.9,1.4,0.9,0.5,0.1,0.0,0.0), dayTicks(), "kWh", "8 h 12 min", "18,9 kWh"),
        )
        SolarRange.Month -> listOf(
            SolarPeriod("cze 2026", listOf(14.0,16.0,19.0,22.0,25.0,21.0,18.0,12.0,9.0,15.0,20.0,24.0,26.0,23.0,19.0,16.0,21.0,25.0,27.0,24.0,20.0,17.0,13.0,18.0,22.0,26.0,28.0,25.0,21.0,19.0), monthTicks(), "kWh", "295 h", "612 kWh"),
            SolarPeriod("lip 2026", listOf(18.0,20.0,23.0,26.0,28.0,24.0,20.0,15.0,12.0,18.0,23.0,27.0,29.0,26.0,22.0,19.0,24.0,28.0,30.0,27.0,23.0,20.0,16.0,21.0,25.0,29.0,31.0,28.0,24.0,22.0), monthTicks(), "kWh", "324 h", "678 kWh"),
        )
        SolarRange.Year -> listOf(
            SolarPeriod("2025", listOf(110.0,170.0,320.0,500.0,660.0,730.0,790.0,760.0,540.0,350.0,175.0,105.0), yearTicks(), "kWh", "2 610 h", "5 015 kWh"),
            SolarPeriod("2026", listOf(120.0,180.0,340.0,520.0,690.0,760.0,820.0,780.0,560.0,360.0,180.0,110.0), yearTicks(), "kWh", "2 720 h", "5 210 kWh"),
        )
        SolarRange.Total -> listOf(
            SolarPeriod("2019 – 2026", listOf(4100.0,4520.0,4780.0,4990.0,5120.0,5080.0,5260.0,3200.0),
                listOf(0f to "19", 0.14f to "20", 0.28f to "21", 0.42f to "22", 0.57f to "23", 0.71f to "24", 0.85f to "25", 1f to "26").map { AxisTick(it.first, it.second) },
                "kWh", "18 240 h", "32 410 kWh"),
        )
    }

    private fun dayTicks() = listOf(0f to "6", 0.2f to "9", 0.4f to "12", 0.6f to "15", 0.8f to "18", 1f to "21").map { AxisTick(it.first, it.second) }
    private fun monthTicks() = listOf(0f to "1", 0.33f to "10", 0.66f to "20", 1f to "30").map { AxisTick(it.first, it.second) }
    private fun yearTicks() = listOf(0f to "Sty", 0.27f to "Kwi", 0.55f to "Lip", 0.82f to "Paź", 1f to "Gru").map { AxisTick(it.first, it.second) }

    override fun rules() = listOf(
        Rule(1, "Pompa ON", true, listOf(
            Condition.Time("14:00", "23:55"),
            Condition.Delta("solar", "T3", "buffer", "sBuforTemp", CompareOp.Gt, 16.0),
        ), RuleAction("solar", 1)),
        Rule(2, "Pompa OFF", true, listOf(
            Condition.Delta("solar", "T3", "buffer", "sBuforTemp", CompareOp.Lt, 8.0),
        ), RuleAction("solar", 0)),
        Rule(3, "Nocne chłodzenie", false, listOf(
            Condition.Time("22:00", "05:00"),
            Condition.Param("solar", "T4", CompareOp.Gt, 70.0),
        ), RuleAction("buffer", 1)),
    )

    override fun devices() = listOf(
        Device(11, "Lampa sufitowa", "light", "Salon", true),
        Device(12, "Roleta okno", "blind", "Salon", true),
        Device(13, "Termostat", "heating", "Salon", true),
        Device(14, "Sterownik solarny", "solar", "Technika", true),
        Device(15, "Falownik PV", "pv", "Technika", true),
        Device(16, "Sterownik bufora", "buffer", "Technika", true),
        Device(17, "Bramka Home", "hub", "Technika", true),
        Device(18, "Czujnik klimatu", "climate", "Bez pokoju", true),
    )

    override fun rooms() = listOf("Salon", "Sypialnia", "Łazienka", "Kuchnia", "Technika", "Bez pokoju")

    override fun deviceTypes() = listOf(
        DeviceType("solar", "Sterownik solarny"),
        DeviceType("buffer", "Sterownik bufora"),
        DeviceType("pv", "Falownik PV"),
        DeviceType("climate", "Czujnik klimatu"),
        DeviceType("light", "Oświetlenie"),
        DeviceType("blind", "Roleta"),
        DeviceType("heating", "Ogrzewanie"),
        DeviceType("hub", "Bramka / Hub"),
    )
}
