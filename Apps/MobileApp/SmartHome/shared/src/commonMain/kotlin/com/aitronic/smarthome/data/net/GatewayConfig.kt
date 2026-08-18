package com.aitronic.smarthome.data.net

/**
 * Konfiguracja dostępu do bramki. Wartości domyślne = realny stan instalacji
 * (zweryfikowane w Gateway/Software/rpmsg-service + starej apce SmartHomeV2).
 *
 * Kaskada (ustalona z userem):
 *   1. [lanBase]    — w domu po Wi-Fi (PRIMARY, bez hairpin NAT)
 *   2. [remoteBase] — poza domem: port-forward teraz, relay na VPS docelowo
 *   3. mirror (PHP/MySQL) — dopiero gdy bramka całkowicie nieosiągalna (Premium; TODO)
 */
data class GatewayConfig(
    val lanHost: String = DEFAULT_LAN_HOST,
    val remoteHost: String = DEFAULT_REMOTE_HOST,
    val port: Int = DEFAULT_PORT,
    val authToken: String = DEFAULT_TOKEN,
    /** SHA-256 certyfikatu liścia (DER, hex). Bramka serwuje ten sam cert.pem co gen1. */
    val certPinSha256: String = DEFAULT_PIN,
    /** Read-only fallback z mirrora (PHP/MySQL gw-read.php) — próbowany DOPIERO gdy
     * bramka nieosiągalna i TYLKO dla odczytów. null = tier bez backupu. Osobny klucz
     * ([mirrorReadKey], NIE [authToken]) — wyciek z apki daje wtedy tylko odczyt. */
    val mirrorUrl: String? = DEFAULT_MIRROR_URL,
    val mirrorReadKey: String = DEFAULT_MIRROR_READ_KEY,
) {
    val lanBase: String get() = "https://$lanHost:$port"
    val remoteBase: String get() = "https://$remoteHost:$port"

    fun wsUrl(base: String): String =
        base.replace("https://", "wss://") + "/ws?token=$authToken"

    companion object {
        const val DEFAULT_LAN_HOST = "192.168.2.170"
        const val DEFAULT_REMOTE_HOST = "91.123.191.192" // WAN -> port-forward 9443 -> bramka
        const val DEFAULT_PORT = 9443
        const val DEFAULT_TOKEN = "c228cecbca32894a526092abd305cddc"
        const val DEFAULT_PIN = "2C8DB42E24E2C5396F20898243C1A4EB3E0A4B3740B7ADBC1CD2B1344DF22B34"
        // Mirror read-fallback. HTTP na razie (brak SSL na domenie) — zmienić na https
        // gdy dojdzie cert. mirrorReadKey MUSI == $READ_KEY z secrets.php na serwerze.
        const val DEFAULT_MIRROR_URL = "http://iot.aitronic.pl/gw-read.php"
        const val DEFAULT_MIRROR_READ_KEY = "5wqjr458wr9a"
    }
}

/** Skąd ostatnio udało się pobrać dane — status połączenia w UI.
 * Mirror = read-only fallback (bramka offline, dane z kopii). */
enum class GatewaySource { Lan, Remote, Mirror, Offline }
