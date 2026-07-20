package com.aitronic.smarthome.ui.solar

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.sp
import com.aitronic.smarthome.domain.model.SolarState

private val Orange = Color(0xFFF5A207)
// Górny margines viewBox (0..YSHIFT) był pusty — obcinamy go, przesuwając treść do góry.
private const val YSHIFT = 40f

/**
 * Schemat instalacji solarnej — geometria 1:1 z buildSolarSchema() handoffu (viewBox 344x224).
 * Wartości (temperatury, % pompy, stany) pochodzą z [state] — czyli z live telemetrii.
 * @param pumpAngle kąt obrotu trójkątów pomp (0..360, animowany z zewnątrz)
 */
@Composable
fun SolarSchematic(state: SolarState, pumpAngle: Float, modifier: Modifier = Modifier) {
    val tm = rememberTextMeasurer()
    Canvas(modifier.fillMaxWidth().aspectRatio(344f / (224f - YSHIFT))) {
        drawSchema(state, pumpAngle, tm)
    }
}

private fun DrawScope.drawSchema(state: SolarState, angle: Float, tm: TextMeasurer) {
    val s = size.width / 344f
    val white = Color.White
    fun p(x: Float, y: Float) = Offset(x * s, (y - YSHIFT) * s)

    // etykieta wyśrodkowana / prawa / lewa w jednostkach viewBox
    fun text(cx: Float, cy: Float, str: String, sizePx: Float, weight: FontWeight, alpha: Float, anchor: TextAlign = TextAlign.Center) {
        val res = tm.measure(str, TextStyle(color = white.copy(alpha = alpha), fontSize = sizePx.sp, fontWeight = weight))
        val dx = when (anchor) {
            TextAlign.End -> res.size.width.toFloat()
            TextAlign.Center -> res.size.width / 2f
            else -> 0f
        }
        drawText(res, topLeft = Offset(cx * s - dx, (cy - YSHIFT) * s - res.size.height / 2f))
    }

    fun roundRect(x: Float, y: Float, w: Float, h: Float, r: Float, fill: Brush?, fillColor: Color?, stroke: Color?, sw: Float) {
        val tl = p(x, y); val sz = Size(w * s, h * s); val cr = CornerRadius(r * s, r * s)
        if (fill != null) drawRoundRect(fill, tl, sz, cr)
        else if (fillColor != null) drawRoundRect(fillColor, tl, sz, cr)
        if (stroke != null) drawRoundRect(stroke, tl, sz, cr, style = Stroke(width = sw * s))
    }

    fun tankBrush(topY: Float, botY: Float) = Brush.verticalGradient(
        listOf(white.copy(alpha = 0.30f), white.copy(alpha = 0.05f)),
        startY = (topY - YSHIFT) * s, endY = (botY - YSHIFT) * s,
    )

    // --- rury (za elementami) ---
    val pipe = white.copy(alpha = 0.4f)
    fun drawPipe(x1: Float, y1: Float, x2: Float, y2: Float) =
        drawLine(pipe, p(x1, y1), p(x2, y2), strokeWidth = 5f * s, cap = StrokeCap.Round)
    drawPipe(88f, 98f, 138f, 98f)   // hot out (górna)
    drawPipe(212f, 98f, 262f, 98f)  // hot (górna, z pompą)
    drawPipe(88f, 188f, 138f, 188f) // powrót (dolna)
    drawPipe(212f, 188f, 262f, 188f)

    // --- kolektor ---
    text(58f, 86f, tempLabel(state.collectorC), 15f, FontWeight.W600, 1f)
    roundRect(23f, 92f, 68f, 12f, 3f, null, white.copy(alpha = 0.30f), white.copy(alpha = 0.6f), 1.2f) // manifold
    for (i in 0 until 10) {  // rurki próżniowe
        val x = 27f + i * 6.4f
        roundRect(x, 102f, 3.4f, 87f, 1.7f, null, white.copy(alpha = 0.5f), null, 0f)
    }
    roundRect(23f, 186f, 68f, 5f, 2.5f, null, white.copy(alpha = 0.24f), white.copy(alpha = 0.5f), 1f) // dolna szyna
    text(58f, 212f, "Kolektor", 10f, FontWeight.W400, 0.75f)

    // --- zbiornik główny (4 temperatury pozycyjnie od góry: T4..T1) ---
    roundRect(138f, 52f, 74f, 144f, 13f, tankBrush(52f, 196f), null, white.copy(alpha = 0.6f), 1.5f)
    val ys = listOf(80f, 112f, 144f, 176f)
    state.mainTankTemps.take(4).forEachIndexed { i, t ->
        text(175f, ys[i], tempLabel(t), 12.5f, FontWeight.W500, 1f)
    }
    text(175f, 212f, "Zbiornik główny", 10f, FontWeight.W400, 0.75f)

    // --- zbiornik dodatkowy ---
    roundRect(262f, 86f, 62f, 110f, 12f, tankBrush(86f, 196f), null, white.copy(alpha = 0.6f), 1.5f)
    text(293f, 145f, tempLabel(state.auxTankC), 15f, FontWeight.W600, 1f)
    text(293f, 212f, "Zbiornik dodatkowy", 10f, FontWeight.W400, 0.75f)

    // --- pompy (trójkąt w kółku, obrót wokół centroidu = środek kółka) ---
    fun pump(cx: Float, cy: Float, running: Boolean, label: String) {
        if (label.isNotEmpty()) text(cx, cy - 19f, label, 11f, FontWeight.W600, 1f)
        drawCircle(white, radius = 10.5f * s, center = p(cx, cy))
        drawCircle(white.copy(alpha = 0.5f), radius = 10.5f * s, center = p(cx, cy), style = Stroke(width = 1f * s))
        val center = p(cx, cy)
        val tri = Path().apply {
            moveTo(center.x + 9f * s, center.y + 0f)
            lineTo(center.x - 4.5f * s, center.y + 7.79f * s)
            lineTo(center.x - 4.5f * s, center.y - 7.79f * s)
            close()
        }
        val rot = if (running) angle else 0f
        rotate(rot, pivot = center) {
            drawPath(tri, Orange) // kolor stały — niezależny od pracy pompy
        }
    }
    // pompa kolektora: zmiennoobrotowa, read-only — kręci się gdy flowRate > 0
    // (pct < 0 = brak danych -> "—", zamiast mylącego "0%")
    pump(113f, 98f, running = state.collectorPumpOn,
        label = if (state.collectorPumpPct < 0) "—" else "${state.collectorPumpPct}%")
    // pompa dodatkowa: sterowana przekaźnikiem — kręci się dopiero gdy node POTWIERDZI stan
    pump(237f, 98f, running = state.auxPumpOn, label = "")
}

/** Etykieta temperatury; NaN = brak danych z bramki -> "—" (nie udajemy zera). */
private fun tempLabel(v: Double): String = if (v.isNaN()) "—" else "${fmt1(v)}°C"

/** Format PL, 1 miejsce po przecinku. */
private fun fmt1(v: Double): String {
    val r = kotlin.math.round(v * 10).toLong()
    val whole = r / 10
    val frac = (if (r < 0) -r else r) % 10
    return "$whole,$frac"
}
