package com.aitronic.smarthome.ui.common

import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import com.aitronic.smarthome.util.lastUpdateLabel
import com.aitronic.smarthome.util.nowEpochSeconds
import com.aitronic.smarthome.util.relativeTimeLabel
import kotlinx.coroutines.delay

/**
 * Etykieta "Ostatnia aktualizacja: …" odświeżana **w czasie rzeczywistym** bez nowej telemetrii.
 *
 * Zamiast globalnej tablicy timerów: jeden `produceState` na kartę (lekka zawieszona korutyna),
 * który liczy **dokładny delay do najbliższego progu** etykiety i budzi się tylko na nim:
 *   teraz (─60s→) 1 min temu (─co minutę→) N min temu (─>60 min→) czas bezwzględny [stop].
 * Po wejściu w regime czasu bezwzględnego korutyna kończy — etykieta już się nie zmienia.
 * Kluczowany na [tsSeconds]: nowa telemetria restartuje harmonogram (znów "teraz").
 *
 * Compose zarządza cyklem życia: N kart = N niezależnych korutyn, znikają razem z kartą.
 */
@Composable
fun liveLastUpdateLabel(tsSeconds: Long): String? {
    val label by produceState(initialValue = lastUpdateLabel(tsSeconds), tsSeconds) {
        while (true) {
            val now = nowEpochSeconds()
            value = lastUpdateLabel(tsSeconds, now)
            if (tsSeconds <= 0L) break
            val diff = now - tsSeconds
            val waitSec = when {
                diff < 60L    -> 60L - diff           // do progu "1 min temu"
                diff <= 3600L -> 60L - (diff % 60L)   // do następnej pełnej minuty
                else          -> break                // czas bezwzględny — koniec aktualizacji
            }
            delay(waitSec.coerceAtLeast(1L) * 1000L)
        }
    }
    return label
}

/**
 * Jak [liveLastUpdateLabel], ale zwraca BARE etykietę bez prefiksu „Ostatnia
 * aktualizacja:" — do statusu połączenia („Offline — dane z kopii: 24 min temu").
 * Ta sama logika progów/wybudzeń.
 */
@Composable
fun liveRelativeTimeLabel(tsSeconds: Long): String? {
    val label by produceState(initialValue = relativeTimeLabel(tsSeconds), tsSeconds) {
        while (true) {
            val now = nowEpochSeconds()
            value = relativeTimeLabel(tsSeconds, now)
            if (tsSeconds <= 0L) break
            val diff = now - tsSeconds
            val waitSec = when {
                diff < 60L    -> 60L - diff
                diff <= 3600L -> 60L - (diff % 60L)
                else          -> break
            }
            delay(waitSec.coerceAtLeast(1L) * 1000L)
        }
    }
    return label
}
