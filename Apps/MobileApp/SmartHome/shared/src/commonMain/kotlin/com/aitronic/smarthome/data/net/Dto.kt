package com.aitronic.smarthome.data.net

import kotlinx.serialization.Serializable

/**
 * DTO 1:1 z odpowiedziami bramki (zweryfikowane w Go: store.go NodeInfo, join.go pendingJoin).
 * Wszystkie pola z wartościami domyślnymi — bramka bywa permisywna, a apka ma nie wywalać się
 * na brakującym kluczu.
 */

/** command=listnodes → [{id,address,type,name,factory,status,lastSeen,provisionedAt,room}] */
@Serializable
data class NodeInfoDto(
    /** Stała tożsamość logiczna (AUTOINCREMENT) — kotwica historii/reguł, przeżywa
     * zmianę adresu/chipa. Klucz urządzenia w apce. */
    val id: Long = 0,
    /** Adres RF (reużywalny, 0 gdy detached/odłączony). Do komend (pompa) i replace. */
    val address: Int = 0,
    val type: Int = 0,
    val name: String = "",
    val factory: String = "",
    val status: String = "active", // pending_join | active | detached | legacy
    val lastSeen: Long = 0,
    val provisionedAt: Long = 0,
    /** Grupowanie w apce; "" = Bez pokoju. Etykieta — node o pokoju nic nie wie. */
    val room: String = "",
    /** Maska zdolności NODE_CAP(ACTION_*) zadeklarowana przez node przy JOIN — na jakie
     * akcje edytor automatyzacji może go wskazać jako cel (0 = tylko czujnik). */
    val capabilities: Long = 0,
)

/** command=listjoins → [{factory,type,firstSeen,lastSeen,count}] */
@Serializable
data class PendingJoinDto(
    val factory: String = "",
    val type: Int = 0,
    /** Zdolności zadeklarowane przy JOIN (bitmaska NODE_CAP(ACTION_*)). */
    val capabilities: Long = 0,
    val firstSeen: Long = 0,
    val lastSeen: Long = 0,
    val count: Int = 0,
)

/**
 * command=state → [{address,type,params:{k:v},ts}] — OSTATNIA ZNANA telemetria z bazy
 * (tabela node_param). Czytane raz przy otwarciu apki, żeby wszystkie pola i stany
 * pomp były realne od razu, bez czekania na kolejny push WS (co 2 min).
 */
@Serializable
data class NodeStateDto(
    val address: Int = 0,
    val type: Int = 0,
    val params: Map<String, Double> = emptyMap(),
    val ts: Long = 0,
    /** Moc chwilowa policzona przez bramkę (VIEW solar_state) — tylko nody solar. */
    val powerKw: Double? = null,
    /** Uzysk narastająco w dobie [kWh] (VIEW solar_state) — tylko nody solar. */
    val energyDayKwh: Double? = null,
)

/**
 * command=history&range=day|month|year|total → [SolarSeriesDto] (solarhistory.go).
 * Same liczby, bez etykiet/jednostek — formatowanie robi apka (locale telefonu).
 * samples/expected = pokrycie danymi: pozwala odróżnić "0 kWh bo noc" od
 * "0 kWh bo bramka nie zbierała" (nie rysujemy dziury jako słabego dnia).
 */
@Serializable
data class SolarBarDto(
    val bucket: Long = 0,        // unix s, początek okresu (godzina/dzień/miesiąc/rok)
    val energyKwh: Double = 0.0,
    val pumpMinutes: Long = 0,
    val samples: Long = 0,
    val expected: Long = 0,
)

@Serializable
data class SolarSeriesDto(
    val bucket: Long = 0,        // początek całego okresu
    val label: String = "",      // gotowa etykieta PL (bramka: "12 lip 2026")
    val bars: List<SolarBarDto> = emptyList(),
    val energyKwh: Double = 0.0,   // suma za CAŁY okres
    val pumpMinutes: Long = 0,
    val samples: Long = 0,
    val expected: Long = 0,
)

/** command=approvejoin → {address,factory,name,type} */
@Serializable
data class ApproveResultDto(
    val address: Int = 0,
    val factory: String = "",
    val name: String = "",
    val type: Int = 0,
)

/** command=replacenode&factory=<hex>&target=<addr> → {factory,type,address,replaced} */
@Serializable
data class ReplaceResultDto(
    val factory: String = "",
    val type: Int = 0,
    val address: Int = 0,
    val replaced: Boolean = false,
)

/** command=listtrash → [{id,type,name,room,lastSeen,archivedAt}] — soft-usunięte nody
 * (kosz), okno retencji 60 dni. */
@Serializable
data class TrashNodeDto(
    val id: Long = 0,
    val type: Int = 0,
    val name: String = "",
    val room: String = "",
    val factory: String = "",   // chip który był na nim przy usunięciu (do dopasowania JOIN po factory_id)
    val lastSeen: Long = 0,
    val archivedAt: Long = 0,   // unix s — kiedy trafił do kosza
)

/** command=restorenode&id=<id> → {id,status} (status = "detached"). */
@Serializable
data class RestoreResultDto(
    val id: Long = 0,
    val status: String = "",
)

/** Zdarzenia z kanału WS (wshub.go: join_pending / telemetry / node_status). */
sealed interface GatewayEvent {
    data class JoinPending(val factory: String, val nodeType: Int) : GatewayEvent
    data class Telemetry(val address: Int, val nodeType: Int, val params: Map<String, Double>, val ts: Long) : GatewayEvent
    data class NodeStatus(val address: Int, val status: String) : GatewayEvent
}

/** Typy nodów (Shared/Protocol/node_protocol.h). */
object NodeTypes {
    const val SOLAR = 0
    const val BUFOR = 1
    const val CURTAINS = 2
    const val LIGHT = 3
    const val VENTILATION = 4
    const val SMARTPHONE = 5
    const val TH_SENSOR = 6

    /** Typ noda → klucz typu urządzenia w UI (ui/theme DeviceColors, ikony). */
    fun toUiType(nodeType: Int): String = when (nodeType) {
        SOLAR -> "solar"
        BUFOR -> "buffer"
        CURTAINS -> "blind"
        LIGHT -> "light"
        VENTILATION -> "hub"
        TH_SENSOR -> "climate"
        else -> "hub"
    }

    /** Etykieta typu dla list/edytora urządzeń. */
    fun label(nodeType: Int): String = when (nodeType) {
        SOLAR -> "Sterownik solarny"
        BUFOR -> "Sterownik bufora"
        CURTAINS -> "Roleta"
        LIGHT -> "Oświetlenie"
        VENTILATION -> "Wentylacja"
        TH_SENSOR -> "Czujnik klimatu"
        else -> "Urządzenie"
    }
}

/** Nazwy parametrów telemetrii (telemetry.go). */
object Params {
    // solar (NODE_SOLAR_CONTROLLER)
    const val TCOL = "Tcol"
    const val T1 = "T1"; const val T2 = "T2"; const val T3 = "T3"; const val T4 = "T4"
    const val ENERGY_GAIN = "energyGain"
    const val FLOW_RATE = "flowRate"
    const val PUMP_STATE = "pumpState"
    // bufor
    const val SBUF_TEMP = "sBuforTemp"
    // TH sensor
    const val TEMPERATURE = "temperature"
    const val HUMIDITY = "humidity"
    const val BATT_MV = "batt_mv"
}
