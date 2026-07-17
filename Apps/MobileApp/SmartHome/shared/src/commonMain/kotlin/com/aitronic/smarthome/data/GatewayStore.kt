package com.aitronic.smarthome.data

import com.aitronic.smarthome.data.net.*
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/** Telemetria jednego noda (ostatni odczyt). */
data class NodeTelemetry(val nodeType: Int, val params: Map<String, Double>, val ts: Long)

/**
 * Stan całej apki pochodzący z bramki. Jedno źródło prawdy dla UI.
 * Uzupełniany: (a) odpytaniem HTTP przy starcie/odświeżeniu, (b) na żywo z WS.
 */
data class GatewayState(
    val source: GatewaySource = GatewaySource.Offline,
    val nodes: List<NodeInfoDto> = emptyList(),
    val joins: List<PendingJoinDto> = emptyList(),
    /** address -> ostatnia telemetria */
    val telemetry: Map<Int, NodeTelemetry> = emptyMap(),
    /**
     * address -> moc policzona przez bramkę (VIEW solar_state) przy ostatnim `state`.
     * Fallback zanim przyjdzie 1. telemetria — moc realna od razu po otwarciu apki.
     * Live moc liczy się już wprost z przyrostu w telemetrii (patrz solarPowerKw).
     */
    val powerKwHint: Map<Int, Double> = emptyMap(),
    /** address -> uzysk narastająco w dobie [kWh] (VIEW solar_state). Bramka liczy. */
    val energyDayKwh: Map<Int, Double> = emptyMap(),
    val loading: Boolean = false,
    val error: String? = null,
) {
    val online: Boolean get() = source != GatewaySource.Offline

    /**
     * Node danego typu — **najświeższy** (max ts), nie „pierwszy z brzegu".
     * W `node_param` siedzą też stare/testowe nody (state zwraca ich więcej niż listnodes);
     * wybór po kolejności mapy potrafił trafić w stalego noda tego samego typu, przez co UI
     * pokazywał jego (puste) parametry jako zera i ignorował żywą telemetrię z WS.
     */
    fun firstOfType(nodeType: Int): NodeTelemetry? =
        telemetry.values.filter { it.nodeType == nodeType }.maxByOrNull { it.ts }

    fun param(nodeType: Int, name: String): Double? = firstOfType(nodeType)?.params?.get(name)
}

/**
 * Warstwa łącząca UI z bramką: trzyma [state], subskrybuje kanał WS i wystawia akcje.
 * Zapisy zawsze idą do bramki (źródło prawdy + aktuator) — zgodnie z ustaloną kaskadą.
 */
class GatewayStore(
    private val client: GatewayClient,
    private val scope: CoroutineScope,
) {
    private val _state = MutableStateFlow(GatewayState())
    val state: StateFlow<GatewayState> = _state.asStateFlow()

    /** Start: pierwsze pobranie + nasłuch live. Wołać raz (np. z AppScaffold). */
    fun start() {
        scope.launch { refresh() }
        scope.launch {
            client.events().collect { ev -> onEvent(ev) }
        }
    }

    /**
     * Odświeżenie stanu z bramki: lista nodów + oczekujące JOIN-y + **ostatnia znana
     * telemetria z bazy** (`command=state`). Dzięki temu zaraz po otwarciu apki wszystkie
     * pola i stany pomp są realne — nie czekamy na push WS (co 2 min).
     */
    suspend fun refresh() {
        _state.update { it.copy(loading = true, error = null) }
        try {
            val nodes = client.listNodes()
            val joins = runCatching { client.listJoins() }.getOrDefault(emptyList())
            val snapshot = runCatching { client.state() }.getOrDefault(emptyList())
            _state.update { s ->
                // merge po ts: świeższy odczyt z WS ma pierwszeństwo nad snapshotem z bazy
                val merged = s.telemetry.toMutableMap()
                for (n in snapshot) {
                    val cur = merged[n.address]
                    if (cur == null || n.ts >= cur.ts) {
                        // scalamy z tym, co już mamy (snapshot per-param z node_param jest pełny,
                        // ale nie nadpisujmy świeższych kluczy przypadkiem)
                        merged[n.address] = NodeTelemetry(n.type, (cur?.params ?: emptyMap()) + n.params, n.ts)
                    }
                }
                val hints = s.powerKwHint + snapshot.mapNotNull { n -> n.powerKw?.let { n.address to it } }
                val dayKwh = s.energyDayKwh + snapshot.mapNotNull { n -> n.energyDayKwh?.let { n.address to it } }
                s.copy(
                    nodes = nodes, joins = joins, telemetry = merged,
                    powerKwHint = hints, energyDayKwh = dayKwh,
                    loading = false, source = client.source, error = null,
                )
            }
        } catch (t: Throwable) {
            _state.update {
                it.copy(loading = false, source = GatewaySource.Offline, error = t.message ?: "Brak połączenia z bramką")
            }
        }
    }

    private fun onEvent(ev: GatewayEvent) {
        when (ev) {
            is GatewayEvent.Telemetry -> _state.update { s ->
                val old = s.telemetry[ev.address]
                // 🔑 SCALAMY parametry, nie podmieniamy. Node wysyła też telemetrię
                // "pump-only" (SEND_PUMP_STATUS = sam pumpState) — podmiana całej mapy
                // kasowała temperatury i uzysk aż do następnej pełnej telemetrii (2 min).
                val mergedParams = (old?.params ?: emptyMap()) + ev.params
                s.copy(
                    telemetry = s.telemetry + (ev.address to NodeTelemetry(ev.nodeType, mergedParams, ev.ts)),
                    source = client.source,
                )
            }
            is GatewayEvent.NodeStatus -> {
                // status zmieniony po stronie bramki -> odśwież listę nodów
                scope.launch { refresh() }
            }
            is GatewayEvent.JoinPending -> _state.update { s ->
                if (s.joins.any { it.factory == ev.factory }) s
                else s.copy(joins = s.joins + PendingJoinDto(factory = ev.factory, type = ev.nodeType))
            }
        }
    }

    // --- Akcje (zawsze do bramki) ---

    suspend fun pump(on: Boolean): Result<Unit> = runCatching {
        if (!client.pump(on)) error("Bramka odrzuciła komendę pompy")
    }

    suspend fun approveJoin(factoryHex: String, name: String): Result<ApproveResultDto> = runCatching {
        val r = client.approveJoin(factoryHex, name)
        _state.update { s -> s.copy(joins = s.joins.filterNot { it.factory == factoryHex }) }
        refresh()
        r
    }

    suspend fun removeNode(address: Int): Result<Unit> = runCatching {
        client.removeNode(address)
        refresh()
    }

    suspend fun updateNode(address: Int, name: String, room: String): Result<Unit> = runCatching {
        if (!client.updateNode(address, name, room)) error("Bramka odrzuciła zmianę")
        refresh()
    }

    /** Wykresy uzysku solarnego (day|month|year|total). Osobne żądanie, nie część live-state. */
    suspend fun solarHistory(range: String, count: Int = 0): Result<List<SolarSeriesDto>> =
        runCatching { client.solarHistory(range, count) }

    suspend fun rulesJson(): Result<String> = runCatching { client.getRulesJson() }

    suspend fun saveRulesJson(json: String): Result<Unit> = runCatching {
        if (!client.setRulesJson(json)) error("Bramka odrzuciła reguły")
    }
}
