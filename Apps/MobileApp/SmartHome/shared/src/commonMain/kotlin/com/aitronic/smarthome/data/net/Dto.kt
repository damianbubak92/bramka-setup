package com.aitronic.smarthome.data.net

import kotlinx.serialization.Serializable

/**
 * DTO 1:1 z odpowiedziami bramki (zweryfikowane w Go: store.go NodeInfo, join.go pendingJoin).
 * Wszystkie pola z wartościami domyślnymi — bramka bywa permisywna, a apka ma nie wywalać się
 * na brakującym kluczu.
 */

/** command=listnodes → [{address,type,name,factory,status,lastSeen,provisionedAt}] */
@Serializable
data class NodeInfoDto(
    val address: Int = 0,
    val type: Int = 0,
    val name: String = "",
    val factory: String = "",
    val status: String = "active", // pending_join | active | pending_remove
    val lastSeen: Long = 0,
    val provisionedAt: Long = 0,
)

/** command=listjoins → [{factory,type,firstSeen,lastSeen,count}] */
@Serializable
data class PendingJoinDto(
    val factory: String = "",
    val type: Int = 0,
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
)

/** command=approvejoin → {address,factory,name,type} */
@Serializable
data class ApproveResultDto(
    val address: Int = 0,
    val factory: String = "",
    val name: String = "",
    val type: Int = 0,
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
