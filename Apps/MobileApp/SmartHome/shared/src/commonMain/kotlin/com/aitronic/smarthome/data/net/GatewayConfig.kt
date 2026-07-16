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
    }
}

/** Skąd ostatnio udało się pobrać dane — do pokazania w UI (status połączenia). */
enum class GatewaySource { Lan, Remote, Offline }
