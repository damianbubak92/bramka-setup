package com.aitronic.smarthome.ui.auto

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.runtime.snapshots.SnapshotStateList
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.aitronic.smarthome.data.GatewayStore
import com.aitronic.smarthome.data.SmartHomeRepository
import com.aitronic.smarthome.data.net.GatewaySource
import com.aitronic.smarthome.data.net.RulesCodec
import com.aitronic.smarthome.domain.model.*
import com.aitronic.smarthome.ui.components.*
import com.aitronic.smarthome.ui.icons.ShIcons
import com.aitronic.smarthome.ui.theme.Sh
import com.aitronic.smarthome.ui.theme.deviceColor
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

@Composable private fun noRipple() = remember { MutableInteractionSource() }

private data class EditDraft(
    val id: Long?,
    val name: String,
    val conds: List<CondDraft>,
    val target: String,
    val value: Int,
)

private data class PickerReq(val title: String, val options: List<Pair<String, String>>, val selected: String, val onPick: (String) -> Unit)
private data class ConfirmReq(val title: String, val msg: String, val label: String, val onOk: () -> Unit)

/**
 * Cała funkcja Automatyzacje: lista + edytor + dialogi.
 * Reguły są **czytane z bramki** (`getrules`) i **wypychane po każdej zmianie** (`setrules`).
 *
 * ⚠️ Ograniczenie backendu: schemat bramki nie zna flagi „enabled" — wypychamy tylko
 * reguły włączone. Wyłączona reguła znika z bramki (zostaje lokalnie do czasu odświeżenia).
 */
@Composable
fun AutomationsRoot(repo: SmartHomeRepository, store: GatewayStore? = null) {
    val rules = remember { mutableStateListOf<Rule>() }
    var editing by remember { mutableStateOf<EditDraft?>(null) }
    var confirm by remember { mutableStateOf<ConfirmReq?>(null) }
    var picker by remember { mutableStateOf<PickerReq?>(null) }
    var toast by remember { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()
    val gwState = store?.state?.collectAsState()?.value
    val online = gwState?.online ?: false

    // Wczytaj reguły z bramki (raz). Bez bramki -> dane przykładowe.
    LaunchedEffect(store) {
        if (store == null) { rules.addAll(repo.rules()); return@LaunchedEffect }
        store.rulesJson()
            .onSuccess { rules.clear(); rules.addAll(RulesCodec.decode(it)) }
            .onFailure { toast = "Nie udało się pobrać reguł" }
    }

    // Wypchnięcie aktualnego zestawu na bramkę.
    fun push(okMsg: String) {
        val s = store ?: return
        scope.launch {
            s.saveRulesJson(RulesCodec.encode(rules.toList()))
                .onSuccess { toast = okMsg }
                .onFailure { toast = "Bramka odrzuciła zapis" }
        }
    }

    LaunchedEffect(toast) { if (toast != null) { delay(2600); toast = null } }

    Box(Modifier.fillMaxSize().background(Sh.bg)) {
        if (editing == null) {
            AutoList(
                rules = rules, online = online,
                onToggleOnline = { scope.launch { store?.refresh() } },
                onToggleRule = { id -> rules.replaceRule(id) { it.copy(enabled = !it.enabled) }; push("Zsynchronizowano z bramką") },
                onNew = { editing = EditDraft(null, "", listOf(CondDraft()), "solar", 1) },
                onEdit = { r -> editing = EditDraft(r.id, r.name, r.conditions.map { it.toDraft() }, r.action.target, r.action.value) },
                onDelete = { r -> confirm = ConfirmReq("Usunąć regułę?", "„${r.name}\" zostanie trwale usunięta.", "Usuń") { rules.remove(r); confirm = null; push("Usunięto regułę") } },
            )
        } else {
            AutoEditor(
                draft = editing!!,
                onChange = { editing = it },
                onCancel = { editing = null },
                onPick = { picker = it },
                onSave = {
                    val d = editing!!
                    val saved = Rule(d.id ?: ((rules.maxOfOrNull { it.id } ?: 0L) + 1L), d.name.ifBlank { "Bez nazwy" }, true, d.conds.map { it.toCondition() }, RuleAction(d.target, d.value))
                    if (d.id == null) rules.add(saved) else rules.replaceRule(d.id) { saved }
                    editing = null; push("Zapisano regułę")
                },
            )
        }

        // Toast
        toast?.let { t ->
            Box(Modifier.fillMaxSize().padding(bottom = 24.dp), contentAlignment = Alignment.BottomCenter) {
                Row(
                    Modifier.clip(RoundedCornerShape(22.dp)).background(Sh.graphite).padding(horizontal = 18.dp, vertical = 11.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Icon(ShIcons.Check, null, tint = Color(0xFF7CD8A6), modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(8.dp))
                    Text(t, color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.W500)
                }
            }
        }

        picker?.let { p ->
            SheetPicker(
                title = p.title,
                options = p.options.map { (id, label) -> PickerOption(label, id == p.selected, { p.onPick(id); picker = null }) },
                onDismiss = { picker = null },
            )
        }
        confirm?.let { c -> ConfirmDialog(c.title, c.msg, c.label, c.onOk) { confirm = null } }
    }
}

private inline fun SnapshotStateList<Rule>.replaceRule(id: Long, transform: (Rule) -> Rule) {
    val i = indexOfFirst { it.id == id }
    if (i >= 0) this[i] = transform(this[i])
}

// ---------------- LISTA ----------------

@Composable
private fun AutoList(
    rules: List<Rule>,
    online: Boolean,
    onToggleOnline: () -> Unit,
    onToggleRule: (Long) -> Unit,
    onNew: () -> Unit,
    onEdit: (Rule) -> Unit,
    onDelete: (Rule) -> Unit,
) {
    Box(Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.statusBars).verticalScroll(rememberScrollState())) {
            // Header
            Column(Modifier.padding(start = 24.dp, end = 24.dp, top = 12.dp, bottom = 6.dp)) {
                Text("Automatyzacje", color = Sh.textPrimary, fontSize = 28.sp, fontWeight = FontWeight.W500, letterSpacing = (-0.2).sp)
                Row(
                    Modifier.padding(top = 6.dp).clickable(noRipple(), null) { onToggleOnline() },
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(Modifier.size(8.dp).clip(CircleShape).background(if (online) Sh.online else Sh.dangerAlt))
                    Spacer(Modifier.width(7.dp))
                    val enabled = rules.count { it.enabled }
                    Text(
                        if (online) "Połączono na żywo · ${rules.size} reguł, $enabled aktywne" else "Brak połączenia z bramką · dotknij, aby wznowić",
                        color = Sh.textMuted, fontSize = 13.sp,
                    )
                }
            }

            if (rules.isEmpty()) {
                EmptyState(
                    iconBg = Color(0xFFFDF0D0), icon = ShIcons.Bolt, iconTint = Color(0xFFE1850B),
                    title = "Brak automatyzacji",
                    desc = "Twórz reguły, które automatycznie sterują urządzeniami — np. włącz pompę, gdy kolektor jest cieplejszy od bufora.",
                    action = "Nowa automatyzacja", onAction = onNew,
                )
            } else {
                Column(Modifier.padding(start = 20.dp, end = 20.dp, top = 12.dp, bottom = 110.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    rules.forEach { r -> RuleCard(r, { onToggleRule(r.id) }, { onEdit(r) }, { onDelete(r) }) }
                }
            }
        }

        // FAB
        Row(
            Modifier.align(Alignment.BottomEnd).padding(end = 20.dp, bottom = 24.dp)
                .clip(RoundedCornerShape(20.dp)).background(Sh.graphiteBtn)
                .clickable(noRipple(), null) { onNew() }.padding(horizontal = 20.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(ShIcons.Plus, null, tint = Color.White, modifier = Modifier.size(22.dp))
            Spacer(Modifier.width(10.dp))
            Text("Nowa", color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.W500)
        }
    }
}

@Composable
private fun RuleCard(r: Rule, onToggle: () -> Unit, onEdit: () -> Unit, onDelete: () -> Unit) {
    val on = r.enabled
    val col = deviceColor(r.action.target)
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(22.dp)).background(Sh.surface).padding(16.dp)
            .then(if (on) Modifier else Modifier.alpha(0.62f)),
    ) {
        Row {
            Box(
                Modifier.size(42.dp).clip(RoundedCornerShape(14.dp)).background(if (on) col.bg else Color(0xFFEDEAE3)),
                contentAlignment = Alignment.Center,
            ) { Icon(ShIcons.Bolt, null, tint = if (on) col.c else Sh.textMuted, modifier = Modifier.size(22.dp)) }
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(r.name, color = Sh.textPrimary, fontSize = 16.sp, fontWeight = FontWeight.W500, modifier = Modifier.weight(1f))
                    RuleToggle(on, onToggle)
                }
                // JEŚLI
                FlowPills(prefix = "JEŚLI") {
                    r.conditions.forEachIndexed { i, c ->
                        if (i > 0) AndText()
                        CondPill(condSummary(c), condDeviceKey(c), on)
                    }
                }
                Spacer(Modifier.height(8.dp))
                // TO
                FlowPills(prefix = "TO") {
                    val chipC = if (on) col.chipC else Sh.textMuted
                    val chipBg = if (on) col.chipBg else Color(0xFFEDEAE3)
                    Row(
                        Modifier.clip(RoundedCornerShape(20.dp)).background(chipBg).padding(horizontal = 11.dp, vertical = 5.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Icon(ShIcons.Bolt, null, tint = chipC, modifier = Modifier.size(13.dp))
                        Spacer(Modifier.width(6.dp))
                        Text(actionText(r.action), color = chipC, fontSize = 12.sp, fontWeight = FontWeight.W500)
                    }
                }
            }
        }
        Box(Modifier.fillMaxWidth().padding(top = 14.dp).height(1.dp).background(Sh.hairline))
        Row(Modifier.fillMaxWidth().padding(top = 8.dp), horizontalArrangement = Arrangement.End) {
            TextAction(ShIcons.Pencil, "Edytuj", Sh.textSecondary, onEdit)
            Spacer(Modifier.width(6.dp))
            TextAction(ShIcons.Trash, "Usuń", Sh.dangerAlt, onDelete)
        }
    }
}

@Composable
private fun TextAction(icon: androidx.compose.ui.graphics.vector.ImageVector, text: String, color: Color, onClick: () -> Unit) {
    Row(
        Modifier.clip(RoundedCornerShape(12.dp)).clickable(noRipple(), null) { onClick() }.padding(horizontal = 14.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(icon, null, tint = color, modifier = Modifier.size(16.dp))
        Spacer(Modifier.width(6.dp))
        Text(text, color = color, fontSize = 13.sp, fontWeight = FontWeight.W500)
    }
}

@Composable
private fun FlowPills(prefix: String, content: @Composable FlowRowScope.() -> Unit) {
    FlowRow(
        Modifier.fillMaxWidth().padding(top = 9.dp),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Text(prefix, color = Sh.textMuted, fontSize = 11.sp, fontWeight = FontWeight.W700, letterSpacing = 0.4.sp, modifier = Modifier.align(Alignment.CenterVertically))
        content()
    }
}

@Composable
private fun FlowRowScope.AndText() =
    Text("ORAZ", color = Sh.andMuted, fontSize = 11.sp, fontWeight = FontWeight.W700, modifier = Modifier.align(Alignment.CenterVertically))

@Composable
private fun CondPill(text: String, deviceKey: String?, enabled: Boolean) {
    val bg: Color; val fg: Color
    when {
        !enabled -> { bg = Color(0xFFEDEAE3); fg = Sh.textMuted }
        deviceKey == null -> { bg = Color(0xFFEDE8DE); fg = Sh.textPrimary }
        else -> { val c = deviceColor(deviceKey); bg = c.chipBg; fg = c.chipC }
    }
    Box(Modifier.clip(RoundedCornerShape(20.dp)).background(bg).padding(horizontal = 10.dp, vertical = 4.dp)) {
        Text(text, color = fg, fontSize = 12.sp, fontWeight = FontWeight.W500, maxLines = 1)
    }
}

@Composable
private fun RuleToggle(on: Boolean, onToggle: () -> Unit) {
    val left by animateDpAsState(if (on) 18.dp else 2.dp, tween(200), label = "k")
    val track by animateColorAsState(if (on) Sh.online else Color(0xFFD9D3C7), tween(200), label = "t")
    Box(
        Modifier.size(width = 40.dp, height = 24.dp).clip(RoundedCornerShape(12.dp)).background(track)
            .clickable(noRipple(), null) { onToggle() },
    ) {
        Box(Modifier.padding(start = left, top = 2.dp).size(20.dp).clip(CircleShape).background(Color.White))
    }
}

@Composable
private fun EmptyState(iconBg: Color, icon: androidx.compose.ui.graphics.vector.ImageVector, iconTint: Color, title: String, desc: String, action: String?, onAction: () -> Unit) {
    Column(
        Modifier.fillMaxWidth().padding(start = 30.dp, end = 30.dp, top = 48.dp, bottom = 30.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Box(Modifier.size(76.dp).clip(RoundedCornerShape(24.dp)).background(iconBg), contentAlignment = Alignment.Center) {
            Icon(icon, null, tint = iconTint, modifier = Modifier.size(38.dp))
        }
        Spacer(Modifier.height(18.dp))
        Text(title, color = Sh.textPrimary, fontSize = 19.sp, fontWeight = FontWeight.W500)
        Text(desc, color = Sh.textMuted, fontSize = 14.sp, lineHeight = 21.sp, modifier = Modifier.padding(top = 8.dp), textAlign = androidx.compose.ui.text.style.TextAlign.Center)
        if (action != null) {
            Spacer(Modifier.height(22.dp))
            Row(
                Modifier.clip(RoundedCornerShape(18.dp)).background(Sh.graphiteBtn).clickable(noRipple(), null) { onAction() }.padding(horizontal = 22.dp, vertical = 13.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(ShIcons.Plus, null, tint = Color.White, modifier = Modifier.size(20.dp))
                Spacer(Modifier.width(9.dp))
                Text(action, color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.W500)
            }
        }
    }
}

// ---------------- EDYTOR ----------------

@Composable
private fun AutoEditor(
    draft: EditDraft,
    onChange: (EditDraft) -> Unit,
    onCancel: () -> Unit,
    onPick: (PickerReq) -> Unit,
    onSave: () -> Unit,
) {
    val errors = draft.conds.map { it.error() }
    val valid = errors.all { it == null }

    Column(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing)) {
        // Top bar
        Row(Modifier.fillMaxWidth().padding(start = 2.dp, end = 14.dp, top = 2.dp, bottom = 10.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(44.dp).clip(CircleShape).clickable(noRipple(), null) { onCancel() }, contentAlignment = Alignment.Center) {
                Icon(ShIcons.ChevronLeft, "Wstecz", tint = Sh.textPrimary, modifier = Modifier.size(24.dp))
            }
            Text(if (draft.id == null) "Nowa automatyzacja" else "Edytuj automatyzację", color = Sh.textPrimary, fontSize = 20.sp, fontWeight = FontWeight.W500, modifier = Modifier.padding(start = 6.dp))
        }

        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(start = 20.dp, end = 20.dp, top = 4.dp, bottom = 20.dp)) {
            InputField("Nazwa", draft.name, { onChange(draft.copy(name = it)) }, placeholder = "np. Pompa ON")

            Row(Modifier.fillMaxWidth().padding(top = 22.dp, bottom = 10.dp, start = 2.dp, end = 2.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("Warunki", color = Sh.textPrimary, fontSize = 16.sp, fontWeight = FontWeight.W500, modifier = Modifier.weight(1f))
                Text("wszystkie muszą być spełnione", color = Sh.textMuted, fontSize = 12.sp)
            }

            draft.conds.forEachIndexed { i, c ->
                CondCard(
                    index = i, cond = c, error = errors[i], canRemove = draft.conds.size > 1,
                    onChange = { nc -> onChange(draft.copy(conds = draft.conds.toMutableList().also { it[i] = nc })) },
                    onRemove = { onChange(draft.copy(conds = draft.conds.toMutableList().also { it.removeAt(i) })) },
                    onPick = onPick,
                )
                Spacer(Modifier.height(12.dp))
            }

            // dodaj warunek
            Row(
                Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp))
                    .border(1.5.dp, Sh.dashed, RoundedCornerShape(16.dp))
                    .clickable(noRipple(), null) { onChange(draft.copy(conds = draft.conds + CondDraft())) }
                    .padding(13.dp),
                horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(ShIcons.Plus, null, tint = Sh.textPrimary, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(8.dp))
                Text("Dodaj warunek", color = Sh.textPrimary, fontSize = 14.sp, fontWeight = FontWeight.W500)
            }

            // Akcja
            Text("Akcja", color = Sh.textPrimary, fontSize = 16.sp, fontWeight = FontWeight.W500, modifier = Modifier.padding(top = 24.dp, bottom = 10.dp, start = 2.dp))
            Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(20.dp)).background(Sh.surface).padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                PickerRow("Urządzenie docelowe", deviceName(draft.target)) {
                    onPick(PickerReq("Wybierz urządzenie", AutoTargets, draft.target) { onChange(draft.copy(target = it)) })
                }
                Box(Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Sh.bg).border(1.dp, Sh.fieldBorder, RoundedCornerShape(14.dp)).padding(horizontal = 14.dp, vertical = 9.dp)) {
                    Column { Text("Typ akcji", color = Sh.textMuted, fontSize = 11.sp); Text("Ustaw przekaźnik", color = Sh.textPrimary, fontSize = 15.sp, modifier = Modifier.padding(top = 1.dp)) }
                }
                Column {
                    Text("Stan przekaźnika", color = Sh.textMuted, fontSize = 11.sp, modifier = Modifier.padding(start = 2.dp, bottom = 6.dp))
                    Segmented(listOf(1 to "Włącz (ON)", 0 to "Wyłącz (OFF)"), draft.value, { onChange(draft.copy(value = it)) })
                }
            }

            // Podgląd reguły
            RulePreview(draft)
        }

        // Footer
        Row(Modifier.fillMaxWidth().background(Sh.bg).padding(horizontal = 20.dp, vertical = 12.dp), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            OutlineButton("Anuluj", Modifier.weight(1f), onCancel)
            Box(
                Modifier.weight(1.4f).clip(RoundedCornerShape(16.dp))
                    .background(if (valid) Sh.graphiteBtn else Color(0xFFD9D3C7))
                    .clickable(noRipple(), null, enabled = valid) { onSave() }.padding(vertical = 14.dp),
                contentAlignment = Alignment.Center,
            ) { Text("Zapisz", color = if (valid) Color.White else Sh.textMuted, fontSize = 15.sp, fontWeight = FontWeight.W500) }
        }
    }
}

@Composable
private fun CondCard(
    index: Int, cond: CondDraft, error: String?, canRemove: Boolean,
    onChange: (CondDraft) -> Unit, onRemove: () -> Unit, onPick: (PickerReq) -> Unit,
) {
    Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(18.dp)).background(Sh.surface).padding(14.dp)) {
        Row(Modifier.fillMaxWidth().padding(bottom = 12.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("WARUNEK ${index + 1}", color = Sh.textMuted, fontSize = 13.sp, fontWeight = FontWeight.W500, letterSpacing = 0.4.sp, modifier = Modifier.weight(1f))
            if (canRemove) Icon(ShIcons.Trash, "Usuń", tint = Sh.dangerAlt, modifier = Modifier.size(16.dp).clickable(noRipple(), null) { onRemove() })
        }
        Segmented(
            listOf(CondType.Time to "Czas", CondType.Param to "Parametr", CondType.Delta to "Delta"),
            cond.type, { t -> onChange(defaultForType(t)) },
        )
        Spacer(Modifier.height(12.dp))
        when (cond.type) {
            CondType.Time -> Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                InputField("Od", cond.start, { onChange(cond.copy(start = it)) }, Modifier.weight(1f))
                InputField("Do", cond.end, { onChange(cond.copy(end = it)) }, Modifier.weight(1f))
            }
            CondType.Param -> Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                PickerRow("Urządzenie", deviceName(cond.device)) { onPick(PickerReq("Wybierz urządzenie", AutoDevices, cond.device) { d -> onChange(cond.copy(device = d, param = AutoParams[d]?.first() ?: cond.param)) }) }
                PickerRow("Parametr", cond.param) { onPick(PickerReq("Wybierz parametr", paramOptions(cond.device), cond.param) { p -> onChange(cond.copy(param = p)) }) }
                Segmented(listOf(CompareOp.Gt to "Większe niż", CompareOp.Lt to "Mniejsze niż"), cond.op, { onChange(cond.copy(op = it)) })
                InputField("Wartość progowa", cond.value, { onChange(cond.copy(value = it)) }, number = true)
            }
            CondType.Delta -> Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    PickerRow("Urządzenie 1", deviceName(cond.device1), Modifier.weight(1f)) { onPick(PickerReq("Wybierz urządzenie", AutoDevices, cond.device1) { d -> onChange(cond.copy(device1 = d, param1 = AutoParams[d]?.first() ?: cond.param1)) }) }
                    PickerRow("Parametr 1", cond.param1, Modifier.weight(1f)) { onPick(PickerReq("Wybierz parametr", paramOptions(cond.device1), cond.param1) { p -> onChange(cond.copy(param1 = p)) }) }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    PickerRow("Urządzenie 2", deviceName(cond.device2), Modifier.weight(1f)) { onPick(PickerReq("Wybierz urządzenie", AutoDevices, cond.device2) { d -> onChange(cond.copy(device2 = d, param2 = AutoParams[d]?.first() ?: cond.param2)) }) }
                    PickerRow("Parametr 2", cond.param2, Modifier.weight(1f)) { onPick(PickerReq("Wybierz parametr", paramOptions(cond.device2), cond.param2) { p -> onChange(cond.copy(param2 = p)) }) }
                }
                Segmented(listOf(CompareOp.Gt to "Większe niż", CompareOp.Lt to "Mniejsze niż"), cond.op, { onChange(cond.copy(op = it)) })
                InputField("Minimalna różnica", cond.min, { onChange(cond.copy(min = it)) }, number = true)
            }
        }
        if (error != null) {
            Row(Modifier.padding(top = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                Icon(ShIcons.AlertCircle, null, tint = Sh.dangerAlt, modifier = Modifier.size(15.dp))
                Spacer(Modifier.width(7.dp))
                Text(error, color = Sh.dangerAlt, fontSize = 12.sp)
            }
        }
    }
}

@Composable
private fun RulePreview(draft: EditDraft) {
    val col = deviceColor(draft.target)
    Column(
        Modifier.fillMaxWidth().padding(top = 22.dp).clip(RoundedCornerShape(20.dp))
            .background(Sh.surface).border(1.dp, Sh.divider, RoundedCornerShape(20.dp)).padding(horizontal = 18.dp, vertical = 16.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(ShIcons.Bolt, null, tint = Color(0xFFE1850B), modifier = Modifier.size(16.dp))
            Spacer(Modifier.width(8.dp))
            Text("PODGLĄD REGUŁY", color = Sh.textMuted, fontSize = 11.sp, fontWeight = FontWeight.W600, letterSpacing = 0.6.sp)
        }
        Spacer(Modifier.height(14.dp))
        Text("JEŚLI", color = Color(0xFFE1850B), fontSize = 12.sp, fontWeight = FontWeight.W700, letterSpacing = 0.4.sp)
        FlowRow(Modifier.fillMaxWidth().padding(top = 6.dp, bottom = 16.dp), horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            draft.conds.forEachIndexed { i, c ->
                if (i > 0) AndText()
                CondPill(c.summary(), c.deviceKey(), true)
            }
        }
        Text("TO", color = Sh.online, fontSize = 12.sp, fontWeight = FontWeight.W700, letterSpacing = 0.4.sp)
        Row(
            Modifier.padding(top = 8.dp).clip(RoundedCornerShape(20.dp)).background(col.chipBg).padding(horizontal = 11.dp, vertical = 5.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(ShIcons.Bolt, null, tint = col.chipC, modifier = Modifier.size(13.dp))
            Spacer(Modifier.width(6.dp))
            Text("${deviceName(draft.target)} · Przekaźnik ${if (draft.value == 1) "ON" else "OFF"}", color = col.chipC, fontSize = 12.sp, fontWeight = FontWeight.W500)
        }
    }
}

private fun defaultForType(t: CondType): CondDraft = when (t) {
    CondType.Time -> CondDraft(CondType.Time, start = "06:00", end = "22:00")
    CondType.Param -> CondDraft(CondType.Param, device = "solar", param = "T3", op = CompareOp.Gt, value = "60")
    CondType.Delta -> CondDraft(CondType.Delta, device1 = "solar", param1 = "T3", device2 = "buffer", param2 = "sBuforTemp", op = CompareOp.Gt, min = "8")
}

private fun paramOptions(device: String): List<Pair<String, String>> =
    (AutoParams[device] ?: emptyList()).map { it to it }
