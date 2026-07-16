package com.aitronic.smarthome.ui.auto

import com.aitronic.smarthome.domain.model.CompareOp
import com.aitronic.smarthome.domain.model.Condition
import com.aitronic.smarthome.domain.model.RuleAction

/**
 * Katalog reguł **przycięty do tego, co silnik M4F realnie umie zakodować**
 * (Shared/Protocol/automation.h: DEV_SOLAR/BUFFER/SMARTPHONE, PARAM_T1..T4/SBUF_TEMP).
 *
 * Design apki był robiony na przyszłość (PV, czujnik klimatu, Tcol, moc/uzysk…) —
 * te pozycje wrócą, gdy rozszerzymy protokół (np. po podłączeniu noda T&H).
 * Do tego czasu nie wystawiamy ich w edytorze, żeby nie dało się zapisać reguły,
 * której bramka nie potrafi wykonać.
 */
val AutoDevices: List<Pair<String, String>> = listOf(
    "solar" to "Sterownik solarny",
    "buffer" to "Sterownik bufora",
)

val AutoParams: Map<String, List<String>> = mapOf(
    "solar" to listOf("T1", "T2", "T3", "T4"),
    "buffer" to listOf("sBuforTemp"),
)

/** Cele akcji: dziś tylko sterownik solarny (przekaźnik = pompa dodatkowa). */
val AutoTargets: List<Pair<String, String>> = listOf(
    "solar" to "Sterownik solarny",
)

fun deviceName(id: String): String = AutoDevices.firstOrNull { it.first == id }?.second ?: "—"

fun opSym(op: CompareOp): String = if (op == CompareOp.Gt) ">" else "<"

/** Format liczby PL (przecinek, bez zbędnego ,0). */
fun fmtNum(v: Double): String {
    if (v == v.toLong().toDouble()) return v.toLong().toString()
    return v.toString().replace('.', ',')
}

fun fmtNumStr(s: String): String = s.trim().replace('.', ',').ifEmpty { "?" }

/** Streszczenie warunku na pigułkę (condSummary handoffu). */
fun condSummary(c: Condition): String = when (c) {
    is Condition.Time -> "Czas ${c.start}–${c.end}"
    is Condition.Param -> "${deviceName(c.device)} · ${c.param} ${opSym(c.op)} ${fmtNum(c.value)}"
    is Condition.Delta -> "${c.param1} − ${c.param2} ${opSym(c.op)} ${fmtNum(c.min)}"
}

/** Kolor akcentu pigułki = typ urządzenia źródłowego (time = neutralny). */
fun condDeviceKey(c: Condition): String? = when (c) {
    is Condition.Time -> null
    is Condition.Param -> c.device
    is Condition.Delta -> c.device1
}

fun actionText(action: RuleAction): String =
    "${deviceName(action.target)} · Przekaźnik ${if (action.value == 1) "ON" else "OFF"}"

// --- Model roboczy edytora (liczby jako String, by walidować puste/niepoprawne) ---

enum class CondType { Time, Param, Delta }

data class CondDraft(
    val type: CondType = CondType.Time,
    val start: String = "06:00",
    val end: String = "22:00",
    val device: String = "solar",
    val param: String = "T3",
    val op: CompareOp = CompareOp.Gt,
    val value: String = "60",
    val device1: String = "solar",
    val param1: String = "T3",
    val device2: String = "buffer",
    val param2: String = "sBuforTemp",
    val min: String = "8",
)

fun Condition.toDraft(): CondDraft = when (this) {
    is Condition.Time -> CondDraft(CondType.Time, start = start, end = end)
    is Condition.Param -> CondDraft(CondType.Param, device = device, param = param, op = op, value = fmtNum(value))
    is Condition.Delta -> CondDraft(CondType.Delta, device1 = device1, param1 = param1, device2 = device2, param2 = param2, op = op, min = fmtNum(min))
}

fun CondDraft.toCondition(): Condition = when (type) {
    CondType.Time -> Condition.Time(start, end)
    CondType.Param -> Condition.Param(device, param, op, value.replace(',', '.').toDoubleOrNull() ?: 0.0)
    CondType.Delta -> Condition.Delta(device1, param1, device2, param2, op, min.replace(',', '.').toDoubleOrNull() ?: 0.0)
}

/** Streszczenie draftu (do podglądu). */
fun CondDraft.summary(): String = when (type) {
    CondType.Time -> "Czas $start–$end"
    CondType.Param -> "${deviceName(device)} · $param ${opSym(op)} ${fmtNumStr(value)}"
    CondType.Delta -> "$param1 − $param2 ${opSym(op)} ${fmtNumStr(min)}"
}

fun CondDraft.deviceKey(): String? = when (type) {
    CondType.Time -> null
    CondType.Param -> device
    CondType.Delta -> device1
}

/** Walidacja warunku (condError handoffu). Zwraca komunikat lub null. */
fun CondDraft.error(): String? = when (type) {
    CondType.Time -> if (start == end) "godziny „od\" i „do\" są takie same" else null
    CondType.Param -> if (value.replace(',', '.').toDoubleOrNull() == null) "brak poprawnej wartości progowej" else null
    CondType.Delta -> {
        val m = min.replace(',', '.').toDoubleOrNull()
        when {
            m == null -> "brak poprawnej wartości różnicy"
            m < 0 -> "różnica nie może być ujemna"
            device1 == device2 && param1 == param2 -> "oba parametry są identyczne"
            else -> null
        }
    }
}
