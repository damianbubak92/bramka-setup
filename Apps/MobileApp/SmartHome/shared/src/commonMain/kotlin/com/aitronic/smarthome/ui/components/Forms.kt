package com.aitronic.smarthome.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.aitronic.smarthome.ui.icons.ShIcons
import com.aitronic.smarthome.ui.theme.Sh

// wspólny helper: własne MutableInteractionSource → clickable bez ripple
@Composable
private fun noRipple() = remember { MutableInteractionSource() }

/** Segment (segmented control) na torze segTrack; aktywny = biała uniesiona pastylka. */
@Composable
fun <T> Segmented(
    options: List<Pair<T, String>>,
    selected: T,
    onSelect: (T) -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier.clip(RoundedCornerShape(14.dp)).background(Sh.segTrack).padding(4.dp),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        options.forEach { (value, label) ->
            val active = value == selected
            Box(
                Modifier.weight(1f).clip(RoundedCornerShape(11.dp))
                    .background(if (active) Sh.surface else Color.Transparent)
                    .clickable(noRipple(), null) { onSelect(value) }
                    .padding(vertical = 9.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    label,
                    color = if (active) Sh.textPrimary else Sh.textSecondary,
                    fontSize = 13.sp,
                    fontWeight = FontWeight.W500,
                    maxLines = 1,
                )
            }
        }
    }
}

/** Wiersz wyboru (otwiera bottom-sheet): etykieta + wartość + chevron. */
@Composable
fun PickerRow(label: String, value: String, modifier: Modifier = Modifier, locked: Boolean = false, onClick: () -> Unit = {}) {
    Row(
        modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp))
            .background(if (locked) Sh.fieldBg else Sh.surface)
            .border(1.dp, if (locked) Sh.divider else Sh.fieldBorder, RoundedCornerShape(14.dp))
            .then(if (locked) Modifier else Modifier.clickable(noRipple(), null) { onClick() })
            .padding(horizontal = 14.dp, vertical = 11.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(label, color = Sh.textMuted, fontSize = 11.sp)
            Text(value, color = Sh.textPrimary, fontSize = 15.sp, modifier = Modifier.padding(top = 1.dp))
        }
        Icon(if (locked) ShIcons.Lock else ShIcons.ChevronDown, null, tint = if (locked) Sh.textMuted else Sh.andMuted, modifier = Modifier.size(if (locked) 16.dp else 18.dp))
    }
}

/** Pole tekstowe/liczbowe z etykietą nad. */
@Composable
fun InputField(
    label: String,
    value: String,
    onValueChange: (String) -> Unit,
    modifier: Modifier = Modifier,
    placeholder: String = "",
    number: Boolean = false,
) {
    Column(modifier) {
        Text(label, color = Sh.textMuted, fontSize = 11.sp, modifier = Modifier.padding(start = 2.dp, bottom = 5.dp))
        Box(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Sh.surface)
                .border(1.dp, Sh.fieldBorder, RoundedCornerShape(14.dp))
                .padding(horizontal = 14.dp, vertical = 13.dp),
        ) {
            BasicTextField(
                value = value,
                onValueChange = onValueChange,
                textStyle = TextStyle(color = Sh.textPrimary, fontSize = 16.sp),
                singleLine = true,
                cursorBrush = SolidColor(Sh.graphite),
                keyboardOptions = KeyboardOptions(keyboardType = if (number) KeyboardType.Number else KeyboardType.Text),
                modifier = Modifier.fillMaxWidth(),
            )
            if (value.isEmpty() && placeholder.isNotEmpty()) {
                Text(placeholder, color = Sh.textMuted, fontSize = 16.sp)
            }
        }
    }
}

/** Wspólny dialog potwierdzenia usunięcia. */
@Composable
fun ConfirmDialog(title: String, message: String, confirmLabel: String, onConfirm: () -> Unit, onDismiss: () -> Unit) {
    Box(
        Modifier.fillMaxSize().background(Color(0xFF201B13).copy(alpha = 0.45f))
            .clickable(noRipple(), null) { onDismiss() }
            .padding(28.dp),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier.widthIn(max = 320.dp).clip(RoundedCornerShape(24.dp)).background(Sh.surface)
                .clickable(noRipple(), null) {}  // pochłania kliknięcie
                .padding(start = 22.dp, end = 22.dp, top = 24.dp, bottom = 18.dp),
        ) {
            Box(
                Modifier.size(52.dp).clip(RoundedCornerShape(16.dp)).background(Color(0xFFF7DDD9)),
                contentAlignment = Alignment.Center,
            ) { Icon(ShIcons.Trash, null, tint = Sh.danger, modifier = Modifier.size(26.dp)) }
            Spacer(Modifier.height(16.dp))
            Text(title, color = Sh.textPrimary, fontSize = 19.sp, fontWeight = FontWeight.W500)
            Text(message, color = Sh.textSecondary, fontSize = 14.sp, lineHeight = 21.sp, modifier = Modifier.padding(top = 8.dp))
            Spacer(Modifier.height(22.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                OutlineButton("Anuluj", Modifier.weight(1f), onDismiss)
                Box(
                    Modifier.weight(1f).clip(RoundedCornerShape(15.dp)).background(Sh.danger)
                        .clickable(noRipple(), null) { onConfirm() }.padding(vertical = 13.dp),
                    contentAlignment = Alignment.Center,
                ) { Text(confirmLabel, color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.W500) }
            }
        }
    }
}

/** Prosty dialog informacyjny (błąd/komunikat) z jednym przyciskiem OK. */
@Composable
fun NoticeDialog(title: String, message: String, onDismiss: () -> Unit) {
    Box(
        Modifier.fillMaxSize().background(Color(0xFF201B13).copy(alpha = 0.45f))
            .clickable(noRipple(), null) { onDismiss() }
            .padding(28.dp),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier.widthIn(max = 320.dp).clip(RoundedCornerShape(24.dp)).background(Sh.surface)
                .clickable(noRipple(), null) {}
                .padding(start = 22.dp, end = 22.dp, top = 22.dp, bottom = 16.dp),
        ) {
            Text(title, color = Sh.textPrimary, fontSize = 18.sp, fontWeight = FontWeight.W500)
            Text(message, color = Sh.textSecondary, fontSize = 14.sp, lineHeight = 21.sp, modifier = Modifier.padding(top = 8.dp))
            Spacer(Modifier.height(20.dp))
            Box(
                Modifier.align(Alignment.End).clip(RoundedCornerShape(15.dp)).background(Sh.graphiteBtn)
                    .clickable(noRipple(), null) { onDismiss() }.padding(horizontal = 26.dp, vertical = 11.dp),
            ) { Text("OK", color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.W500) }
        }
    }
}

@Composable
fun OutlineButton(text: String, modifier: Modifier = Modifier, onClick: () -> Unit) {
    Box(
        modifier.clip(RoundedCornerShape(15.dp)).border(1.5.dp, Color(0xFFD9D3C7), RoundedCornerShape(15.dp))
            .clickable(noRipple(), null) { onClick() }.padding(vertical = 13.dp),
        contentAlignment = Alignment.Center,
    ) { Text(text, color = Sh.textSecondary, fontSize = 15.sp, fontWeight = FontWeight.W500) }
}

data class PickerOption(val label: String, val selected: Boolean, val onPick: () -> Unit, val removable: Boolean = false, val onRemove: () -> Unit = {})

/** Bottom-sheet z listą opcji + opcjonalny footer (np. zarządzanie pokojami). */
@Composable
fun SheetPicker(
    title: String,
    options: List<PickerOption>,
    onDismiss: () -> Unit,
    footer: (@Composable () -> Unit)? = null,
) {
    Box(
        Modifier.fillMaxSize().background(Color(0xFF201B13).copy(alpha = 0.4f))
            .clickable(noRipple(), null) { onDismiss() },
        contentAlignment = Alignment.BottomCenter,
    ) {
        Column(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(topStart = 26.dp, topEnd = 26.dp)).background(Sh.surface)
                .clickable(noRipple(), null) {}
                .padding(horizontal = 12.dp, vertical = 10.dp),
        ) {
            Box(Modifier.fillMaxWidth().padding(vertical = 6.dp), contentAlignment = Alignment.Center) {
                Box(Modifier.size(width = 40.dp, height = 5.dp).clip(RoundedCornerShape(3.dp)).background(Color(0xFFE0DACE)))
            }
            Text(title, color = Sh.textPrimary, fontSize = 15.sp, fontWeight = FontWeight.W500, modifier = Modifier.padding(start = 12.dp, top = 4.dp, bottom = 10.dp))
            options.forEach { o ->
                Row(
                    Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp))
                        .clickable(noRipple(), null) { o.onPick() }.padding(horizontal = 12.dp, vertical = 14.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(o.label, color = Sh.textPrimary, fontSize = 16.sp, modifier = Modifier.weight(1f))
                    if (o.selected) Icon(ShIcons.Check, null, tint = Sh.textPrimary, modifier = Modifier.size(20.dp))
                    if (o.removable) {
                        Spacer(Modifier.width(10.dp))
                        Icon(ShIcons.Trash, "Usuń", tint = Sh.danger, modifier = Modifier.size(18.dp).clickable(noRipple(), null) { o.onRemove() })
                    }
                }
            }
            footer?.invoke()
        }
    }
}
