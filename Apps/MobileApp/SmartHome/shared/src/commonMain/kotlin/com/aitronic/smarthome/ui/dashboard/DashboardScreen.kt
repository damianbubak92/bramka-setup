package com.aitronic.smarthome.ui.dashboard

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import com.aitronic.smarthome.data.solarDailyYieldKwh
import com.aitronic.smarthome.data.solarState
import com.aitronic.smarthome.domain.model.DashboardData
import com.aitronic.smarthome.domain.model.PvState
import com.aitronic.smarthome.domain.model.RoomTile
import com.aitronic.smarthome.ui.icons.ShIcons
import com.aitronic.smarthome.ui.theme.Sh
import com.aitronic.smarthome.ui.theme.deviceColor
import androidx.compose.ui.graphics.vector.ImageVector

@Composable
fun DashboardScreen(
    data: DashboardData,
    store: com.aitronic.smarthome.data.GatewayStore? = null,
    onOpenSolar: () -> Unit,
    onOpenClimate: () -> Unit,
) {
    // Live tylko dla systemu solarnego — Klimat/PV/Pokoje są na razie atrapami
    // (nod T&H w drodze; PV nie istnieje w bramce).
    val gw = store?.state?.collectAsState()?.value
    val liveSolar = gw?.let { s -> s.solarState()?.let { st -> s to st } }
    val data = if (liveSolar == null) data else data.copy(
        solar = liveSolar.second,
        solarDailyYield = liveSolar.first.solarDailyYieldKwh()
            ?.let { "${fmt1(it)} kWh" } ?: data.solarDailyYield,
    )
    Column(
        Modifier
            .fillMaxSize()
            .background(Sh.bg)
            .windowInsetsPadding(WindowInsets.statusBars)
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 24.dp)
            .padding(bottom = 20.dp),
    ) {
        Header(data.greetingName, data.statusLine)

        Spacer(Modifier.height(16.dp))
        Text("BEZ POKOJU", color = Sh.textMuted, fontSize = 13.sp, fontWeight = FontWeight.W500, letterSpacing = 0.5.sp)

        Spacer(Modifier.height(12.dp))
        SolarTile(data, onOpenSolar)
        Spacer(Modifier.height(12.dp))
        ClimateTile(data, onOpenClimate)
        Spacer(Modifier.height(12.dp))
        PvCard(data.pv)

        Spacer(Modifier.height(14.dp))
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text("Pokoje", color = Sh.textPrimary, fontSize = 16.sp, fontWeight = FontWeight.W500, modifier = Modifier.weight(1f))
            Text("Wszystkie", color = Sh.textPrimary, fontSize = 13.sp, fontWeight = FontWeight.W500)
        }
        Spacer(Modifier.height(10.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            data.rooms.forEach { room -> RoomCard(room, Modifier.weight(1f)) }
        }
    }
}

@Composable
private fun Header(name: String, status: String) {
    Row(
        Modifier.fillMaxWidth().padding(top = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text("Dzień dobry, $name", color = Sh.textMuted, fontSize = 14.sp)
            Text(status, color = Sh.textPrimary, fontSize = 26.sp, fontWeight = FontWeight.W500, letterSpacing = (-0.2).sp)
        }
        Box(
            Modifier.size(44.dp).clip(CircleShape).background(Color(0xFFEFE7D8)),
            contentAlignment = Alignment.Center,
        ) {
            Icon(ShIcons.Person, null, tint = Sh.textPrimary, modifier = Modifier.size(22.dp))
        }
    }
}

@Composable
private fun SolarTile(data: DashboardData, onClick: () -> Unit) {
    HeroCard(brush = Sh.solarTile, shadow = Color(0xFFE1850B).copy(alpha = 0.32f), onClick = onClick) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(ShIcons.Sun, null, tint = Color.White, modifier = Modifier.size(22.dp))
            Spacer(Modifier.width(10.dp))
            Text("System solarny", color = Color.White, fontSize = 17.sp, fontWeight = FontWeight.W500, modifier = Modifier.weight(1f))
            Badge { Icon(ShIcons.Plug, null, tint = Color.White, modifier = Modifier.size(13.dp)); Spacer(Modifier.width(5.dp)); BadgeText("Sieć") }
        }
        Spacer(Modifier.height(16.dp))
        Row(verticalAlignment = Alignment.Bottom) {
            Column {
                BigValue(data.solarDailyYield.substringBefore(" "), " " + data.solarDailyYield.substringAfter(" "))
                Text("Uzysk dzienny", color = Color.White.copy(alpha = 0.85f), fontSize = 13.sp, modifier = Modifier.padding(top = 4.dp))
            }
            Spacer(Modifier.width(20.dp))
            SolarBars(Modifier.weight(1f).height(52.dp))
        }
        DetailsRow(
            listOf(
                "${fmt1(data.solar.collectorC)}°C" to "Kolektor",
                "${fmt1(data.solar.mainTankTemps.first())}°C" to "Zbiornik główny",
                "${fmt1(data.solar.auxTankC)}°C" to "Zbiornik dodatkowy",
            )
        )
    }
}

@Composable
private fun ClimateTile(data: DashboardData, onClick: () -> Unit) {
    HeroCard(brush = Sh.climateTile, shadow = Color(0xFF0E7E95).copy(alpha = 0.30f), onClick = onClick) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(ShIcons.ThermoDrop, null, tint = Color.White, modifier = Modifier.size(26.dp))
            Spacer(Modifier.width(12.dp))
            Text("Czujnik klimatu", color = Color.White, fontSize = 17.sp, fontWeight = FontWeight.W500, modifier = Modifier.weight(1f))
            Badge { Icon(ShIcons.Battery, null, tint = Color.White, modifier = Modifier.size(width = 20.dp, height = 11.dp)); Spacer(Modifier.width(6.dp)); BadgeText("${data.climate.batteryPct}%") }
        }
        Spacer(Modifier.height(16.dp))
        Row {
            Column { BigValue("${fmt1(data.climate.tempC)}", "°C"); Text("Temperatura", color = Color.White.copy(alpha = 0.85f), fontSize = 13.sp, modifier = Modifier.padding(top = 4.dp)) }
            Spacer(Modifier.width(32.dp))
            Column { BigValue("${data.climate.humidity}", "%"); Text("Wilgotność", color = Color.White.copy(alpha = 0.85f), fontSize = 13.sp, modifier = Modifier.padding(top = 4.dp)) }
        }
        Spacer(Modifier.height(16.dp))
        Divider()
        Spacer(Modifier.height(14.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(ShIcons.Clock, null, tint = Color.White, modifier = Modifier.size(15.dp))
            Spacer(Modifier.width(7.dp))
            Text("Pomiar ${data.climate.lastMeasuredLabel} · interwał ${data.climate.intervalMin} min", color = Color.White.copy(alpha = 0.9f), fontSize = 12.sp)
        }
    }
}

@Composable
private fun PvCard(pv: PvState) {
    HeroCard(brush = Brush.linearGradient(listOf(Color(0xFFE8664F), Color(0xFFC0392B))), shadow = Color(0xFFC0392B).copy(alpha = 0.28f)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(ShIcons.Bolt, null, tint = Color.White, modifier = Modifier.size(24.dp))
            Spacer(Modifier.width(12.dp))
            Text("Fotowoltaika", color = Color.White, fontSize = 17.sp, fontWeight = FontWeight.W500, modifier = Modifier.weight(1f))
            Badge { BadgeText("z ${pv.capacityKwp}") }
        }
        Spacer(Modifier.height(16.dp))
        Column { BigValue(pv.powerKw.substringBefore(" "), " " + pv.powerKw.substringAfter(" ")); Text("Produkcja teraz", color = Color.White.copy(alpha = 0.85f), fontSize = 13.sp, modifier = Modifier.padding(top = 4.dp)) }
    }
}

@Composable
private fun RoomCard(room: RoomTile, modifier: Modifier = Modifier) {
    val col = deviceColor(room.accentType)
    Column(
        modifier
            .shadow(4.dp, RoundedCornerShape(20.dp), spotColor = Sh.cardShadow, ambientColor = Sh.cardShadow)
            .clip(RoundedCornerShape(20.dp))
            .background(Sh.surface)
            .padding(14.dp),
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(room.name, color = Sh.textPrimary, fontSize = 14.sp, fontWeight = FontWeight.W500, modifier = Modifier.weight(1f))
            Icon(ShIcons.Flame, null, tint = col.c, modifier = Modifier.size(18.dp))
        }
        Text(room.tempLabel, color = Sh.textPrimary, fontSize = 26.sp, fontWeight = FontWeight.W300, modifier = Modifier.padding(top = 6.dp))
        Text(room.subtitle, color = Sh.textMuted, fontSize = 12.sp)
    }
}

// --- Wspólne elementy ---

@Composable
private fun HeroCard(
    brush: Brush,
    shadow: Color,
    onClick: (() -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    var m = Modifier
        .fillMaxWidth()
        .shadow(16.dp, RoundedCornerShape(26.dp), spotColor = shadow, ambientColor = shadow)
        .clip(RoundedCornerShape(26.dp))
        .background(brush)
    if (onClick != null) m = m.clickable { onClick() }
    Column(m.padding(horizontal = 20.dp, vertical = 18.dp), content = content)
}

@Composable
private fun Badge(content: @Composable RowScope.() -> Unit) {
    Row(
        Modifier.clip(RoundedCornerShape(20.dp)).background(Color.White.copy(alpha = 0.20f))
            .padding(horizontal = 10.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        content = content,
    )
}

@Composable
private fun BadgeText(text: String) =
    Text(text, color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.W500)

@Composable
private fun RowScope.BigValue(value: String, unit: String) {
    Row(verticalAlignment = Alignment.Bottom) {
        Text(value, color = Color.White, fontSize = 40.sp, fontWeight = FontWeight.W300, lineHeight = 40.sp)
        Text(unit, color = Color.White, fontSize = 18.sp, fontWeight = FontWeight.W400, modifier = Modifier.padding(bottom = 4.dp))
    }
}

@Composable
private fun ColumnScope.BigValue(value: String, unit: String) {
    Row(verticalAlignment = Alignment.Bottom) {
        Text(value, color = Color.White, fontSize = 40.sp, fontWeight = FontWeight.W300, lineHeight = 40.sp)
        Text(unit, color = Color.White, fontSize = 18.sp, fontWeight = FontWeight.W400, modifier = Modifier.padding(bottom = 4.dp))
    }
}

@Composable
private fun ColumnScope.DetailsRow(items: List<Pair<String, String>>) {
    Spacer(Modifier.height(16.dp))
    Divider()
    Spacer(Modifier.height(14.dp))
    Row(horizontalArrangement = Arrangement.spacedBy(22.dp)) {
        items.forEach { (value, label) ->
            Column {
                Text(value, color = Color.White, fontSize = 16.sp, fontWeight = FontWeight.W500, fontFamily = FontFamily.Monospace)
                Text(label, color = Color.White.copy(alpha = 0.8f), fontSize = 11.sp)
            }
        }
    }
}

@Composable
private fun Divider() = Box(Modifier.fillMaxWidth().height(1.dp).background(Color.White.copy(alpha = 0.25f)))

@Composable
private fun SolarBars(modifier: Modifier = Modifier) {
    // Wysokości i przezroczystości 1:1 z handoffu (8 słupków).
    val bars = listOf(
        0.24f to 0.55f, 0.48f to 0.55f, 0.70f to 0.70f, 0.92f to 1f,
        0.80f to 0.70f, 0.88f to 1f, 0.60f to 0.55f, 0.34f to 0.40f,
    )
    Row(modifier, horizontalArrangement = Arrangement.spacedBy(3.dp), verticalAlignment = Alignment.Bottom) {
        bars.forEach { (h, a) ->
            Box(
                Modifier.weight(1f).fillMaxHeight(h).clip(RoundedCornerShape(3.dp))
                    .background(Color.White.copy(alpha = a)),
            )
        }
    }
}

// Format PL: 1 miejsce po przecinku.
private fun fmt1(v: Double): String {
    val r = kotlin.math.round(v * 10).toLong()
    val whole = r / 10
    val frac = (if (r < 0) -r else r) % 10
    return "$whole,$frac"
}
