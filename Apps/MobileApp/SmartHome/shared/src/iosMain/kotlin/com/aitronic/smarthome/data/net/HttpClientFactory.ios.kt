package com.aitronic.smarthome.data.net

import io.ktor.client.HttpClient
import io.ktor.client.engine.darwin.Darwin
import io.ktor.client.plugins.HttpTimeout
import io.ktor.client.plugins.websocket.WebSockets

/**
 * iOS: engine Darwin.
 *
 * ⚠️ TODO (do zrobienia na Macu, przed jakimkolwiek wydaniem iOS):
 * pinning certyfikatu liścia — Darwin wymaga obsługi `handleChallenge`
 * (NSURLAuthenticationChallenge → SecTrust → porównanie SHA-256 DER z [pinSha256]).
 * Tego kodu NIE da się skompilować/zweryfikować na Windows, więc świadomie
 * zostaje na etap iOS. Do tego czasu klient iOS jest BEZ pinningu i nie wolno
 * go wypuszczać.
 */
actual fun createHttpClient(pinSha256: String): HttpClient = HttpClient(Darwin) {
    install(WebSockets)
    install(HttpTimeout)
    expectSuccess = false
}
