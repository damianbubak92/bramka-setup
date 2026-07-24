package com.aitronic.smarthome.util

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

actual fun nowEpochSeconds(): Long = System.currentTimeMillis() / 1000L

actual fun formatLocalDateTime(epochSeconds: Long): String =
    SimpleDateFormat("HH:mm dd-MM-yyyy", Locale.forLanguageTag("pl-PL")).format(Date(epochSeconds * 1000L))

actual fun formatLocalHm(epochSeconds: Long): String =
    SimpleDateFormat("HH:mm", Locale.forLanguageTag("pl-PL")).format(Date(epochSeconds * 1000L))
