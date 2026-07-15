package com.aitronic.smarthome.ui.climate

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.sp
import com.aitronic.smarthome.domain.model.ClimateMetric
import com.aitronic.smarthome.domain.model.Series
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * Wykres liniowy z wypełnieniem — geometria 1:1 z buildChart() handoffu.
 * viewBox 0..320 x 0..150; obszar rysowania X0=30..X1=300, Y0=16..Y1=120.
 */
@Composable
fun ClimateChart(series: Series, metric: ClimateMetric, modifier: Modifier = Modifier) {
    val tm = rememberTextMeasurer()
    Canvas(modifier.fillMaxWidth().aspectRatio(320f / 150f)) {
        drawChart(series, metric, tm)
    }
}

private fun DrawScope.drawChart(series: Series, metric: ClimateMetric, tm: TextMeasurer) {
    val arr = series.values
    if (arr.isEmpty()) return
    val isTemp = metric == ClimateMetric.Temperature
    var lo = arr.min()
    var hi = arr.max()
    if (isTemp) { lo = min(lo, 22.0); hi = max(hi, 22.0) }
    val pad = max((hi - lo) * 0.15, 1.0)
    lo -= pad; hi += pad

    val sx = size.width / 320f
    val sy = size.height / 150f
    fun px(x: Float) = x * sx
    fun py(y: Float) = y * sy

    val X0 = 30f; val X1 = 300f; val Y0 = 16f; val Y1 = 120f
    val n = arr.size
    fun xFor(i: Int) = if (n == 1) X0 else X0 + (i.toFloat() / (n - 1)) * (X1 - X0)
    fun yFor(v: Double) = (Y1 - (v - lo) / (hi - lo) * (Y1 - Y0)).toFloat()

    val white = Color.White
    val grid = white.copy(alpha = 0.16f)
    val axis = white.copy(alpha = 0.7f)
    val unit = series.unit

    // linie siatki
    for (gy in listOf(16f, 68f, 120f)) {
        drawLine(grid, Offset(px(X0), py(gy)), Offset(px(X1), py(gy)), strokeWidth = 1f)
    }
    // etykiety osi Y (prawe wyrównanie do x=26)
    val yLabels = listOf(19f to hi.roundToInt(), 71f to ((hi + lo) / 2).roundToInt(), 123f to lo.roundToInt())
    for ((ly, v) in yLabels) {
        val res = tm.measure("$v$unit", TextStyle(color = axis, fontSize = 9.sp))
        drawText(res, topLeft = Offset(px(26f) - res.size.width, py(ly) - res.size.height / 2f))
    }

    // linia "zadana 22°C" (tylko temperatura)
    if (isTemp) {
        val ys = yFor(22.0)
        drawLine(
            white.copy(alpha = 0.55f), Offset(px(X0), py(ys)), Offset(px(X1), py(ys)),
            strokeWidth = 1.5f, pathEffect = PathEffect.dashPathEffect(floatArrayOf(5f, 4f)),
        )
        val res = tm.measure("zadana 22°C", TextStyle(color = white.copy(alpha = 0.85f), fontSize = 9.sp))
        drawText(res, topLeft = Offset(px(X1) - res.size.width, py(ys) - res.size.height - 2f))
    }

    // area + linia
    val linePath = Path()
    val areaPath = Path()
    areaPath.moveTo(px(X0), py(Y1))
    arr.forEachIndexed { i, v ->
        val x = px(xFor(i)); val y = py(yFor(v))
        if (i == 0) linePath.moveTo(x, y) else linePath.lineTo(x, y)
        areaPath.lineTo(x, y)
    }
    areaPath.lineTo(px(X1), py(Y1)); areaPath.close()
    drawPath(areaPath, white.copy(alpha = 0.14f))
    drawPath(linePath, white, style = Stroke(width = 2.5f))

    // etykiety osi X (wyśrodkowane)
    for (t in series.xTicks) {
        val x = X0 + t.fraction * (X1 - X0)
        val res = tm.measure(t.label, TextStyle(color = axis, fontSize = 8.sp, textAlign = TextAlign.Center))
        drawText(res, topLeft = Offset(px(x) - res.size.width / 2f, py(135f)))
    }
}
