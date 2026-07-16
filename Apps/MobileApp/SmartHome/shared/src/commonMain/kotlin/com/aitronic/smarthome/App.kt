package com.aitronic.smarthome

import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
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
        LaunchedEffect(Unit) { store.start() }
        AppScaffold(store = store)
    }
}
