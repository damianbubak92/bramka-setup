package com.aitronic.smarthome.data.net

import com.aitronic.smarthome.domain.model.*
import kotlinx.serialization.json.*

/**
 * Kodek reguł: model domenowy <-> JSON apki, który rozumie bramka
 * (Gateway/.../rulesjson.go -> shared/automation.h).
 *
 * Ordinale (automation.h, zweryfikowane):
 *   DEV_SOLAR=0, DEV_BUFFER=1, DEV_SMARTPHONE=2
 *   PARAM_T1..T4=0..3, PARAM_SBUF_TEMP=4
 *   COND_TIME=0, COND_PARAMETER=1, COND_PARAMETER_DELTA=2
 *   OP_LESS_THAN=0, OP_MORE_THAN=1, OP_BETWEEN=2
 *   ACTION_SET_RELAY=0
 *
 * 🔑 KLUCZOWE (engine.c): OP_MORE_THAN porównuje do **thresholdMin** (`mn`),
 * a OP_LESS_THAN do **thresholdMax** (`mx`). Wpisanie wartości w złe pole =
 * reguła nigdy nie odpali.
 */
object RulesCodec {

    private val json = Json { ignoreUnknownKeys = true; isLenient = true }

    // klucz UI -> ordinal
    private val devOrd = mapOf("solar" to 0, "buffer" to 1, "smartphone" to 2)
    private val ordDev = devOrd.entries.associate { (k, v) -> v to k }
    private val paramOrd = mapOf("T1" to 0, "T2" to 1, "T3" to 2, "T4" to 3, "sBuforTemp" to 4)
    private val ordParam = paramOrd.entries.associate { (k, v) -> v to k }

    private fun opOrd(op: CompareOp) = if (op == CompareOp.Gt) 1 else 0
    private fun ordOp(o: Int) = if (o == 1) CompareOp.Gt else CompareOp.Lt

    private fun hhmm(s: String): Pair<Int, Int> {
        val p = s.split(":")
        return (p.getOrNull(0)?.toIntOrNull() ?: 0) to (p.getOrNull(1)?.toIntOrNull() ?: 0)
    }

    private fun two(n: Int) = n.toString().padStart(2, '0')

    /** Model -> JSON dla command=setrules. */
    fun encode(rules: List<Rule>): String {
        val arr = buildJsonArray {
            rules.filter { it.enabled }.forEach { r ->
                addJsonObject {
                    put("name", r.name)
                    put("condCnt", r.conditions.size)
                    putJsonArray("conds") {
                        r.conditions.forEach { c -> add(encodeCond(c)) }
                    }
                    putJsonObject("action") {
                        put("target", devOrd[r.action.target] ?: 0)
                        put("actionType", 0) // ACTION_SET_RELAY
                        put("value", r.action.value)
                    }
                }
            }
        }
        return arr.toString()
    }

    private fun encodeCond(c: Condition): JsonObject = when (c) {
        is Condition.Time -> {
            val (hS, mS) = hhmm(c.start); val (hE, mE) = hhmm(c.end)
            buildJsonObject { put("type", 0); put("hS", hS); put("mS", mS); put("hE", hE); put("mE", mE) }
        }
        is Condition.Param -> buildJsonObject {
            put("type", 1)
            put("d", devOrd[c.device] ?: 0)
            put("p", paramOrd[c.param] ?: 0)
            put("op", opOrd(c.op))
            // MORE -> mn, LESS -> mx (engine.c)
            put("mn", if (c.op == CompareOp.Gt) c.value else 0.0)
            put("mx", if (c.op == CompareOp.Lt) c.value else 0.0)
        }
        is Condition.Delta -> buildJsonObject {
            put("type", 2)
            put("d1", devOrd[c.device1] ?: 0); put("p1", paramOrd[c.param1] ?: 0)
            put("d2", devOrd[c.device2] ?: 0); put("p2", paramOrd[c.param2] ?: 0)
            put("op", opOrd(c.op))
            put("mn", if (c.op == CompareOp.Gt) c.min else 0.0)
            put("mx", if (c.op == CompareOp.Lt) c.min else 0.0)
        }
    }

    /** JSON z command=getrules -> model. Bramka zwraca tylko reguły aktywne. */
    fun decode(text: String): List<Rule> {
        val arr = runCatching { json.parseToJsonElement(text).jsonArray }.getOrNull() ?: return emptyList()
        return arr.mapIndexedNotNull { i, el ->
            val o = el as? JsonObject ?: return@mapIndexedNotNull null
            val conds = o["conds"]?.jsonArray.orEmpty().mapNotNull { decodeCond(it as? JsonObject ?: return@mapNotNull null) }
            val a = o["action"]?.jsonObject
            Rule(
                id = (i + 1).toLong(),
                name = o["name"]?.jsonPrimitive?.contentOrNull ?: "Reguła ${i + 1}",
                enabled = true,
                conditions = conds,
                action = RuleAction(
                    target = ordDev[a?.get("target")?.jsonPrimitive?.intOrNull ?: 0] ?: "solar",
                    value = a?.get("value")?.jsonPrimitive?.intOrNull ?: 0,
                ),
            )
        }
    }

    private fun decodeCond(o: JsonObject): Condition? {
        fun i(k: String) = o[k]?.jsonPrimitive?.intOrNull ?: 0
        fun d(k: String) = o[k]?.jsonPrimitive?.doubleOrNull ?: 0.0
        return when (i("type")) {
            0 -> Condition.Time("${two(i("hS"))}:${two(i("mS"))}", "${two(i("hE"))}:${two(i("mE"))}")
            1 -> {
                val op = ordOp(i("op"))
                Condition.Param(
                    device = ordDev[i("d")] ?: "solar",
                    param = ordParam[i("p")] ?: "T1",
                    op = op,
                    value = if (op == CompareOp.Gt) d("mn") else d("mx"),
                )
            }
            2 -> {
                val op = ordOp(i("op"))
                Condition.Delta(
                    device1 = ordDev[i("d1")] ?: "solar", param1 = ordParam[i("p1")] ?: "T1",
                    device2 = ordDev[i("d2")] ?: "buffer", param2 = ordParam[i("p2")] ?: "sBuforTemp",
                    op = op,
                    min = if (op == CompareOp.Gt) d("mn") else d("mx"),
                )
            }
            else -> null
        }
    }
}
