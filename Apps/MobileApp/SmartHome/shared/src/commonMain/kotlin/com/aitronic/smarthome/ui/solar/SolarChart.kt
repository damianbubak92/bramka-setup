package com.aitronic.smarthome.ui.solar

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.sp
import com.aitronic.smarthome.domain.model.SolarPeriod
import kotlin.math.max
import kotlin.math.roundToInt

/** Wykres słupkowy uzysku — geometria 1:1 z buildBar() handoffu (viewBox 322x150). */
@Composable
fun SolarChart(period: SolarPeriod, modifier: Modifier = Modifier) {
    val tm = rememberTextMeasurer()
    Canvas(modifier.fillMaxWidth().aspectRatio(322f / 150f)) {
        drawBars(period, tm)
    }
}

private fun DrawScope.drawBars(period: SolarPeriod, tm: TextMeasurer) {
    val bars = period.bars
    if (bars.isEmpty()) return
    val sx = size.width / 322f
    val syc = size.height / 150f
    fun px(x: Float) = x * sx
    fun py(y: Float) = y * syc

    // marginesy zbalansowane (lewy≈prawy) i przesunięte w lewo
    val X0 = 23f; val X1 = 301f; val Y0 = 14f; val Y1 = 120f
    val n = bars.size
    val maxV = max(bars.max(), 0.0001)
    val hi = maxV * 1.15
    fun yFor(v: Double) = (Y1 - (v / hi) * (Y1 - Y0)).toFloat()
    val slot = (X1 - X0) / n
    val barW = max(slot * 0.82f, 2f)

    val white = Color.White
    val grid = white.copy(alpha = 0.30f)
    val axis = white.copy(alpha = 0.95f)

    // siatka
    for (gy in listOf(Y0, (Y0 + Y1) / 2f, Y1)) {
        drawLine(grid, Offset(px(X0), py(gy)), Offset(px(X1), py(gy)), strokeWidth = 1f)
    }
    // etykiety Y (prawe wyrównanie do x=28)
    val yl = listOf((Y0 + 3f) to hi.roundToInt(), ((Y0 + Y1) / 2f + 3f) to (hi / 2).roundToInt(), (Y1 + 3f) to 0)
    for ((ly, v) in yl) {
        val res = tm.measure("$v", TextStyle(color = axis, fontSize = 9.sp))
        drawText(res, topLeft = Offset(px(19f) - res.size.width, py(ly) - res.size.height / 2f))
    }
    // słupki
    bars.forEachIndexed { i, v ->
        val cx = X0 + slot * (i + 0.5f)
        val y = yFor(v)
        val hgt = max(Y1 - y, 0f)
        val fill = if (v >= maxV * 0.999) white else white.copy(alpha = 0.72f)
        val r = minOf(barW / 2f, 3f)
        drawRoundRect(
            fill,
            topLeft = Offset(px(cx - barW / 2f), py(y)),
            size = Size(barW * sx, hgt * syc),
            cornerRadius = CornerRadius(r * sx, r * sx),
        )
    }
    // etykiety X
    for (t in period.xTicks) {
        val x = X0 + t.fraction * (X1 - X0)
        val res = tm.measure(t.label, TextStyle(color = axis, fontSize = 8.sp))
        drawText(res, topLeft = Offset(px(x) - res.size.width / 2f, py(135f)))
    }
    // jednostka (prawy górny)
    val u = tm.measure(period.unit, TextStyle(color = white.copy(alpha = 0.6f), fontSize = 9.sp))
    drawText(u, topLeft = Offset(px(X1) - u.size.width, py(2f)))
}
