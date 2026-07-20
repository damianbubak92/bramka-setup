package com.aitronic.smarthome.ui.theme

import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color

/**
 * Design tokens odtworzone 1:1 z design_handoff_smart_home/README.md.
 * Motyw "ciepły": grafit = chrome, zielony = stany, kolor = typ urządzenia.
 * Pełnoekranowe ekrany szczegółów używają "device-themed surface" (całe tło w kolorze tożsamości).
 */
object Sh {
    // --- Baza (ekran jasny / Dashboard) ---
    val bg = Color(0xFFF7F4EF)            // tło aplikacji (ciepła biel)
    val surface = Color(0xFFFFFFFF)       // karta / powierzchnia
    val textPrimary = Color(0xFF201B13)   // tekst główny
    val textSecondary = Color(0xFF6B675E) // tekst drugorzędny
    val textMuted = Color(0xFF8A857B)     // tekst wyciszony
    val divider = Color(0xFFECE6DA)       // linia / separator
    val navActivePill = Color(0xFFFDECCB) // aktywna pigułka nawigacji

    // --- Bazowe role chrome ---
    val graphite = Color(0xFF201B13)      // chrome / FAB / przyciski "Zapisz"
    val graphiteBtn = Color(0xFF2A2620)   // FAB / przycisk akcji (nieco jaśniejszy)
    val online = Color(0xFF2E9E6B)        // status online / pompa pracuje / przełączniki ON
    val danger = Color(0xFFC0392B)        // usuwanie / potwierdzenie
    val dangerAlt = Color(0xFFC0492B)     // błąd synchronizacji
    val warn = Color(0xFFC98A2B)          // wymaga akcji (np. detached — wymaga JOIN)
    val warnBg = Color(0xFFF6E9CF)        // tło ostrzeżenia (ikona detached)

    // --- Formularze (jasny motyw) ---
    val fieldBorder = Color(0xFFE7E1D6)
    val fieldBg = Color(0xFFF2EDE3)       // pole zablokowane
    val segTrack = Color(0xFFE8E1D4)      // tor segmentu
    val dashed = Color(0xFFCFC7B8)        // ramka przerywana
    val hairline = Color(0xFFF2EDE3)      // cienka linia w karcie
    val andMuted = Color(0xFFB7B0A3)      // "ORAZ" / chevrony pól

    // --- Cienie ---
    // karta (jasny ekran): 0 2px 10px rgba(80,60,20,.06)
    val cardShadow = Color(0xFF503C14).copy(alpha = 0.06f)

    // --- Device-themed surfaces (pełny ekran szczegółów) ---
    val climateSurface = Color(0xFF0E7E95) // turkus
    val solarSurface = Color(0xFFE1850B)   // pomarańcz

    // Kafle na dashboardzie: gradient 135deg
    val climateTile = Brush.linearGradient(listOf(Color(0xFF22B0C6), Color(0xFF0E7E95)))
    val solarTile = Brush.linearGradient(listOf(Color(0xFFF6A21E), Color(0xFFE1850B)))

    // Nakładki na device-themed surface (białe alfy)
    fun onSurfaceDivider() = Color.White.copy(alpha = 0.20f)
    fun onSurfaceLabel() = Color.White.copy(alpha = 0.70f)
    fun onSurfaceGrid() = Color.White.copy(alpha = 0.16f)
    fun onSurfaceInactive() = Color.White.copy(alpha = 0.75f)
}

/** Akcent semantyczny per typ urządzenia (z A_COLORS w prototypie). */
data class DeviceColor(
    val c: Color,      // akcent
    val bg: Color,     // tło ikony (jasne)
    val chipC: Color,  // tekst chipa
    val chipBg: Color, // tło chipa
)

/** Stała mapa kolorów typów urządzeń (klucz = typ). */
val DeviceColors: Map<String, DeviceColor> = mapOf(
    "solar" to DeviceColor(Color(0xFFE1850B), Color(0xFFFDF0D0), Color(0xFF8A5A00), Color(0xFFFBEBCB)),
    "buffer" to DeviceColor(Color(0xFFA15C2B), Color(0xFFEFE1D3), Color(0xFF7A4A1E), Color(0xFFEEDFCF)),
    "pv" to DeviceColor(Color(0xFFC0392B), Color(0xFFF7DDD9), Color(0xFF8E2A20), Color(0xFFF5DAD5)),
    "climate" to DeviceColor(Color(0xFF0E7E95), Color(0xFFD8ECF1), Color(0xFF0A5A6B), Color(0xFFD6EBF0)),
    "light" to DeviceColor(Color(0xFFC99400), Color(0xFFFBEFC7), Color(0xFF7A5F00), Color(0xFFFBEFC7)),
    "blind" to DeviceColor(Color(0xFF2F8F83), Color(0xFFD8ECE8), Color(0xFF0E4A3E), Color(0xFFD8ECE8)),
    "heating" to DeviceColor(Color(0xFFD9542B), Color(0xFFFBE0D5), Color(0xFF8A3A16), Color(0xFFFBE0D5)),
    "hub" to DeviceColor(Color(0xFF2E9E6B), Color(0xFFDDEBE4), Color(0xFF0A5A3A), Color(0xFFDDEBE4)),
)

fun deviceColor(type: String): DeviceColor = DeviceColors[type] ?: DeviceColors.getValue("hub")
