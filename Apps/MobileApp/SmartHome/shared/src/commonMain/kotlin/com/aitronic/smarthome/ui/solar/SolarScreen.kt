package com.aitronic.smarthome.ui.solar

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.*
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import kotlinx.coroutines.delay
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.aitronic.smarthome.data.GatewayStore
import com.aitronic.smarthome.data.SmartHomeRepository
import com.aitronic.smarthome.data.solarState
import com.aitronic.smarthome.domain.model.SolarRange
import com.aitronic.smarthome.domain.model.SolarState
import com.aitronic.smarthome.ui.icons.ShIcons
import kotlinx.coroutines.launch

private val Surface = Color(0xFFE1850B)

/** Ile czekamy na potwierdzenie z noda (SEND_PUMP_STATUS) zanim uznamy próbę za nieudaną. */
private const val PUMP_CONFIRM_MS = 6_000L

/** Stała wysokość paska statusu pompy — rezerwacja miejsca, żeby spinner nie rozpychał layoutu. */
private val PUMP_STATUS_H = 22.dp

/** Brak telemetrii z bramki: same "—", pompy stoją. NaN = wartość nieznana. */
private val NO_SOLAR_DATA = SolarState(
    powerKw = "— kW",
    collectorC = Double.NaN,
    mainTankTemps = List(4) { Double.NaN },
    auxTankC = Double.NaN,
    collectorPumpPct = -1, // -1 = brak danych ("—", nie mylące "0%")
    collectorPumpOn = false,
    auxPumpOn = false,
)

@Composable
fun SolarScreen(repo: SmartHomeRepository, store: GatewayStore? = null, onBack: () -> Unit) {
    val scope = rememberCoroutineScope()
    val gw = store?.state?.collectAsState()?.value
    // Live z bramki (snapshot z bazy przy starcie + push WS). Gdy bramka nie ma telemetrii
    // tego noda — pokazujemy "—" i pompy STOJĄ. Żadnych danych przykładowych: udawanie
    // pracującej pompy byłoby mylące.
    val live = gw?.solarState()
    val solar = live ?: NO_SOLAR_DATA
    var tab by remember { mutableStateOf(SolarRange.Day) }
    val periods = remember(tab) { repo.solarPeriods(tab) }
    var periodIndex by remember(tab) { mutableStateOf(periods.lastIndex.coerceAtLeast(0)) }
    // --- Pompa dodatkowa ---
    // Prawda o stanie pompy pochodzi WYŁĄCZNIE z telemetrii (pumpState) — node odsyła ją
    // natychmiast po wykonaniu komendy (SEND_PUMP_STATUS), więc nie czekamy 2 min.
    // `pending` = wartość zadana, trzymana do czasu potwierdzenia (wtedy toggle jest już
    // przestawiony i widać spinner). Brak potwierdzenia w [PUMP_CONFIRM_MS] -> powrót.
    val confirmed = solar.auxPumpOn
    var pending by remember { mutableStateOf<Boolean?>(null) }
    var pumpError by remember { mutableStateOf<String?>(null) }
    val auxOn = pending ?: confirmed

    LaunchedEffect(confirmed) {
        if (pending != null && confirmed == pending) { pending = null; pumpError = null } // node potwierdził
    }
    LaunchedEffect(pending) {
        if (pending != null) {
            delay(PUMP_CONFIRM_MS)
            if (pending != null) { pending = null; pumpError = "Pompa nie potwierdziła zmiany" }
        }
    }

    // ciągły obrót trójkątów pomp (1.15 s / obrót, liniowo)
    val rot = rememberInfiniteTransition(label = "pump")
    val angle by rot.animateFloat(
        0f, 360f,
        infiniteRepeatable(tween(1150, easing = LinearEasing), RepeatMode.Restart),
        label = "angle",
    )

    val period = periods[periodIndex.coerceIn(0, periods.lastIndex)]

    Column(Modifier.fillMaxSize().background(Surface).windowInsetsPadding(WindowInsets.safeDrawing)) {
        // Top bar
        Row(
            Modifier.fillMaxWidth().padding(start = 2.dp, end = 14.dp, top = 2.dp, bottom = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                Modifier.size(44.dp).clip(CircleShape)
                    .clickable(remember { MutableInteractionSource() }, indication = null) { onBack() },
                contentAlignment = Alignment.Center,
            ) { Icon(ShIcons.ChevronLeft, "Wstecz", tint = Color.White, modifier = Modifier.size(24.dp)) }
            Icon(ShIcons.Sun, null, tint = Color.White, modifier = Modifier.size(24.dp))
            Spacer(Modifier.width(6.dp))
            Text("System solarny", color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.W500)
        }

        Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
            // --- Sekcja 1: moc + schemat ---
            Column(Modifier.padding(start = 24.dp, end = 24.dp, top = 6.dp, bottom = 14.dp)) {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Column {
                        Text("Aktualnie generowana moc", color = Color.White.copy(alpha = 0.8f), fontSize = 13.sp)
                        Spacer(Modifier.height(6.dp))
                        Row(verticalAlignment = Alignment.Bottom) {
                            Text(solar.powerKw.substringBefore(" "), color = Color.White, fontSize = 46.sp, fontWeight = FontWeight.W200, lineHeight = 46.sp)
                            Text(" " + solar.powerKw.substringAfter(" "), color = Color.White, fontSize = 20.sp, modifier = Modifier.padding(bottom = 4.dp))
                        }
                    }
                    Column(horizontalAlignment = Alignment.End) {
                        Text("Pompa dodatkowa", color = Color.White.copy(alpha = 0.85f), fontSize = 12.sp)
                        Spacer(Modifier.height(9.dp))
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(if (auxOn) "ON" else "OFF", color = Color.White.copy(alpha = 0.9f), fontSize = 14.sp, fontWeight = FontWeight.W600)
                            Spacer(Modifier.width(8.dp))
                            PumpToggle(auxOn, enabled = pending == null) {
                                val target = !auxOn
                                pumpError = null
                                pending = target
                                // Prawdziwa komenda: bramka -> M4F -> SPI -> CC1310 -> RF -> node.
                                // Trójkąt ruszy dopiero, gdy node odeśle pumpState (potwierdzenie).
                                scope.launch {
                                    store?.pump(target)?.onFailure {
                                        pending = null
                                        pumpError = "Bramka nie przyjęła komendy"
                                    }
                                }
                            }
                        }
                        // Status próby przełączenia. Miejsce jest ZAREZERWOWANE na stałe —
                        // spinner pojawia się "w locie" i nie przesuwa niczego na ekranie.
                        Box(
                            Modifier.height(PUMP_STATUS_H).padding(top = 6.dp),
                            contentAlignment = Alignment.CenterEnd,
                        ) {
                            when {
                                pending != null -> Row(verticalAlignment = Alignment.CenterVertically) {
                                    CircularProgressIndicator(
                                        color = Color.White, strokeWidth = 2.dp,
                                        modifier = Modifier.size(13.dp),
                                    )
                                    Spacer(Modifier.width(6.dp))
                                    Text(
                                        if (pending == true) "Włączanie pompy…" else "Wyłączanie pompy…",
                                        color = Color.White.copy(alpha = 0.85f), fontSize = 11.sp,
                                    )
                                }
                                pumpError != null -> Text(
                                    pumpError!!, color = Color.White, fontSize = 11.sp, fontWeight = FontWeight.W500,
                                )
                            }
                        }
                    }
                }
                Spacer(Modifier.height(2.dp))
                SolarSchematic(solar, angle)
            }

            SectionDivider()

            // --- Sekcja 2: uzyski ---
            Column(Modifier.padding(start = 20.dp, end = 20.dp, top = 8.dp, bottom = 24.dp)) {
                Text("Uzyski energii", color = Color.White.copy(alpha = 0.8f), fontSize = 13.sp)
                Spacer(Modifier.height(12.dp))
                // taby zakresu (wyśrodkowane)
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally)) {
                    SolarTab("Dzień", tab == SolarRange.Day) { tab = SolarRange.Day }
                    SolarTab("Miesiąc", tab == SolarRange.Month) { tab = SolarRange.Month }
                    SolarTab("Rok", tab == SolarRange.Year) { tab = SolarRange.Year }
                    SolarTab("Całkowite", tab == SolarRange.Total) { tab = SolarRange.Total }
                }
                Spacer(Modifier.height(16.dp))
                // nawigator okresu
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.SpaceBetween) {
                    NavCircle(ShIcons.ChevronLeft, enabled = periodIndex > 0) { if (periodIndex > 0) periodIndex-- }
                    Text(period.label, color = Color.White, fontSize = 17.sp, fontWeight = FontWeight.W500)
                    NavCircle(ShIcons.ChevronRight, enabled = periodIndex < periods.lastIndex) { if (periodIndex < periods.lastIndex) periodIndex++ }
                }
                Spacer(Modifier.height(12.dp))
                SolarChart(period)
                Spacer(Modifier.height(12.dp))
                // podsumowanie
                Box(Modifier.fillMaxWidth().height(1.dp).background(Color.White.copy(alpha = 0.2f)))
                Spacer(Modifier.height(10.dp))
                Row(Modifier.fillMaxWidth()) {
                    Column(Modifier.weight(1f)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Icon(ShIcons.Clock, null, tint = Color.White, modifier = Modifier.size(14.dp))
                            Spacer(Modifier.width(6.dp))
                            Text("Czas pracy pompy", color = Color.White.copy(alpha = 0.8f), fontSize = 12.sp)
                        }
                        Text(period.pumpRuntime, color = Color.White, fontSize = 26.sp, fontWeight = FontWeight.W300, modifier = Modifier.padding(top = 4.dp))
                    }
                    Column(Modifier.weight(1f), horizontalAlignment = Alignment.End) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Icon(ShIcons.Bolt, null, tint = Color.White, modifier = Modifier.size(14.dp))
                            Spacer(Modifier.width(6.dp))
                            Text("Uzysk energii", color = Color.White.copy(alpha = 0.8f), fontSize = 12.sp)
                        }
                        Text(period.energyYield, color = Color.White, fontSize = 26.sp, fontWeight = FontWeight.W300, modifier = Modifier.padding(top = 4.dp))
                    }
                }
            }
        }
    }
}

@Composable
private fun SectionDivider() =
    Box(Modifier.fillMaxWidth().padding(horizontal = 20.dp).height(1.dp).background(Color.White.copy(alpha = 0.2f)))

/** Przełącznik pompy 64x36, uchwyt 28; ON: biały tor + pomarańczowy uchwyt. */
@Composable
private fun PumpToggle(on: Boolean, enabled: Boolean = true, onToggle: () -> Unit) {
    val knobLeft by animateDpAsState(if (on) 32.dp else 4.dp, tween(200), label = "knob")
    val trackColor by animateColorAsState(if (on) Color.White else Color.White.copy(alpha = 0.30f), tween(200), label = "track")
    val knobColor by animateColorAsState(if (on) Surface else Color.White, tween(200), label = "knobC")
    Box(
        Modifier.size(width = 64.dp, height = 36.dp).clip(RoundedCornerShape(18.dp)).background(trackColor)
            // w trakcie próby blokujemy klikanie, żeby nie wysyłać sprzecznych komend
            .clickable(remember { MutableInteractionSource() }, indication = null, enabled = enabled) { onToggle() },
    ) {
        Box(Modifier.padding(start = knobLeft, top = 4.dp).size(28.dp).clip(CircleShape).background(knobColor))
    }
}

@Composable
private fun SolarTab(text: String, active: Boolean, onClick: () -> Unit) {
    Box(
        Modifier.clip(RoundedCornerShape(18.dp))
            .background(if (active) Color.White else Color.Transparent)
            .clickable(remember { MutableInteractionSource() }, indication = null) { onClick() }
            .padding(horizontal = 15.dp, vertical = 7.dp),
    ) {
        Text(text, color = if (active) Surface else Color.White.copy(alpha = 0.75f), fontSize = 13.sp, fontWeight = FontWeight.W500)
    }
}

@Composable
private fun NavCircle(icon: androidx.compose.ui.graphics.vector.ImageVector, enabled: Boolean, onClick: () -> Unit) {
    Box(
        Modifier.size(40.dp).clip(CircleShape).background(Color.White.copy(alpha = 0.14f))
            .clickable(remember { MutableInteractionSource() }, indication = null, enabled = enabled) { onClick() },
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, null, tint = Color.White.copy(alpha = if (enabled) 1f else 0.4f), modifier = Modifier.size(20.dp))
    }
}
