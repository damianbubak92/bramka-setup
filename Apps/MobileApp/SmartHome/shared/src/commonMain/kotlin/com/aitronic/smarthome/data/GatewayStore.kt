package com.aitronic.smarthome.data

import com.aitronic.smarthome.data.net.*
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
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
    /** Kosz — soft-usunięte nody. Ładowany na żądanie (loadTrash), nie live. */
    val trash: List<TrashNodeDto> = emptyList(),
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

    /** Puls „pojawił się NOWY oczekujący JOIN" (z WS). Jednorazowe zdarzenie — UI reaguje
     * raz (przeskok na Urządzenia + auto-popup), nie na każdą rekompozycję. */
    private val _newJoin = MutableSharedFlow<Unit>(extraBufferCapacity = 8)
    val newJoin: SharedFlow<Unit> = _newJoin.asSharedFlow()

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
            is GatewayEvent.JoinPending -> {
                // onEvent leci sekwencyjnie z jednego collecta, więc value-read jest bezpieczny.
                val isNew = _state.value.joins.none { it.factory == ev.factory }
                if (isNew) {
                    _state.update { s -> s.copy(joins = s.joins + PendingJoinDto(factory = ev.factory, type = ev.nodeType)) }
                    _newJoin.tryEmit(Unit) // przenieś użytkownika do Urządzeń + otwórz popup
                }
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

    /** Usuń noda po node_id — dla detached (bez adresu). Trafia do kosza. */
    suspend fun removeNodeById(nodeId: Long): Result<Unit> = runCatching {
        client.removeNodeById(nodeId)
        refresh()
    }

    suspend fun updateNode(address: Int, name: String, room: String): Result<Unit> = runCatching {
        if (!client.updateNode(address, name, room)) error("Bramka odrzuciła zmianę")
        refresh()
    }

    /** Wymień chip pod istniejącym AKTYWNYM nodem (nowy chip przejmuje adres target,
     * historia zostaje). factoryHex = chip który właśnie dołączył (pending). newName
     * pusty = zachowaj starą nazwę; podany = przemianuj (pokój zachowany). */
    suspend fun replaceNode(factoryHex: String, targetAddress: Int, newName: String = ""): Result<ReplaceResultDto> = runCatching {
        val r = client.replaceNode(factoryHex, targetAddress)
        if (newName.isNotBlank()) {
            val room = _state.value.nodes.firstOrNull { it.address == targetAddress }?.room ?: ""
            client.updateNode(targetAddress, newName, room)
        }
        _state.update { s -> s.copy(joins = s.joins.filterNot { it.factory == factoryHex }) }
        refresh()
        r
    }

    /** Re-paruj świeży chip na node DETACHED (z kosza): bramka alokuje nowy adres i
     * przypina go do node_id → historia wraca. newName jak w [replaceNode]. */
    suspend fun repairNode(factoryHex: String, nodeId: Long, newName: String = ""): Result<ReplaceResultDto> = runCatching {
        val r = client.repairNode(factoryHex, nodeId)
        if (newName.isNotBlank() && r.address > 0) {
            val room = _state.value.nodes.firstOrNull { it.id == nodeId }?.room ?: ""
            client.updateNode(r.address, newName, room)
        }
        _state.update { s -> s.copy(joins = s.joins.filterNot { it.factory == factoryHex }) }
        refresh()
        r
    }

    /** „Odrzuć" oczekujący JOIN (przypadkowy przycisk): usuń z rejestru bramki i z kolejki. */
    suspend fun rejectJoin(factoryHex: String): Result<Unit> = runCatching {
        client.rejectJoin(factoryHex)
        _state.update { s -> s.copy(joins = s.joins.filterNot { it.factory == factoryHex }) }
    }

    /** „Przywróć/Wymień" celujące w node w KOSZU: wyjmij z kosza → detached, potem re-paruj
     * chip (repair). Historia (po node_id) wraca, nazwa zachowana. */
    suspend fun restoreAndRepair(factoryHex: String, nodeId: Long): Result<ReplaceResultDto> = runCatching {
        client.restoreNode(nodeId)
        val r = client.repairNode(factoryHex, nodeId)
        _state.update { s -> s.copy(
            joins = s.joins.filterNot { it.factory == factoryHex },
            trash = s.trash.filterNot { it.id == nodeId },
        ) }
        refresh()
        r
    }

    /** Załaduj kosz (soft-usunięte nody) do stanu — na otwarcie ekranu Kosz / popupu JOIN. */
    suspend fun loadTrash(): Result<Unit> = runCatching {
        val t = client.listTrash()
        _state.update { s -> s.copy(trash = t) }
    }

    /** Przywróć noda z kosza → wraca jako `detached`. Usuwamy z kosza OPTYMISTYCZNIE
     * (mirror od-archiwizowuje z opóźnieniem workera ~15s, więc od-pytanie by go jeszcze
     * pokazało), a listę nodów odświeżamy. */
    suspend fun restoreNode(id: Long): Result<Unit> = runCatching {
        client.restoreNode(id)
        _state.update { s -> s.copy(trash = s.trash.filterNot { it.id == id }) }
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
