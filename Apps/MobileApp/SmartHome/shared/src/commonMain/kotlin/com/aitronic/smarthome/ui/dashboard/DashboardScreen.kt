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
import com.aitronic.smarthome.data.GatewayState
import com.aitronic.smarthome.data.climateStateFor
import com.aitronic.smarthome.data.solarDailyYieldKwhFor
import com.aitronic.smarthome.data.solarStateFor
import com.aitronic.smarthome.data.net.NodeInfoDto
import com.aitronic.smarthome.data.net.NodeTypes
import com.aitronic.smarthome.data.net.Params
import com.aitronic.smarthome.domain.model.ClimateState
import com.aitronic.smarthome.domain.model.DashboardData
import com.aitronic.smarthome.domain.model.SolarState
import com.aitronic.smarthome.ui.icons.ShIcons
import com.aitronic.smarthome.ui.theme.Sh
import com.aitronic.smarthome.ui.theme.deviceColor

@Composable
fun DashboardScreen(
    data: DashboardData,
    store: com.aitronic.smarthome.data.GatewayStore? = null,
    onOpenSolar: () -> Unit,
    onOpenClimate: () -> Unit,
) {
    val gw = store?.state?.collectAsState()?.value
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

        if (gw == null) {
            DashEmpty("Łączenie z bramką…")
        } else {
            // Jedna karta na REALNY node: active (gen2, JOIN) + legacy (gen1 sniff 241/242).
            // Legacy bufor jest wtopiony w kartę legacy solara → nie dostaje własnej karty.
            // Detached/pending → brak karty. Trash → node znika z gw.nodes → karta znika sama.
            val nodes = gw.nodes
                .filter { it.status == "active" || it.status == "legacy" }
                .filterNot { it.status == "legacy" && it.type == NodeTypes.BUFOR }
            if (nodes.isEmpty()) {
                DashEmpty("Brak sparowanych urządzeń.\nDodaj urządzenie w zakładce Urządzenia.")
            } else {
                // Grupowanie po pokoju (jak lista urządzeń); "Bez pokoju" na końcu.
                val groups = nodes.groupBy { it.room.ifBlank { "Bez pokoju" } }
                    .toList().sortedBy { it.first == "Bez pokoju" }
                groups.forEach { (room, roomNodes) ->
                    SectionHeader(room.uppercase())
                    Spacer(Modifier.height(12.dp))
                    roomNodes.forEachIndexed { i, n ->
                        if (i > 0) Spacer(Modifier.height(12.dp))
                        NodeCard(gw, n, onOpenSolar, onOpenClimate)
                    }
                    Spacer(Modifier.height(18.dp))
                }
            }
        }
    }
}

/** Wybór karty wg typu noda. gen1 i gen2 tego samego typu = osobne karty (per-node). */
@Composable
private fun NodeCard(gw: GatewayState, n: NodeInfoDto, onOpenSolar: () -> Unit, onOpenClimate: () -> Unit) {
    val name = n.name.ifBlank { NodeTypes.label(n.type) }
    when (n.type) {
        NodeTypes.SOLAR -> SolarNodeCard(
            name = name,
            // aux (2. bufor) wstrzykujemy tylko dla gen1 (legacy); gen2 → "—" (źródło w settings później)
            state = gw.solarStateFor(n.address, injectAux = n.status == "legacy"),
            dailyYield = gw.solarDailyYieldKwhFor(n.address)?.let { "${fmt1(it)} kWh" },
            onClick = onOpenSolar,
        )
        NodeTypes.TH_SENSOR -> ClimateNodeCard(name, gw.climateStateFor(n.address), onOpenClimate)
        NodeTypes.BUFOR -> BufferNodeCard(name, gw.telemetry[n.address]?.params?.get(Params.SBUF_TEMP))
        else -> GenericNodeCard(name, n.type)
    }
}

@Composable
private fun SectionHeader(text: String) =
    Text(text, color = Sh.textMuted, fontSize = 13.sp, fontWeight = FontWeight.W500, letterSpacing = 0.5.sp)

@Composable
private fun DashEmpty(msg: String) {
    Column(Modifier.fillMaxWidth().padding(top = 48.dp), horizontalAlignment = Alignment.CenterHorizontally) {
        Text(msg, color = Sh.textMuted, fontSize = 14.sp, lineHeight = 21.sp, textAlign = androidx.compose.ui.text.style.TextAlign.Center)
    }
}

/** Temperatura lub "—" (null / NaN = brak danych). */
private fun tLabel(v: Double?): String = if (v == null || v.isNaN()) "—" else "${fmt1(v)}°C"

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
private fun SolarNodeCard(name: String, state: SolarState?, dailyYield: String?, onClick: () -> Unit) {
    HeroCard(brush = Sh.solarTile, shadow = Color(0xFFE1850B).copy(alpha = 0.32f), onClick = onClick) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(ShIcons.Sun, null, tint = Color.White, modifier = Modifier.size(22.dp))
            Spacer(Modifier.width(10.dp))
            Text(name, color = Color.White, fontSize = 17.sp, fontWeight = FontWeight.W500, modifier = Modifier.weight(1f))
            Badge { Icon(ShIcons.Plug, null, tint = Color.White, modifier = Modifier.size(13.dp)); Spacer(Modifier.width(5.dp)); BadgeText("Sieć") }
        }
        Spacer(Modifier.height(16.dp))
        Row(verticalAlignment = Alignment.Bottom) {
            Column {
                if (dailyYield != null) BigValue(dailyYield.substringBefore(" "), " " + dailyYield.substringAfter(" "))
                else BigValue("—", " kWh")
                Text("Uzysk dzienny", color = Color.White.copy(alpha = 0.85f), fontSize = 13.sp, modifier = Modifier.padding(top = 4.dp))
            }
            Spacer(Modifier.width(20.dp))
            SolarBars(Modifier.weight(1f).height(52.dp))
        }
        DetailsRow(
            listOf(
                tLabel(state?.collectorC) to "Kolektor",
                tLabel(state?.mainTankTemps?.firstOrNull()) to "Zbiornik główny",
                tLabel(state?.auxTankC) to "Zbiornik dodatkowy",
            )
        )
    }
}

@Composable
private fun ClimateNodeCard(name: String, state: ClimateState?, onClick: () -> Unit) {
    HeroCard(brush = Sh.climateTile, shadow = Color(0xFF0E7E95).copy(alpha = 0.30f), onClick = onClick) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(ShIcons.ThermoDrop, null, tint = Color.White, modifier = Modifier.size(26.dp))
            Spacer(Modifier.width(12.dp))
            Text(name, color = Color.White, fontSize = 17.sp, fontWeight = FontWeight.W500, modifier = Modifier.weight(1f))
        }
        Spacer(Modifier.height(16.dp))
        Row {
            val t = state?.tempC
            Column { BigValue(if (t == null || t.isNaN()) "—" else fmt1(t), "°C"); Text("Temperatura", color = Color.White.copy(alpha = 0.85f), fontSize = 13.sp, modifier = Modifier.padding(top = 4.dp)) }
            Spacer(Modifier.width(32.dp))
            val h = state?.humidity ?: -1
            Column { BigValue(if (h < 0) "—" else "$h", "%"); Text("Wilgotność", color = Color.White.copy(alpha = 0.85f), fontSize = 13.sp, modifier = Modifier.padding(top = 4.dp)) }
        }
    }
}

@Composable
private fun BufferNodeCard(name: String, tempC: Double?) {
    HeroCard(brush = Brush.linearGradient(listOf(Color(0xFF3E8FA8), Color(0xFF2C6E85))), shadow = Color(0xFF2C6E85).copy(alpha = 0.28f)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(ShIcons.Droplet, null, tint = Color.White, modifier = Modifier.size(22.dp))
            Spacer(Modifier.width(10.dp))
            Text(name, color = Color.White, fontSize = 17.sp, fontWeight = FontWeight.W500, modifier = Modifier.weight(1f))
        }
        Spacer(Modifier.height(16.dp))
        Column { BigValue(if (tempC == null || tempC.isNaN()) "—" else fmt1(tempC), "°C"); Text("Temperatura bufora", color = Color.White.copy(alpha = 0.85f), fontSize = 13.sp, modifier = Modifier.padding(top = 4.dp)) }
    }
}

@Composable
private fun GenericNodeCard(name: String, type: Int) {
    val col = deviceColor(NodeTypes.toUiType(type))
    Row(
        Modifier.fillMaxWidth()
            .shadow(4.dp, RoundedCornerShape(20.dp), spotColor = Sh.cardShadow, ambientColor = Sh.cardShadow)
            .clip(RoundedCornerShape(20.dp)).background(Sh.surface).padding(16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(40.dp).clip(RoundedCornerShape(14.dp)).background(col.bg), contentAlignment = Alignment.Center) {
            Icon(ShIcons.Monitor, null, tint = col.c, modifier = Modifier.size(22.dp))
        }
        Spacer(Modifier.width(14.dp))
        Column(Modifier.weight(1f)) {
            Text(name, color = Sh.textPrimary, fontSize = 15.sp, fontWeight = FontWeight.W500)
            Text(NodeTypes.label(type), color = Sh.textMuted, fontSize = 12.sp)
        }
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
