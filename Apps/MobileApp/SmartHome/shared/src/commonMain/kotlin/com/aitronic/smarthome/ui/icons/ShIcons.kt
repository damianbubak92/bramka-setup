package com.aitronic.smarthome.ui.icons

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.unit.dp

/**
 * Ikony odtworzone 1:1 ze ścieżek SVG z design handoffu (viewBox 0 0 24 24, stroke, round cap/join).
 * Kolor bazowy = czarny; do kolorowania używać Icon(tint = ...) (ColorFilter nadpisuje stroke).
 */
private fun strokeIcon(name: String, vararg paths: String, strokeWidth: Float = 2f): ImageVector {
    val b = ImageVector.Builder(
        name = name,
        defaultWidth = 24.dp, defaultHeight = 24.dp,
        viewportWidth = 24f, viewportHeight = 24f,
    )
    for (p in paths) {
        b.addPath(
            pathData = PathParser().parsePathString(p).toNodes(),
            fill = null,
            stroke = SolidColor(Color.Black),
            strokeLineWidth = strokeWidth,
            strokeLineCap = StrokeCap.Round,
            strokeLineJoin = StrokeJoin.Round,
        )
    }
    return b.build()
}

// Okrąg jako ścieżka: środek (cx,cy), promień r.
private fun circle(cx: Float, cy: Float, r: Float): String =
    "M${cx - r},$cy a$r,$r 0 1,0 ${2 * r},0 a$r,$r 0 1,0 ${-2 * r},0"

object ShIcons {
    val Sun: ImageVector = strokeIcon("sun",
        circle(12f, 12f, 4.5f),
        "M12 2v2M12 20v2M2 12h2M20 12h2M5 5l1.5 1.5M17.5 17.5 19 19M19 5l-1.5 1.5M6.5 17.5 5 19",
    )

    val ThermoDrop: ImageVector = strokeIcon("thermo_drop",
        "M8 13V5a2 2 0 114 0v8a3.5 3.5 0 11-4 0z",
        "M18 3.5c1.7 2.1 2.7 3.6 2.7 5a2.7 2.7 0 11-5.4 0c0-1.4 1-2.9 2.7-5z",
    )

    val Bolt: ImageVector = strokeIcon("bolt",
        "M13 2 4 14h7l-1 8 9-12h-7z",
    )

    val Home: ImageVector = strokeIcon("home",
        "M3 10.5 12 3l9 7.5",
        "M5 9.5V21h14V9.5",
    )

    val Monitor: ImageVector = strokeIcon("monitor",
        "M3 4h18v13H3z",
        "M8 21h8M12 17v4",
    )

    val Person: ImageVector = strokeIcon("person",
        circle(12f, 8f, 4f),
        "M4 20c0-3.5 3.6-6 8-6s8 2.5 8 6",
    )

    val Plug: ImageVector = strokeIcon("plug",
        "M9 2 v6 M15 2 v6",
        "M6 8 h12 v2 a6 6 0 0 1 -6 6 a6 6 0 0 1 -6 -6 z",
        "M12 16 v6",
    )

    val Clock: ImageVector = strokeIcon("clock",
        circle(12f, 12f, 9f),
        "M12 7v5l3 2",
    )

    val Flame: ImageVector = strokeIcon("flame",
        "M14 14.8V5a2 2 0 00-4 0v9.8a4 4 0 104 0z",
    )

    val ChevronLeft: ImageVector = strokeIcon("chevron_left",
        "M15 18 l-6 -6 6 -6",
    )

    val ChevronRight: ImageVector = strokeIcon("chevron_right",
        "M9 18 l6 -6 -6 -6",
    )

    val ChevronDown: ImageVector = strokeIcon("chevron_down",
        "M6 9 l6 6 6 -6",
    )

    val Plus: ImageVector = strokeIcon("plus",
        "M12 5 v14 M5 12 h14",
        strokeWidth = 2.2f,
    )

    val Pencil: ImageVector = strokeIcon("pencil",
        "M12 20 h9",
        "M16.5 3.5 a2.1 2.1 0 0 1 3 3 L7 19 l-4 1 1 -4 z",
    )

    val Trash: ImageVector = strokeIcon("trash",
        "M3 6 h18 M8 6 V4 a2 2 0 0 1 2 -2 h4 a2 2 0 0 1 2 2 v2 M19 6 l-1 14 a2 2 0 0 1 -2 2 H8 a2 2 0 0 1 -2 -2 L5 6",
    )

    val Lock: ImageVector = strokeIcon("lock",
        "M5 11 h14 v10 H5 z",
        "M8 11 V7 a4 4 0 0 1 8 0 v4",
    )

    val JoinRing: ImageVector = strokeIcon("join",
        circle(12f, 12f, 9f),
        "M12 8 v8 M8 12 h8",
    )

    val AlertCircle: ImageVector = strokeIcon("alert",
        circle(12f, 12f, 9f),
        "M12 8 v5 M12 16.5 v.01",
    )

    val Bulb: ImageVector = strokeIcon("bulb",
        "M10 21 h4",
        "M12 3 a6 6 0 0 1 4 10.5 c-.8 .8 -1 1.5 -1 2.5 H9 c0 -1 -.2 -1.7 -1 -2.5 A6 6 0 0 1 12 3 z",
    )

    val Blinds: ImageVector = strokeIcon("blinds",
        "M4 4 h16",
        "M6 4 v13 a2 2 0 0 0 2 2 h8 a2 2 0 0 0 2 -2 V4",
        "M9 9 h6 M9 13 h6",
    )

    val Droplet: ImageVector = strokeIcon("droplet",
        "M12 3 c3 4 5 6.5 5 9 a5 5 0 0 1 -10 0 c0 -2.5 2 -5 5 -9 z",
    )

    val Check: ImageVector = strokeIcon("check",
        "M20 6 9 17 l-5 -5",
        strokeWidth = 2.4f,
    )

    // Bateria: szeroka (viewBox 26x14), obudowa=stroke, poziom+końcówka=fill. Renderować size(20.dp, 11.dp).
    val Battery: ImageVector = ImageVector.Builder(
        name = "battery",
        defaultWidth = 26.dp, defaultHeight = 14.dp,
        viewportWidth = 26f, viewportHeight = 14f,
    ).apply {
        addPath( // obudowa
            pathData = PathParser().parsePathString(
                "M3.5 1 h16 a2.5 2.5 0 0 1 2.5 2.5 v7 a2.5 2.5 0 0 1 -2.5 2.5 h-16 a2.5 2.5 0 0 1 -2.5 -2.5 v-7 a2.5 2.5 0 0 1 2.5 -2.5 z"
            ).toNodes(),
            fill = null, stroke = SolidColor(Color.Black), strokeLineWidth = 1.6f,
            strokeLineCap = StrokeCap.Round, strokeLineJoin = StrokeJoin.Round,
        )
        addPath( // poziom naładowania
            pathData = PathParser().parsePathString(
                "M4 3 h13 a1 1 0 0 1 1 1 v6 a1 1 0 0 1 -1 1 h-13 a1 1 0 0 1 -1 -1 v-6 a1 1 0 0 1 1 -1 z"
            ).toNodes(),
            fill = SolidColor(Color.Black),
        )
        addPath( // końcówka (nub)
            pathData = PathParser().parsePathString(
                "M23 4.5 h1 a1 1 0 0 1 1 1 v3 a1 1 0 0 1 -1 1 h-1 z"
            ).toNodes(),
            fill = SolidColor(Color.Black),
        )
    }.build()
}
