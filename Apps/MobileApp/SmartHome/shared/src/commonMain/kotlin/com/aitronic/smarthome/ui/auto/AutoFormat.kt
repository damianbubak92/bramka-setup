package com.aitronic.smarthome.ui.auto

import com.aitronic.smarthome.data.net.NodeInfoDto
import com.aitronic.smarthome.data.net.NodeTypes
import com.aitronic.smarthome.domain.model.ActionTypes
import com.aitronic.smarthome.domain.model.CompareOp
import com.aitronic.smarthome.domain.model.Condition
import com.aitronic.smarthome.domain.model.RuleAction

/**
 * Model automatyzacji jest teraz **per-węzeł**: warunki i akcje wskazują konkretny
 * `node_id` z listy bramki (nie typ urządzenia). Katalog dostępnych parametrów zależy
 * od TYPU węzła, a dostępnych akcji — od zadeklarowanych przez węzeł **capabilities**
 * (przy JOIN). Bramka rozwiązuje `node_id` → adres RF dopiero przy pushu do silnika.
 */

/** Węzeł widziany przez edytor automatyzacji (spłaszczony [NodeInfoDto]). */
data class AutoNode(
    val id: Long,
    val name: String,
    val nodeType: Int,
    val uiType: String,
    val capabilities: Long,
) {
    /** Można na nim wykonać JAKĄKOLWIEK akcję (ma niezerowe capabilities). */
    val actionable: Boolean get() = actionsForCaps(capabilities).isNotEmpty()

    /** Ma parametry, które da się użyć w warunku. */
    val hasCondParams: Boolean get() = condParamsForType(nodeType).isNotEmpty()
}

fun NodeInfoDto.toAutoNode(): AutoNode =
    AutoNode(id, name.ifBlank { NodeTypes.label(type) }, type, NodeTypes.toUiType(type), capabilities)

/** Widok listy węzłów jako mapa id→węzeł (do rozwiązywania nazw/kolorów w O(1)). */
typealias NodeLookup = Map<Long, AutoNode>

// --- Katalogi zależne od węzła ---

/** Parametry węzła dostępne jako źródło WARUNKU (klucz telemetrii → etykieta). */
fun condParamsForType(nodeType: Int): List<Pair<String, String>> = when (nodeType) {
    NodeTypes.SOLAR -> listOf(
        "T1" to "T1", "T2" to "T2", "T3" to "T3", "T4" to "T4",
        "Tcol" to "Kolektor (Tcol)", "flowRate" to "Przepływ pompy",
    )
    NodeTypes.BUFOR -> listOf("sBuforTemp" to "Temp. bufora")
    NodeTypes.TH_SENSOR -> listOf("temperature" to "Temperatura", "humidity" to "Wilgotność")
    else -> emptyList()
}

/** Akcje dostępne dla węzła wg jego capabilities (actionType → etykieta). */
fun actionsForCaps(capabilities: Long): List<Pair<Int, String>> = buildList {
    if (capabilities and (1L shl ActionTypes.SET_RELAY) != 0L) add(ActionTypes.SET_RELAY to "Ustaw przekaźnik")
    // przyszłe: SET_PUMP_SPEED itd. — dojdą tu, gdy protokół je doda.
}

// --- Rozwiązywanie nazw / kolorów po node_id ---

/** Nazwa węzła, albo „niedostępny" gdy reguła wskazuje węzeł spoza aktualnej listy
 * (usunięty / detached). node==0 = akcja systemowa (powiadomienie). */
fun NodeLookup.nodeLabel(id: Long): String = when {
    id == 0L -> "Powiadomienie"
    else -> this[id]?.name ?: "Niedostępny węzeł"
}

fun NodeLookup.uiTypeOf(id: Long): String? = this[id]?.uiType

fun NodeLookup.isAvailable(id: Long): Boolean = id == 0L || containsKey(id)

fun opSym(op: CompareOp): String = if (op == CompareOp.Gt) ">" else "<"

/** Format liczby PL (przecinek, bez zbędnego ,0). */
fun fmtNum(v: Double): String {
    if (v == v.toLong().toDouble()) return v.toLong().toString()
    return v.toString().replace('.', ',')
}

fun fmtNumStr(s: String): String = s.trim().replace('.', ',').ifEmpty { "?" }

/** Streszczenie warunku na pigułkę. */
fun NodeLookup.condSummary(c: Condition): String = when (c) {
    is Condition.Time -> "Czas ${c.start}–${c.end}"
    is Condition.Param -> "${nodeLabel(c.node)} · ${c.param} ${opSym(c.op)} ${fmtNum(c.value)}"
    is Condition.Delta -> "${nodeLabel(c.node1)}·${c.param1} − ${nodeLabel(c.node2)}·${c.param2} ${opSym(c.op)} ${fmtNum(c.min)}"
}

/** Klucz UI-typu do koloru pigułki (time = neutralny; niedostępny węzeł = null). */
fun NodeLookup.condUiType(c: Condition): String? = when (c) {
    is Condition.Time -> null
    is Condition.Param -> uiTypeOf(c.node)
    is Condition.Delta -> uiTypeOf(c.node1)
}

/** Czy KTÓRYKOLWIEK węzeł reguły jest niedostępny (usunięty/detached). */
fun NodeLookup.ruleHasUnavailable(conditions: List<Condition>, action: RuleAction): Boolean {
    val condBad = conditions.any {
        when (it) {
            is Condition.Time -> false
            is Condition.Param -> !isAvailable(it.node)
            is Condition.Delta -> !isAvailable(it.node1) || !isAvailable(it.node2)
        }
    }
    val actBad = action.node != 0L && !isAvailable(action.node)
    return condBad || actBad
}

fun NodeLookup.actionText(action: RuleAction): String = when (action.actionType) {
    ActionTypes.SEND_MESSAGE -> "Powiadomienie: ${action.message.ifBlank { "—" }}"
    else -> "${nodeLabel(action.node)} · Przekaźnik ${if (action.value >= 0.5) "ON" else "OFF"}"
}

// --- Model roboczy edytora (liczby jako String, by walidować puste/niepoprawne) ---

enum class CondType { Time, Param, Delta }

data class CondDraft(
    val type: CondType = CondType.Time,
    val start: String = "06:00",
    val end: String = "22:00",
    val node: Long = 0, val param: String = "",
    val op: CompareOp = CompareOp.Gt,
    val value: String = "60",
    val node1: Long = 0, val param1: String = "",
    val node2: Long = 0, val param2: String = "",
    val min: String = "8",
)

fun Condition.toDraft(): CondDraft = when (this) {
    is Condition.Time -> CondDraft(CondType.Time, start = start, end = end)
    is Condition.Param -> CondDraft(CondType.Param, node = node, param = param, op = op, value = fmtNum(value))
    is Condition.Delta -> CondDraft(CondType.Delta, node1 = node1, param1 = param1, node2 = node2, param2 = param2, op = op, min = fmtNum(min))
}

fun CondDraft.toCondition(): Condition = when (type) {
    CondType.Time -> Condition.Time(start, end)
    CondType.Param -> Condition.Param(node, param, op, value.replace(',', '.').toDoubleOrNull() ?: 0.0)
    CondType.Delta -> Condition.Delta(node1, param1, node2, param2, op, min.replace(',', '.').toDoubleOrNull() ?: 0.0)
}

/** Streszczenie draftu (do podglądu). */
fun NodeLookup.draftSummary(d: CondDraft): String = when (d.type) {
    CondType.Time -> "Czas ${d.start}–${d.end}"
    CondType.Param -> "${nodeLabel(d.node)} · ${d.param} ${opSym(d.op)} ${fmtNumStr(d.value)}"
    CondType.Delta -> "${nodeLabel(d.node1)}·${d.param1} − ${nodeLabel(d.node2)}·${d.param2} ${opSym(d.op)} ${fmtNumStr(d.min)}"
}

fun NodeLookup.draftUiType(d: CondDraft): String? = when (d.type) {
    CondType.Time -> null
    CondType.Param -> uiTypeOf(d.node)
    CondType.Delta -> uiTypeOf(d.node1)
}

/** Walidacja warunku. Zwraca komunikat lub null. */
fun CondDraft.error(): String? = when (type) {
    CondType.Time -> if (start == end) "godziny „od\" i „do\" są takie same" else null
    CondType.Param -> when {
        node == 0L -> "wybierz urządzenie"
        param.isBlank() -> "wybierz parametr"
        value.replace(',', '.').toDoubleOrNull() == null -> "brak poprawnej wartości progowej"
        else -> null
    }
    CondType.Delta -> {
        val m = min.replace(',', '.').toDoubleOrNull()
        when {
            node1 == 0L || node2 == 0L -> "wybierz oba urządzenia"
            param1.isBlank() || param2.isBlank() -> "wybierz oba parametry"
            m == null -> "brak poprawnej wartości różnicy"
            m < 0 -> "różnica nie może być ujemna"
            node1 == node2 && param1 == param2 -> "oba parametry są identyczne"
            else -> null
        }
    }
}
