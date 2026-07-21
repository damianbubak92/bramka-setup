package com.aitronic.smarthome.data.net

import com.aitronic.smarthome.domain.model.*
import kotlinx.serialization.json.*

/**
 * Kodek reguł: model domenowy <-> JSON apki, który rozumie bramka
 * (Gateway/.../rulesjson.go -> Shared/Protocol/automation.h).
 *
 * Model jest **per-węzeł**: warunki/akcje wskazują `node_id` (stała tożsamość, przeżywa
 * zmianę adresu/chipa). Bramka przechowuje ten surowy JSON verbatim (getrules go zwraca)
 * i dopiero przy pushu do silnika rozwiązuje `node_id` → adres RF oraz odfiltrowuje reguły
 * wyłączone. Dlatego wysyłamy **wszystkie** reguły z flagą `enabled` (nie tylko włączone).
 *
 * Ordinale (automation.h):
 *   PARAM_T1..T4=0..3, SBUF_TEMP=4, TCOL=5, ENERGY_GAIN=6, FLOW_RATE=7, PUMP_STATE=8,
 *   TEMPERATURE=9, HUMIDITY=10, BATT_MV=11
 *   COND_TIME=0, COND_PARAMETER=1, COND_PARAMETER_DELTA=2
 *   OP_LESS_THAN=0, OP_MORE_THAN=1
 *   ACTION_SET_RELAY=0, ACTION_SEND_MESSAGE=1
 *
 * 🔑 KLUCZOWE (engine.c): OP_MORE_THAN porównuje do **thresholdMin** (`mn`),
 * a OP_LESS_THAN do **thresholdMax** (`mx`). Wartość w złym polu = reguła nigdy nie odpali.
 */
object RulesCodec {

    private val json = Json { ignoreUnknownKeys = true; isLenient = true }

    private val paramOrd = mapOf(
        "T1" to 0, "T2" to 1, "T3" to 2, "T4" to 3, "sBuforTemp" to 4, "Tcol" to 5,
        "energyGain" to 6, "flowRate" to 7, "pumpState" to 8,
        "temperature" to 9, "humidity" to 10, "batt_mv" to 11,
    )
    private val ordParam = paramOrd.entries.associate { (k, v) -> v to k }

    private fun opOrd(op: CompareOp) = if (op == CompareOp.Gt) 1 else 0
    private fun ordOp(o: Int) = if (o == 1) CompareOp.Gt else CompareOp.Lt

    private fun hhmm(s: String): Pair<Int, Int> {
        val p = s.split(":")
        return (p.getOrNull(0)?.toIntOrNull() ?: 0) to (p.getOrNull(1)?.toIntOrNull() ?: 0)
    }

    private fun two(n: Int) = n.toString().padStart(2, '0')

    /** Model -> JSON dla command=setrules (wszystkie reguły, z flagą enabled). */
    fun encode(rules: List<Rule>): String {
        val arr = buildJsonArray {
            rules.forEach { r ->
                addJsonObject {
                    put("name", r.name)
                    put("enabled", r.enabled)
                    putJsonArray("conds") { r.conditions.forEach { c -> add(encodeCond(c)) } }
                    putJsonObject("action") {
                        put("node", r.action.node)
                        put("actionType", r.action.actionType)
                        put("value", r.action.value)
                        put("msg", r.action.message)
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
            put("node", c.node)
            put("p", paramOrd[c.param] ?: 0)
            put("op", opOrd(c.op))
            // MORE -> mn, LESS -> mx (engine.c)
            put("mn", if (c.op == CompareOp.Gt) c.value else 0.0)
            put("mx", if (c.op == CompareOp.Lt) c.value else 0.0)
        }
        is Condition.Delta -> buildJsonObject {
            put("type", 2)
            put("node1", c.node1); put("p1", paramOrd[c.param1] ?: 0)
            put("node2", c.node2); put("p2", paramOrd[c.param2] ?: 0)
            put("op", opOrd(c.op))
            put("mn", if (c.op == CompareOp.Gt) c.min else 0.0)
            put("mx", if (c.op == CompareOp.Lt) c.min else 0.0)
        }
    }

    /** JSON z command=getrules -> model (surowy, node_id-owy; zawiera też wyłączone). */
    fun decode(text: String): List<Rule> {
        val arr = runCatching { json.parseToJsonElement(text).jsonArray }.getOrNull() ?: return emptyList()
        return arr.mapIndexedNotNull { i, el ->
            val o = el as? JsonObject ?: return@mapIndexedNotNull null
            val conds = o["conds"]?.jsonArray.orEmpty().mapNotNull { decodeCond(it as? JsonObject ?: return@mapNotNull null) }
            val a = o["action"]?.jsonObject
            val actionType = a?.get("actionType")?.jsonPrimitive?.intOrNull ?: ActionTypes.SET_RELAY
            Rule(
                id = (i + 1).toLong(),
                name = o["name"]?.jsonPrimitive?.contentOrNull ?: "Reguła ${i + 1}",
                enabled = o["enabled"]?.jsonPrimitive?.booleanOrNull ?: true,
                conditions = conds,
                action = RuleAction(
                    node = a?.get("node")?.jsonPrimitive?.longOrNull ?: 0L,
                    actionType = actionType,
                    value = a?.get("value")?.jsonPrimitive?.doubleOrNull ?: 0.0,
                    message = a?.get("msg")?.jsonPrimitive?.contentOrNull ?: "",
                ),
            )
        }
    }

    private fun decodeCond(o: JsonObject): Condition? {
        fun i(k: String) = o[k]?.jsonPrimitive?.intOrNull ?: 0
        fun l(k: String) = o[k]?.jsonPrimitive?.longOrNull ?: 0L
        fun d(k: String) = o[k]?.jsonPrimitive?.doubleOrNull ?: 0.0
        return when (i("type")) {
            0 -> Condition.Time("${two(i("hS"))}:${two(i("mS"))}", "${two(i("hE"))}:${two(i("mE"))}")
            1 -> {
                val op = ordOp(i("op"))
                Condition.Param(
                    node = l("node"),
                    param = ordParam[i("p")] ?: "T1",
                    op = op,
                    value = if (op == CompareOp.Gt) d("mn") else d("mx"),
                )
            }
            2 -> {
                val op = ordOp(i("op"))
                Condition.Delta(
                    node1 = l("node1"), param1 = ordParam[i("p1")] ?: "T1",
                    node2 = l("node2"), param2 = ordParam[i("p2")] ?: "sBuforTemp",
                    op = op,
                    min = if (op == CompareOp.Gt) d("mn") else d("mx"),
                )
            }
            else -> null
        }
    }
}
