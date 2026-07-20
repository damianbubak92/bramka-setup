package com.aitronic.smarthome.data.net

import io.ktor.client.HttpClient
import io.ktor.client.plugins.timeout
import io.ktor.client.plugins.websocket.webSocket
import io.ktor.client.request.post
import io.ktor.client.request.setBody
import io.ktor.client.statement.bodyAsText
import io.ktor.http.ContentType
import io.ktor.http.contentType
import io.ktor.http.isSuccess
import io.ktor.websocket.Frame
import io.ktor.websocket.readText
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.flow.channelFlow
import kotlinx.coroutines.delay
import kotlinx.serialization.json.*

/**
 * Klient bramki. Protokół 1:1 z Gateway/Software/rpmsg-service/httpapi.go:
 * POST na "/" z ciałem "command=<X>&authToken=<token>" — serwer matchuje
 * permisywnie przez strings.Contains po query+body (jak gen1), więc wystarczy
 * jedno POST-owe ciało tekstowe.
 *
 * Kaskada: najpierw LAN (krótki timeout), przy niepowodzeniu remote (port-forward).
 * Ostatni działający base jest zapamiętany, żeby nie płacić timeoutu przy każdym żądaniu.
 */
class GatewayClient(
    private val cfg: GatewayConfig,
    private val http: HttpClient = createHttpClient(cfg.certPinSha256),
) {
    private var lastGood: String? = null
    var source: GatewaySource = GatewaySource.Offline
        private set

    private val json = Json { ignoreUnknownKeys = true; isLenient = true }

    /** Wysyła komendę; zwraca surową treść odpowiedzi. Rzuca [GatewayUnreachable] gdy oba base padły. */
    suspend fun command(command: String): String {
        val body = "command=$command&authToken=${cfg.authToken}"
        // kolejność prób: ostatni działający → LAN → remote
        val bases = buildList {
            lastGood?.let { add(it) }
            if (cfg.lanBase !in this) add(cfg.lanBase)
            if (cfg.remoteBase !in this) add(cfg.remoteBase)
        }
        var lastErr: Throwable? = null
        for (base in bases) {
            val isLan = base == cfg.lanBase
            try {
                val res = http.post("$base/") {
                    contentType(ContentType.Text.Plain)
                    setBody(body)
                    timeout {
                        // LAN ma być szybki: nie chcemy czekać, gdy jesteśmy poza domem
                        requestTimeoutMillis = if (isLan) LAN_TIMEOUT_MS else REMOTE_TIMEOUT_MS
                        connectTimeoutMillis = if (isLan) LAN_TIMEOUT_MS else REMOTE_TIMEOUT_MS
                    }
                }
                if (!res.status.isSuccess()) {
                    lastErr = GatewayHttpError(res.status.value, res.bodyAsText())
                    continue
                }
                lastGood = base
                source = if (isLan) GatewaySource.Lan else GatewaySource.Remote
                return res.bodyAsText()
            } catch (t: Throwable) {
                lastErr = t
                if (lastGood == base) lastGood = null // wymuś ponowny wybór
            }
        }
        source = GatewaySource.Offline
        throw GatewayUnreachable("Bramka nieosiągalna (LAN i zdalnie)", lastErr)
    }

    // --- Komendy wysokopoziomowe ---

    /** Pompa dodatkowa. address = konkretny node (null → domyślny solar na bramce). */
    suspend fun pump(on: Boolean, address: Int? = null): Boolean {
        val a = address?.let { "&address=$it" } ?: ""
        return command((if (on) "PUMP_ON" else "PUMP_OFF") + a).trim().equals("OK", ignoreCase = true)
    }

    suspend fun listNodes(): List<NodeInfoDto> = json.decodeFromString(command("listnodes"))

    /** Ostatnia znana telemetria wszystkich nodów (z bazy) — do zasiania stanu przy starcie. */
    suspend fun state(): List<NodeStateDto> = json.decodeFromString(command("state"))

    /** Wykresy uzysku solarnego. range = day|month|year|total; count = ile okresów wstecz;
     * node = node_id konkretnego solara (null → domyślny na bramce). */
    suspend fun solarHistory(range: String, count: Int = 0, node: Long? = null): List<SolarSeriesDto> {
        val c = if (count > 0) "&count=$count" else ""
        val n = node?.let { "&node=$it" } ?: ""
        return json.decodeFromString(command("history&range=$range$c$n"))
    }

    suspend fun listJoins(): List<PendingJoinDto> = json.decodeFromString(command("listjoins"))

    suspend fun approveJoin(factoryHex: String, name: String): ApproveResultDto =
        json.decodeFromString(command("approvejoin&factory=$factoryHex&name=${name.urlEncode()}"))

    /** „Odrzuć" — usuń oczekujący JOIN z rejestru bramki (przypadkowy przycisk). */
    suspend fun rejectJoin(factoryHex: String): Boolean =
        command("rejectjoin&factory=$factoryHex").trim().equals("OK", ignoreCase = true)

    suspend fun removeNode(address: Int): String = command("removenode&address=$address")

    /** Usuń noda po stałym node_id (dla detached — nie ma adresu; nic do powiadomienia). */
    suspend fun removeNodeById(nodeId: Long): String = command("removenode&id=$nodeId")

    /** Zmiana etykiet noda: nazwa + pokój (tylko baza — node o nich nie wie). */
    suspend fun updateNode(address: Int, name: String, room: String): Boolean =
        command("updatenode&address=$address&name=${name.urlEncode()}&room=${room.urlEncode()}")
            .trim().equals("OK", ignoreCase = true)

    /** Wymień chip pod istniejącym AKTYWNYM nodem: nowy chip (factoryHex, właśnie
     * dołączony jako pending) przejmuje adres target; historia (kluczowana po id)
     * zostaje. Bramka wysyła JOIN_ACCEPT do nowego chipa. */
    suspend fun replaceNode(factoryHex: String, targetAddress: Int): ReplaceResultDto =
        json.decodeFromString(command("replacenode&factory=$factoryHex&target=$targetAddress"))

    /** Re-paruj świeży chip na node DETACHED (przywrócony z kosza): bramka alokuje nowy
     * adres i przypina go do stałego node_id → historia (kluczowana po node_id) wraca. */
    suspend fun repairNode(factoryHex: String, nodeId: Long): ReplaceResultDto =
        json.decodeFromString(command("repairnode&factory=$factoryHex&id=$nodeId"))

    /** Kosz — soft-usunięte nody na mirrorze (okno retencji 60 dni). */
    suspend fun listTrash(): List<TrashNodeDto> = json.decodeFromString(command("listtrash"))

    /** Przywróć noda z kosza → wraca lokalnie jako `detached` (czeka na sparowanie). */
    suspend fun restoreNode(id: Long): RestoreResultDto =
        json.decodeFromString(command("restorenode&id=$id"))

    /** Surowy JSON reguł (schemat apki) — parsowanie w warstwie wyżej. */
    suspend fun getRulesJson(): String = command("getrules")

    /** rules= NIE jest url-enkodowane (serwer wycina surowy JSON do "&authToken="). */
    suspend fun setRulesJson(rulesJson: String): Boolean {
        val body = "command=setrules&rules=$rulesJson&authToken=${cfg.authToken}"
        val base = lastGood ?: cfg.lanBase
        val res = http.post("$base/") {
            contentType(ContentType.Text.Plain)
            setBody(body)
            timeout { requestTimeoutMillis = REMOTE_TIMEOUT_MS }
        }
        return res.status.isSuccess() && res.bodyAsText().trim().equals("OK", ignoreCase = true)
    }

    /**
     * Kanał live. Emituje zdarzenia; przy zerwaniu łączy ponownie po [RECONNECT_MS].
     * Base wybierany jak dla komend (ostatni działający → LAN → remote).
     */
    fun events(): Flow<GatewayEvent> = channelFlow {
        while (true) {
            val base = lastGood ?: cfg.lanBase
            try {
                http.webSocket(cfg.wsUrl(base)) {
                    for (frame in incoming) {
                        if (frame is Frame.Text) {
                            parseEvent(frame.readText())?.let { send(it) }
                        }
                    }
                }
            } catch (_: Throwable) {
                // zerwane/niedostępne — spróbuj drugiego base przy następnej pętli
                if (lastGood == cfg.lanBase) lastGood = cfg.remoteBase else lastGood = null
            }
            delay(RECONNECT_MS)
        }
    }

    private fun parseEvent(text: String): GatewayEvent? = try {
        val o = json.parseToJsonElement(text).jsonObject
        when (o["type"]?.jsonPrimitive?.content) {
            "join_pending" -> GatewayEvent.JoinPending(
                o["factory"]?.jsonPrimitive?.content.orEmpty(),
                o["nodeType"]?.jsonPrimitive?.int ?: 0,
            )
            "telemetry" -> GatewayEvent.Telemetry(
                o["address"]?.jsonPrimitive?.int ?: 0,
                o["nodeType"]?.jsonPrimitive?.int ?: 0,
                o["params"]?.jsonObject?.mapValues { it.value.jsonPrimitive.double } ?: emptyMap(),
                o["ts"]?.jsonPrimitive?.long ?: 0L,
            )
            "node_status" -> GatewayEvent.NodeStatus(
                o["address"]?.jsonPrimitive?.int ?: 0,
                o["status"]?.jsonPrimitive?.content.orEmpty(),
            )
            else -> null
        }
    } catch (_: Throwable) {
        null // nieznany/uszkodzony frame nie może wywalić kanału
    }

    companion object {
        const val LAN_TIMEOUT_MS = 1500L
        const val REMOTE_TIMEOUT_MS = 8000L
        const val RECONNECT_MS = 3000L
    }
}

class GatewayUnreachable(message: String, cause: Throwable? = null) : Exception(message, cause)
class GatewayHttpError(val code: Int, val body: String) : Exception("HTTP $code: $body")

/** Minimalne url-encode dla nazwy w approvejoin (serwer robi url.ParseQuery). */
private fun String.urlEncode(): String = buildString {
    for (b in this@urlEncode.encodeToByteArray()) {
        val c = b.toInt().toChar()
        when {
            c.isLetterOrDigit() && c.code < 128 -> append(c)
            c == '-' || c == '_' || c == '.' || c == '~' -> append(c)
            else -> append('%').append((b.toInt() and 0xFF).toString(16).uppercase().padStart(2, '0'))
        }
    }
}
