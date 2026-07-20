package com.aitronic.smarthome.domain.model

/**
 * Modele domenowe apki SmartHome. Czysty Kotlin (commonMain) — wspólne Android/iOS.
 * Docelowo zasilane z bramki (Go/SQLite) — patrz warstwa data/DataSource.
 * Na razie wypełniane przez data/SampleData zgodnie z design handoffem.
 */

// --- Klimat (Czujnik klimatu, node bateryjny temp+wilgotność) ---

data class ClimateState(
    val tempC: Double,
    val humidity: Int,
    val batteryPct: Int,
    val lastMeasuredLabel: String, // np. "2 min temu"
    val intervalMin: Int,          // 1..5
)

enum class ClimateMetric { Temperature, Humidity }
enum class HistoryRange { H24, D7, Month, Year }

/** Seria historyczna + etykiety osi X (fraction 0..1 -> label). */
data class Series(
    val values: List<Double>,
    val xTicks: List<AxisTick>,
    val unit: String,
)

data class AxisTick(val fraction: Float, val label: String)

// --- System solarny ---

data class SolarState(
    val powerKw: String,        // "2,84 kW"
    val collectorC: Double,     // Tcol
    val mainTankTemps: List<Double>, // 4 wartości od góry (T4..T1 pozycyjnie)
    val auxTankC: Double,       // sBuforTemp
    val collectorPumpPct: Int,  // pompa o zmiennej prędkości (read-only)
    val collectorPumpOn: Boolean,
    val auxPumpOn: Boolean,     // pompa dodatkowa (sterowana ON/OFF)
)

enum class SolarRange(val wire: String) { Day("day"), Month("month"), Year("year"), Total("total") }

/** Uzysk energii dla jednego okresu (dzień/miesiąc/rok/total). */
data class SolarPeriod(
    val label: String,          // "12 lip 2026"
    val bars: List<Double>,
    val xTicks: List<AxisTick>,
    val unit: String,           // "kWh"
    val pumpRuntime: String,    // "10 h 34 min"
    val energyYield: String,    // "23,7 kWh"
)

// --- Automatyzacje ---

enum class SyncState { Synced, Syncing, Error }

sealed interface Condition {
    data class Time(val start: String, val end: String) : Condition
    data class Param(val device: String, val param: String, val op: CompareOp, val value: Double) : Condition
    data class Delta(
        val device1: String, val param1: String,
        val device2: String, val param2: String,
        val op: CompareOp, val min: Double,
    ) : Condition
}

enum class CompareOp { Gt, Lt }

data class RuleAction(val target: String, val value: Int) // value: 0|1 (relay OFF/ON)

data class Rule(
    val id: Long,
    val name: String,
    val enabled: Boolean,
    val conditions: List<Condition>, // połączone logicznym ORAZ
    val action: RuleAction,
    val sync: SyncState = SyncState.Synced,
)

// --- Urządzenia + pokoje ---

data class Device(
    val id: Long,
    val name: String,
    val type: String,   // klucz typu (solar/buffer/pv/climate/light/blind/heating/hub)
    val room: String,   // nazwa pokoju lub "Bez pokoju"
    val online: Boolean,
    val sync: SyncState = SyncState.Synced,
    /** Node `detached` (przywrócony z kosza, bez adresu/chipa) — nie działa, dopóki
     * użytkownik nie zrobi re-JOIN. UI pokazuje go na szaro z adnotacją "wymaga JOIN". */
    val needsPairing: Boolean = false,
    /** Stała tożsamość logiczna (node_id z bramki). Do operacji na detached (usuwanie),
     * które nie mają adresu RF. 0 = brak (dane przykładowe). */
    val nodeId: Long = 0,
)

data class DeviceType(val id: String, val name: String)

// --- Dashboard (agregat pod ekran główny) ---

data class RoomTile(
    val name: String,
    val tempLabel: String, // "22,4°"
    val subtitle: String,  // "Grzeje · 3 światła"
    val accentType: String, // typ do koloru ikony (np. "heating")
)

data class PvState(
    val powerKw: String,     // "2,84 kW"
    val capacityKwp: String, // "6,0 kWp"
)

data class DashboardData(
    val greetingName: String,
    val statusLine: String,
    val solar: SolarState,
    val solarDailyYield: String, // "23,7 kWh"
    val solarMiniBars: List<Double>,
    val climate: ClimateState,
    val pv: PvState,
    val rooms: List<RoomTile>,
)
