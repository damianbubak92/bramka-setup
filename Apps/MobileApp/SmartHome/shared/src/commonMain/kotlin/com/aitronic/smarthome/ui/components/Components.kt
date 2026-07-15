package com.aitronic.smarthome.ui.components

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.material3.Text
import com.aitronic.smarthome.ui.theme.Sh

/** Etykieta sekcji: uppercase, wyciszona (np. "BEZ POKOJU"). */
@Composable
fun SectionLabel(text: String, color: Color = Sh.textMuted, modifier: Modifier = Modifier) {
    Text(
        text = text.uppercase(),
        color = color,
        fontSize = 12.sp,
        fontWeight = FontWeight.W600,
        letterSpacing = 0.8.sp,
        modifier = modifier,
    )
}

/**
 * Placeholder ikony: zaokrąglony kafelek w kolorze tła + wewnętrzny "znaczek".
 * TODO Stage-UI: podmienić na Material Symbols wg listy ikon z handoffu (dom, piorun, żarówka, ...).
 */
@Composable
fun IconChip(
    accent: Color,
    bg: Color,
    size: Int = 40,
    corner: Int = 14,
    modifier: Modifier = Modifier,
) {
    Box(
        modifier = modifier
            .size(size.dp)
            .clip(RoundedCornerShape(corner.dp)),
    ) {
        Canvas(Modifier.size(size.dp)) {
            drawRoundRect(color = bg, cornerRadius = CornerRadius(corner.dp.toPx(), corner.dp.toPx()))
            val d = this.size.minDimension
            drawRoundRect(
                color = accent,
                topLeft = Offset(d * 0.32f, d * 0.32f),
                size = Size(d * 0.36f, d * 0.36f),
                cornerRadius = CornerRadius(d * 0.10f, d * 0.10f),
            )
        }
    }
}

/** Mini wykres słupkowy (na kaflu solar). Słupki w podanym kolorze. */
@Composable
fun MiniBars(
    values: List<Double>,
    color: Color = Color.White,
    modifier: Modifier = Modifier,
) {
    Canvas(modifier) {
        if (values.isEmpty()) return@Canvas
        val max = (values.maxOrNull() ?: 1.0).coerceAtLeast(0.0001)
        val n = values.size
        val gap = size.width * 0.03f
        val barW = (size.width - gap * (n - 1)) / n
        values.forEachIndexed { i, v ->
            val h = (v / max).toFloat() * size.height
            val x = i * (barW + gap)
            drawRoundRect(
                color = color,
                topLeft = Offset(x, size.height - h),
                size = Size(barW, h),
                cornerRadius = CornerRadius(barW * 0.3f, barW * 0.3f),
            )
        }
    }
}
