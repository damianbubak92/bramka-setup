package com.aitronic.smarthome.ui.climate

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.aitronic.smarthome.data.GatewayStore
import com.aitronic.smarthome.data.SmartHomeRepository
import com.aitronic.smarthome.data.climateStateFor
import com.aitronic.smarthome.data.net.ClimatePointDto
import com.aitronic.smarthome.domain.model.AxisTick
import com.aitronic.smarthome.domain.model.ClimateMetric
import com.aitronic.smarthome.domain.model.ClimateState
import com.aitronic.smarthome.domain.model.HistoryRange
import com.aitronic.smarthome.domain.model.Series
import com.aitronic.smarthome.ui.common.liveLastUpdateLabel
import com.aitronic.smarthome.ui.icons.ShIcons
import com.aitronic.smarthome.util.formatLocalHm
import kotlin.math.roundToInt

private val Surface = Color(0xFF0E7E95)

/** Który czujnik klimatu otwarto z dashboardu (detal per-node). */
data class ClimateSelection(val name: String, val address: Int, val nodeId: Long)

@Composable
fun ClimateScreen(
    repo: SmartHomeRepository,
    store: GatewayStore? = null,
    sel: ClimateSelection? = null,
    onBack: () -> Unit,
) {
    var metric by remember { mutableStateOf(ClimateMetric.Temperature) }

    // Live per-node: stan bieżący z telemetrii, wykres z bramki. Bez wyboru/bramki → sample.
    val gw = store?.state?.collectAsState()?.value
    val live = sel != null && gw != null
    val liveState: ClimateState? = if (live) gw!!.climateStateFor(sel!!.address) else null
    val ts = if (live) gw!!.telemetry[sel!!.address]?.ts ?: 0L else 0L

    // Historia ostatniej doby — re-fetch przy nowej telemetrii (ts). Oba metryki w points.
    val points by produceState(emptyList<ClimatePointDto>(), live, sel?.nodeId, ts) {
        value = if (live && sel != null) store!!.climateHistory(sel.nodeId).getOrNull() ?: emptyList()
                else emptyList()
    }

    val sample = remember { repo.climate() }
    val tempC = liveState?.tempC ?: sample.tempC
    val humidity = liveState?.humidity ?: sample.humidity
    val batteryPct = liveState?.batteryPct ?: sample.batteryPct
    val title = sel?.name ?: "Czujnik klimatu"

    val series = if (live) buildClimateSeries(points, metric)
                 else repo.climateSeries(metric, HistoryRange.H24)

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
            Text(title, color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.W500)
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
                        Text(if (batteryPct in 0..100) "$batteryPct%" else "—", color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.W500)
                    }
                }
                Spacer(Modifier.height(14.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(40.dp)) {
                    BigReading(if (tempC.isNaN()) "—" else fmt1(tempC), "°C", "Temperatura")
                    BigReading(if (humidity < 0) "—" else "$humidity", "%", "Wilgotność")
                }
                Spacer(Modifier.height(14.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(ShIcons.Clock, null, tint = Color.White, modifier = Modifier.size(14.dp))
                    Spacer(Modifier.width(7.dp))
                    val tsLabel = if (live) liveLastUpdateLabel(ts) ?: "Ostatnia aktualizacja: —"
                                  else "Ostatnia aktualizacja: ${sample.lastMeasuredLabel}"
                    Text(tsLabel, color = Color.White.copy(alpha = 0.85f), fontSize = 12.sp)
                }
            }

            SectionDivider()

            // --- Ostatnia doba ---
            Column(Modifier.padding(horizontal = 24.dp, vertical = 18.dp)) {
                Label("Ostatnia doba")
                Spacer(Modifier.height(14.dp))
                // przełącznik metryki (Temperatura <-> Wilgotność) — zmienia wykres
                Row(
                    Modifier.clip(RoundedCornerShape(16.dp)).background(Color.White.copy(alpha = 0.14f)).padding(4.dp),
                    horizontalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    MetricSeg("Temperatura", metric == ClimateMetric.Temperature, Modifier.weight(1f)) { metric = ClimateMetric.Temperature }
                    MetricSeg("Wilgotność", metric == ClimateMetric.Humidity, Modifier.weight(1f)) { metric = ClimateMetric.Humidity }
                }
                Spacer(Modifier.height(20.dp))
                if (series.values.isEmpty()) {
                    Box(Modifier.fillMaxWidth().height(150.dp), contentAlignment = Alignment.Center) {
                        Text("Brak danych z ostatniej doby\n(zbieranie w toku).", color = Color.White.copy(alpha = 0.7f),
                            fontSize = 13.sp, textAlign = androidx.compose.ui.text.style.TextAlign.Center, lineHeight = 19.sp)
                    }
                } else {
                    ClimateChart(series)
                }
            }
        }
    }
}

/** Buduje serię wykresu z surowych punktów bramki dla wybranej metryki (etykiety osi = godziny). */
private fun buildClimateSeries(points: List<ClimatePointDto>, metric: ClimateMetric): Series {
    val unit = if (metric == ClimateMetric.Temperature) "°C" else "%"
    if (points.isEmpty()) return Series(emptyList(), emptyList(), unit)
    val values = points.map { if (metric == ClimateMetric.Temperature) it.temp else it.hum }
    val n = points.size
    val ticks = if (n == 1) listOf(AxisTick(0f, formatLocalHm(points[0].t)))
        else listOf(0f, 0.25f, 0.5f, 0.75f, 1f).map { f ->
            val idx = (f * (n - 1)).roundToInt().coerceIn(0, n - 1)
            AxisTick(f, formatLocalHm(points[idx].t))
        }
    return Series(values, ticks, unit)
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

// Format PL: 1 miejsce po przecinku.
private fun fmt1(v: Double): String {
    val r = kotlin.math.round(v * 10).toLong()
    val whole = r / 10
    val frac = (if (r < 0) -r else r) % 10
    return "$whole,$frac"
}
