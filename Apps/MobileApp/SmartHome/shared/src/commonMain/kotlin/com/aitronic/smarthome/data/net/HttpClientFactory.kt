package com.aitronic.smarthome.data.net

import io.ktor.client.HttpClient

/**
 * Tworzy klienta HTTP z **pinningiem certyfikatu liścia** (SHA-256 DER, hex).
 * Tożsamość bramki gwarantuje pin — dlatego weryfikacja hostname jest wyłączona
 * (bramka ma self-signed cert i bywa adresowana po IP: LAN vs WAN).
 *
 * Android: OkHttp + własny X509TrustManager (port CertPin.java z SmartHomeV2).
 * iOS: Darwin (pinning do dorobienia na Macu — patrz actual).
 */
expect fun createHttpClient(pinSha256: String): HttpClient
