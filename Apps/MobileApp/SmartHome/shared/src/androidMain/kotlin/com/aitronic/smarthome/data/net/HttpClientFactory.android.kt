package com.aitronic.smarthome.data.net

import io.ktor.client.HttpClient
import io.ktor.client.engine.okhttp.OkHttp
import io.ktor.client.plugins.HttpTimeout
import io.ktor.client.plugins.websocket.WebSockets
import okhttp3.OkHttpClient
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import javax.net.ssl.SSLContext
import javax.net.ssl.X509TrustManager

/**
 * TrustManager pinujący certyfikat liścia po SHA-256 (DER) — 1:1 logika z CertPin.java.
 * Nie ufamy łańcuchowi CA (bramka ma self-signed) — pin JEST weryfikacją tożsamości.
 */
private class PinnedTrustManager(private val pinHex: String) : X509TrustManager {
    override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) = Unit

    override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {
        val leaf = chain?.firstOrNull() ?: throw CertificateException("Brak certyfikatu serwera")
        val sha = MessageDigest.getInstance("SHA-256").digest(leaf.encoded)
        val actual = sha.joinToString("") { b -> "%02X".format(b) }
        if (!actual.equals(pinHex, ignoreCase = true)) {
            throw CertificateException("Certyfikat bramki niezgodny z pinned SHA-256")
        }
    }

    override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
}

actual fun createHttpClient(pinSha256: String): HttpClient {
    val tm = PinnedTrustManager(pinSha256)
    val ssl = SSLContext.getInstance("TLS").apply {
        init(null, arrayOf(tm), SecureRandom())
    }
    val ok = OkHttpClient.Builder()
        .sslSocketFactory(ssl.socketFactory, tm)
        .hostnameVerifier { _, _ -> true } // pin jest właściwym sprawdzeniem tożsamości
        .build()

    return HttpClient(OkHttp) {
        engine { preconfigured = ok }
        install(WebSockets)
        install(HttpTimeout)
        expectSuccess = false
    }
}
