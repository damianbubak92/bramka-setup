package com.aitronic.smarthome

import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.compose.ui.tooling.preview.Preview
import com.aitronic.smarthome.data.GatewayStore
import com.aitronic.smarthome.data.net.GatewayClient
import com.aitronic.smarthome.data.net.GatewayConfig
import com.aitronic.smarthome.ui.AppScaffold

@Composable
@Preview
fun App() {
    MaterialTheme {
        val scope = rememberCoroutineScope()
        // Jeden klient/store na całą apkę: kaskada LAN -> zdalnie, kanał WS z reconnectem.
        val store = remember {
            GatewayStore(GatewayClient(GatewayConfig()), scope)
        }
        // WS + odświeżanie TYLKO na foreground (bateria: koniec pingów/WS w tle).
        // ON_START → łącz + przeładuj świeże dane; ON_STOP → rozłącz. Powrót zachowuje
        // się jak świeży start (mirror-first, WS ustala online/offline).
        val lifecycleOwner = LocalLifecycleOwner.current
        DisposableEffect(lifecycleOwner) {
            val obs = LifecycleEventObserver { _, event ->
                when (event) {
                    Lifecycle.Event.ON_START -> store.onForeground()
                    Lifecycle.Event.ON_STOP  -> store.onBackground()
                    else -> {}
                }
            }
            lifecycleOwner.lifecycle.addObserver(obs)
            onDispose { lifecycleOwner.lifecycle.removeObserver(obs) }
        }
        AppScaffold(store = store)
    }
}
