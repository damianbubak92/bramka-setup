package com.aitronic.smarthome.ui.climate

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.aitronic.smarthome.data.SmartHomeRepository
import com.aitronic.smarthome.domain.model.ClimateMetric
import com.aitronic.smarthome.domain.model.HistoryRange
import com.aitronic.smarthome.ui.icons.ShIcons
import kotlinx.coroutines.delay

private val Surface = Color(0xFF0E7E95)

@Composable
fun ClimateScreen(repo: SmartHomeRepository, onBack: () -> Unit) {
    val climate = remember { repo.climate() }
    var metric by remember { mutableStateOf(ClimateMetric.Temperature) }
    var range by remember { mutableStateOf(HistoryRange.H24) }
    var interval by remember { mutableStateOf(climate.intervalMin) }
    var savedInterval by remember { mutableStateOf(climate.intervalMin) }
    var showToast by remember { mutableStateOf(false) }

    LaunchedEffect(showToast) {
        if (showToast) { delay(2600); showToast = false }
    }

    Column(
        Modifier.fillMaxSize().background(Surface).windowInsetsPadding(WindowInsets.safeDrawing),
    ) {
        // Top bar (stały)
        Row(
            Modifier.fillMaxWidth().padding(start = 2.dp, end = 14.dp, top = 2.dp, bottom = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                Modifier.size(44.dp).clip(CircleShape)
                    .clickable(remember { MutableInteractionSource() }, indication = null) { onBack() },
                contentAlignment = Alignment.Center,
            ) { Icon(ShIcons.ChevronLeft, "Wstecz", tint = Color.White, modifier = Modifier.size(24.dp)) }
            Icon(ShIcons.ThermoDrop, null, tint = Color.White, modifier = Modifier.size(24.dp))
            Spacer(Modifier.width(6.dp))
            Text("Czujnik klimatu", color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.W500)
        }

        Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
            // --- Aktualny pomiar ---
            Column(Modifier.padding(start = 24.dp, end = 24.dp, top = 6.dp, bottom = 18.dp)) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Label("Aktualny pomiar", Modifier.weight(1f))
                    Row(
                        Modifier.clip(RoundedCornerShape(20.dp)).background(Color.White.copy(alpha = 0.18f))
                            .padding(horizontal = 10.dp, vertical = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Icon(ShIcons.Battery, null, tint = Color.White, modifier = Modifier.size(width = 20.dp, height = 11.dp))
                        Spacer(Modifier.width(6.dp))
                        Text("${climate.batteryPct}%", color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.W500)
                    }
                }
                Spacer(Modifier.height(14.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(40.dp)) {
                    BigReading(fmt1(climate.tempC), "°C", "Temperatura")
                    BigReading("${climate.humidity}", "%", "Wilgotność")
                }
                Spacer(Modifier.height(14.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(ShIcons.Clock, null, tint = Color.White, modifier = Modifier.size(14.dp))
                    Spacer(Modifier.width(7.dp))
                    Text("Pomiar ${climate.lastMeasuredLabel} · interwał $interval min", color = Color.White.copy(alpha = 0.85f), fontSize = 12.sp)
                }
            }

            SectionDivider()

            // --- Dane historyczne ---
            Column(Modifier.padding(horizontal = 24.dp, vertical = 18.dp)) {
                Label("Dane historyczne")
                Spacer(Modifier.height(14.dp))
                // segment metryki
                Row(
                    Modifier.clip(RoundedCornerShape(16.dp)).background(Color.White.copy(alpha = 0.14f)).padding(4.dp),
                    horizontalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    MetricSeg("Temperatura", metric == ClimateMetric.Temperature, Modifier.weight(1f)) { metric = ClimateMetric.Temperature }
                    MetricSeg("Wilgotność", metric == ClimateMetric.Humidity, Modifier.weight(1f)) { metric = ClimateMetric.Humidity }
                }
                Spacer(Modifier.height(14.dp))
                // pigułki zakresu
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    RangePill("24 h", range == HistoryRange.H24) { range = HistoryRange.H24 }
                    RangePill("7 dni", range == HistoryRange.D7) { range = HistoryRange.D7 }
                    RangePill("Miesiąc", range == HistoryRange.Month) { range = HistoryRange.Month }
                    RangePill("Rok", range == HistoryRange.Year) { range = HistoryRange.Year }
                }
                Spacer(Modifier.height(20.dp))
                ClimateChart(remember(metric, range) { repo.climateSeries(metric, range) }, metric)
            }

            SectionDivider()

            // --- Interwał pomiaru ---
            Column(Modifier.padding(start = 24.dp, end = 24.dp, top = 18.dp, bottom = 30.dp)) {
                Label("Interwał pomiaru")
                Spacer(Modifier.height(3.dp))
                Text("Ustaw jak często dokonywać pomiaru", color = Color.White.copy(alpha = 0.65f), fontSize = 12.sp)
                Spacer(Modifier.height(14.dp))
                Row(verticalAlignment = Alignment.Bottom) {
                    Text("$interval", color = Color.White, fontSize = 44.sp, fontWeight = FontWeight.W200, lineHeight = 44.sp)
                    Spacer(Modifier.width(6.dp))
                    Text("min", color = Color.White.copy(alpha = 0.85f), fontSize = 16.sp, modifier = Modifier.padding(bottom = 6.dp))
                }
                Slider(
                    value = interval.toFloat(),
                    onValueChange = { interval = it.toInt() },
                    valueRange = 1f..5f,
                    steps = 3,
                    colors = SliderDefaults.colors(
                        thumbColor = Color.White,
                        activeTrackColor = Color.White,
                        inactiveTrackColor = Color.White.copy(alpha = 0.28f),
                        activeTickColor = Color.White.copy(alpha = 0.6f),
                        inactiveTickColor = Color.White.copy(alpha = 0.4f),
                    ),
                )
                Row(Modifier.fillMaxWidth().padding(horizontal = 2.dp), horizontalArrangement = Arrangement.SpaceBetween) {
                    (1..5).forEach { Text("$it", color = Color.White.copy(alpha = 0.75f), fontSize = 12.sp) }
                }
                Spacer(Modifier.height(20.dp))
                Column(Modifier.fillMaxWidth().heightIn(min = 54.dp)) {
                    val canSave = interval != savedInterval && !showToast
                    if (canSave) {
                        Box(
                            Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(Color.White)
                                .clickable(remember { MutableInteractionSource() }, indication = null) {
                                    savedInterval = interval; showToast = true
                                }
                                .padding(15.dp),
                            contentAlignment = Alignment.Center,
                        ) { Text("Zapisz interwał", color = Surface, fontSize = 15.sp, fontWeight = FontWeight.W500) }
                    }
                    AnimatedVisibility(showToast, enter = fadeIn(), exit = fadeOut()) {
                        Row(
                            Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(Color.White.copy(alpha = 0.18f)).padding(15.dp),
                            horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Icon(ShIcons.Check, null, tint = Color.White, modifier = Modifier.size(20.dp))
                            Spacer(Modifier.width(8.dp))
                            Text("Zapisano", color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.W500)
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun Label(text: String, modifier: Modifier = Modifier) =
    Text(text, color = Color.White.copy(alpha = 0.8f), fontSize = 13.sp, modifier = modifier)

@Composable
private fun SectionDivider() =
    Box(Modifier.fillMaxWidth().padding(horizontal = 24.dp).height(1.dp).background(Color.White.copy(alpha = 0.2f)))

@Composable
private fun BigReading(value: String, unit: String, label: String) {
    Column {
        Row(verticalAlignment = Alignment.Bottom) {
            Text(value, color = Color.White, fontSize = 46.sp, fontWeight = FontWeight.W200, lineHeight = 46.sp)
            Text(unit, color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.W400, modifier = Modifier.padding(bottom = 4.dp))
        }
        Text(label, color = Color.White.copy(alpha = 0.8f), fontSize = 13.sp, modifier = Modifier.padding(top = 6.dp))
    }
}

@Composable
private fun MetricSeg(text: String, active: Boolean, modifier: Modifier = Modifier, onClick: () -> Unit) {
    Box(
        modifier.clip(RoundedCornerShape(13.dp))
            .background(if (active) Color.White else Color.Transparent)
            .clickable(remember { MutableInteractionSource() }, indication = null) { onClick() }
            .padding(vertical = 9.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(text, color = if (active) Surface else Color.White.copy(alpha = 0.75f), fontSize = 14.sp, fontWeight = FontWeight.W500)
    }
}

@Composable
private fun RangePill(text: String, active: Boolean, onClick: () -> Unit) {
    Box(
        Modifier.clip(RoundedCornerShape(18.dp))
            .background(if (active) Color.White else Color.Transparent)
            .clickable(remember { MutableInteractionSource() }, indication = null) { onClick() }
            .padding(horizontal = 15.dp, vertical = 7.dp),
    ) {
        Text(text, color = if (active) Surface else Color.White.copy(alpha = 0.75f), fontSize = 13.sp, fontWeight = FontWeight.W500)
    }
}

// Format PL: 1 miejsce po przecinku.
private fun fmt1(v: Double): String {
    val r = kotlin.math.round(v * 10).toLong()
    val whole = r / 10
    val frac = (if (r < 0) -r else r) % 10
    return "$whole,$frac"
}
