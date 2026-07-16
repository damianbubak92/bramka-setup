package com.aitronic.smarthome.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.ui.graphics.vector.ImageVector
import com.aitronic.smarthome.data.GatewayStore
import com.aitronic.smarthome.data.SampleRepository
import com.aitronic.smarthome.data.SmartHomeRepository
import com.aitronic.smarthome.ui.auto.AutomationsRoot
import com.aitronic.smarthome.ui.climate.ClimateScreen
import com.aitronic.smarthome.ui.dashboard.DashboardScreen
import com.aitronic.smarthome.ui.devices.DevicesRoot
import com.aitronic.smarthome.ui.icons.ShIcons
import com.aitronic.smarthome.ui.solar.SolarScreen
import com.aitronic.smarthome.ui.theme.Sh

private enum class Tab(val label: String, val icon: ImageVector) {
    Dashboard("Dashboard", ShIcons.Home),
    Automations("Automatyzacje", ShIcons.Bolt),
    Devices("Urządzenia", ShIcons.Monitor),
}
private enum class Detail { None, Climate, Solar }

/**
 * Główny szkielet apki. Nawigacja state-owa (płytka, jak prototyp HTML):
 * dolne taby + pełnoekranowe detale (Solar/Klimat) otwierane z kafli dashboardu.
 * Repozytorium wstrzykiwane (na razie SampleRepository) — Stage 2 podmieni na sieciowe.
 */
@Composable
fun AppScaffold(store: GatewayStore? = null, repo: SmartHomeRepository = SampleRepository) {
    var tab by remember { mutableStateOf(Tab.Dashboard) }
    var detail by remember { mutableStateOf(Detail.None) }

    Column(Modifier.fillMaxSize().background(Sh.bg)) {
        Box(Modifier.weight(1f)) {
            when (detail) {
                Detail.Climate -> ClimateScreen(repo) { detail = Detail.None }
                Detail.Solar -> SolarScreen(repo, store) { detail = Detail.None }
                Detail.None -> when (tab) {
                    Tab.Dashboard -> DashboardScreen(
                        data = repo.dashboard(),
                        store = store,
                        onOpenSolar = { detail = Detail.Solar },
                        onOpenClimate = { detail = Detail.Climate },
                    )
                    Tab.Automations -> AutomationsRoot(repo, store)
                    Tab.Devices -> DevicesRoot(repo, store)
                }
            }
        }
        // Dolna nawigacja tylko na ekranach głównych (na detalu ukryta).
        if (detail == Detail.None) {
            BottomNav(current = tab, onSelect = { tab = it })
        }
    }
}

@Composable
private fun BottomNav(current: Tab, onSelect: (Tab) -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .background(Sh.surface)
            .windowInsetsPadding(WindowInsets.navigationBars)
            .padding(top = 8.dp, bottom = 10.dp)
            .padding(horizontal = 12.dp),
    ) {
        Tab.entries.forEach { t ->
            val active = t == current
            // Animacje: pigułka pojawia się/rozpływa (fade + scale), tint płynnie.
            val spec = tween<Float>(240)
            val pillAlpha by animateFloatAsState(if (active) 1f else 0f, spec, label = "pillAlpha")
            val pillScale by animateFloatAsState(if (active) 1f else 0.7f, tween(240), label = "pillScale")
            val tint by animateColorAsState(if (active) Sh.textPrimary else Sh.textMuted, tween(240), label = "tint")
            val labelColor by animateColorAsState(if (active) Sh.textPrimary else Sh.textMuted, tween(240), label = "labelColor")
            val noRipple = remember { MutableInteractionSource() }

            Column(
                Modifier.weight(1f).clickable(interactionSource = noRipple, indication = null) { onSelect(t) },
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Box(contentAlignment = Alignment.Center) {
                    // Pigułka jako osobna warstwa — animowana niezależnie od ikony (brak przeskoku layoutu).
                    Box(
                        Modifier
                            .matchParentSize()
                            .graphicsLayer { alpha = pillAlpha; scaleX = pillScale; scaleY = pillScale }
                            .clip(RoundedCornerShape(16.dp))
                            .background(Sh.navActivePill),
                    )
                    Icon(
                        t.icon, t.label,
                        tint = tint,
                        modifier = Modifier.padding(horizontal = 18.dp, vertical = 6.dp).size(26.dp),
                    )
                }
                Spacer(Modifier.height(4.dp))
                Text(t.label, color = labelColor, fontSize = 11.sp, fontWeight = FontWeight.W500)
            }
        }
    }
}

