package com.aitronic.smarthome.ui.devices

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.aitronic.smarthome.data.GatewayStore
import com.aitronic.smarthome.data.SmartHomeRepository
import com.aitronic.smarthome.data.net.NodeTypes
import com.aitronic.smarthome.domain.model.Device
import com.aitronic.smarthome.ui.components.*
import com.aitronic.smarthome.ui.icons.ShIcons
import com.aitronic.smarthome.ui.theme.Sh
import com.aitronic.smarthome.ui.theme.deviceColor
import kotlinx.coroutines.launch

@Composable private fun noRipple() = remember { MutableInteractionSource() }

private const val NO_ROOM = "Bez pokoju"

/** Node uznajemy za online, jeśli odezwał się w tym oknie (telemetria leci co ~2 min). */
private const val ONLINE_WINDOW_S = 360L

private fun deviceIcon(type: String): ImageVector = when (type) {
    "solar" -> ShIcons.Sun
    "buffer" -> ShIcons.Droplet
    "pv" -> ShIcons.Bolt
    "climate" -> ShIcons.ThermoDrop
    "light" -> ShIcons.Bulb
    "blind" -> ShIcons.Blinds
    "heating" -> ShIcons.Flame
    else -> ShIcons.Monitor
}

private data class DevDraft(val id: Long?, val name: String, val type: String, val room: String, val joining: Boolean)
private data class ConfirmReq(val title: String, val msg: String, val onOk: () -> Unit)
/** Źródło celu wymiany → decyduje o komendzie: ACTIVE=replacenode (po adresie),
 * DETACHED=repairnode (po node_id), TRASH=restorenode+repairnode. */
private enum class TargetKind { ACTIVE, DETACHED, TRASH }
/** Kandydat do "Wymień istniejące" — node tego samego typu, nieusunięty trwale. */
private data class ReplaceTarget(val nodeId: Long, val address: Int, val name: String, val kind: TargetKind)

/**
 * Urządzenia = **nody zarejestrowane w bramce** (`command=listnodes` → tabela `node`).
 * Nazwa i pokój są trwałe w bazie (`command=updatenode`); online liczymy z `last_seen`
 * (node żyje, jeśli odezwał się w ciągu [ONLINE_WINDOW_S] — telemetria leci co ~2 min).
 */
@Composable
fun DevicesRoot(
    repo: SmartHomeRepository,
    store: GatewayStore? = null,
    autoOpenJoin: Boolean = false,
    onJoinConsumed: () -> Unit = {},
) {
    val scope = rememberCoroutineScope()
    val gw = store?.state?.collectAsState()?.value
    val typeNames = remember { repo.deviceTypes().associate { it.id to it.name } }

    val devices: List<Device> = if (gw == null) remember { repo.devices() } else {
        val nowS = gw.telemetry.values.maxOfOrNull { it.ts } ?: 0L
        gw.nodes.map { n ->
            Device(
                id = n.address.toLong(),
                name = n.name.ifBlank { NodeTypes.label(n.type) },
                type = NodeTypes.toUiType(n.type),
                room = n.room.ifBlank { NO_ROOM },
                online = n.lastSeen > 0 && (nowS - n.lastSeen) <= ONLINE_WINDOW_S,
            )
        }
    }

    // Lista pokoi: katalog bazowy + te faktycznie używane przez nody (z bazy).
    val rooms = remember { mutableStateListOf<String>().apply { addAll(repo.rooms()) } }
    val allRooms = remember(devices, rooms.toList()) {
        (rooms + devices.map { it.room }).distinct().sortedBy { it == NO_ROOM } // "Bez pokoju" na końcu
    }

    var filter by remember { mutableStateOf("Wszystkie") }
    var editing by remember { mutableStateOf<DevDraft?>(null) }
    var confirm by remember { mutableStateOf<ConfirmReq?>(null) }
    var roomPicker by remember { mutableStateOf(false) }
    var newRoom by remember { mutableStateOf("") }
    var showJoin by remember { mutableStateOf(false) }
    var showTrash by remember { mutableStateOf(false) }
    var replacePick by remember { mutableStateOf(false) }
    var notice by remember { mutableStateOf<String?>(null) }
    // Błąd z bramki niesie tekst w treści; obcinamy prefiks "HTTP 400: " dla czytelności.
    fun fail(t: Throwable) { notice = t.message?.substringAfter(": ")?.ifBlank { null } ?: "Operacja nie powiodła się" }

    val joinCtx = gw?.joins?.firstOrNull()
    val nowS = gw?.telemetry?.values?.maxOfOrNull { it.ts } ?: 0L

    // Cele "Wymień istniejące": kompatybilne (ten sam typ) i nieusunięte trwale —
    // aktywne (replace) + detached (repair) + w koszu (restore+repair).
    val replaceTargets = remember(gw?.nodes, gw?.trash, joinCtx?.type) {
        val t = joinCtx?.type ?: -1
        val fromNodes = gw?.nodes.orEmpty()
            .filter { it.type == t && (it.status == "active" && it.address > 0 || it.status == "detached") }
            .map { ReplaceTarget(it.id, it.address, it.name.ifBlank { NodeTypes.label(it.type) },
                if (it.status == "detached") TargetKind.DETACHED else TargetKind.ACTIVE) }
        val fromTrash = gw?.trash.orEmpty()
            .filter { it.type == t }
            .map { ReplaceTarget(it.id, 0, it.name.ifBlank { NodeTypes.label(it.type) }, TargetKind.TRASH) }
        fromNodes + fromTrash
    }

    // Klasyfikacja JOINa po factory_id: jeśli chip pasuje do noda detached albo w koszu
    // (jego WŁASNY chip wraca) → tryb "Przywróć". Inaczej → "Dodaj nowe / Wymień".
    // (Aktywne bramka wycisza — do apki taki JOIN nie dochodzi.)
    val detachedMatch = joinCtx?.let { j -> gw?.nodes?.firstOrNull { it.status == "detached" && it.factory.equals(j.factory, true) } }
    val trashMatch = joinCtx?.let { j -> gw?.trash?.firstOrNull { it.factory.equals(j.factory, true) } }
    val typeLabel = joinCtx?.let { NodeTypes.label(it.type) } ?: "Urządzenie"
    val restoreName = detachedMatch?.name?.ifBlank { typeLabel } ?: trashMatch?.name?.ifBlank { typeLabel }
    val isRestore = detachedMatch != null || trashMatch != null

    // Auto-propozycja nazwy dla "Dodaj nowe": etykieta typu, numerowana gdy zajęta.
    val nameSuggestion = remember(gw?.nodes, joinCtx?.type) {
        val base = joinCtx?.let { NodeTypes.label(it.type) } ?: "Urządzenie"
        val taken = gw?.nodes.orEmpty().map { it.name.trim() }
        if (taken.none { it.equals(base, true) }) base
        else generateSequence(2) { it + 1 }.map { "$base $it" }.first { s -> taken.none { it.equals(s, true) } }
    }

    // Auto-popup po nowym JOINie (sygnał z AppScaffold): wyjdź z edytora/kosza na czystą
    // listę i otwórz dialog. onJoinConsumed kasuje flagę u rodzica.
    LaunchedEffect(autoOpenJoin) {
        if (autoOpenJoin) {
            editing = null
            showTrash = false
            showJoin = true
            onJoinConsumed()
        }
    }
    // Kosz potrzebny do klasyfikacji (trashMatch) i do celów wymiany — dociągnij gdy popup
    // pokazuje JOIN (tanie, raz na otwarcie/zmianę joina).
    LaunchedEffect(showJoin, joinCtx?.factory) {
        if (showJoin && joinCtx != null) store?.loadTrash()
    }

    Box(Modifier.fillMaxSize().background(Sh.bg)) {
        if (showTrash) {
            TrashScreen(
                items = gw?.trash ?: emptyList(), nowS = nowS,
                onBack = { showTrash = false },
                onRestore = { t -> scope.launch { store?.restoreNode(t.id) } },
            )
        } else if (editing == null) {
            DevList(
                devices = devices, rooms = allRooms, typeNames = typeNames, filter = filter,
                onFilter = { filter = it },
                pending = gw?.joins?.size ?: 0,
                onJoin = {
                    // Prawdziwy JOIN: node zgłasza się sam (bramka wypycha join_pending po WS).
                    scope.launch { store?.refresh() }
                    if (gw?.joins?.isNotEmpty() == true) showJoin = true
                },
                onTrash = { scope.launch { store?.loadTrash() }; showTrash = true },
                onOpen = { d -> editing = DevDraft(d.id, d.name, d.type, d.room, joining = false) },
            )
        } else {
            DevEditor(
                draft = editing!!, typeName = typeNames[editing!!.type] ?: editing!!.type,
                onChange = { editing = it },
                onCancel = { editing = null },
                onOpenRoom = { roomPicker = true },
                onDelete = {
                    val d = editing!!
                    confirm = ConfirmReq("Usunąć urządzenie?", "„${d.name.ifBlank { "urządzenie" }}\" trafi do kosza (przywracalne przez 60 dni). Powiązane automatyzacje mogą przestać działać.") {
                        d.id?.let { id -> scope.launch { store?.removeNode(id.toInt()) } }
                        confirm = null; editing = null
                    }
                },
                onSave = {
                    val d = editing!!
                    d.id?.let { id ->
                        val room = if (d.room == NO_ROOM) "" else d.room
                        scope.launch { store?.updateNode(id.toInt(), d.name.ifBlank { typeNames[d.type] ?: "Urządzenie" }, room) }
                    }
                    editing = null
                },
            )
        }

        if (roomPicker) {
            val cur = editing?.room ?: NO_ROOM
            SheetPicker(
                title = "Pokój",
                options = allRooms.map { r ->
                    PickerOption(r, r == cur, { editing = editing?.copy(room = r); roomPicker = false }, removable = r != NO_ROOM, onRemove = {
                        // przypisania żyją w bazie -> przenieś tam nody z tego pokoju
                        scope.launch {
                            devices.filter { it.room == r }.forEach { d ->
                                d.id?.let { store?.updateNode(it.toInt(), d.name, "") }
                            }
                        }
                        if (editing?.room == r) editing = editing?.copy(room = NO_ROOM)
                        rooms.remove(r)
                    })
                },
                onDismiss = { roomPicker = false },
                footer = {
                    Row(Modifier.fillMaxWidth().padding(top = 12.dp).border(0.dp, Color.Transparent).padding(horizontal = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                        Box(Modifier.weight(1f).clip(RoundedCornerShape(12.dp)).background(Sh.surface).border(1.dp, Sh.fieldBorder, RoundedCornerShape(12.dp)).padding(horizontal = 12.dp, vertical = 11.dp)) {
                            androidx.compose.foundation.text.BasicTextField(
                                newRoom, { newRoom = it },
                                textStyle = androidx.compose.ui.text.TextStyle(color = Sh.textPrimary, fontSize = 15.sp),
                                singleLine = true, cursorBrush = androidx.compose.ui.graphics.SolidColor(Sh.graphite),
                                modifier = Modifier.fillMaxWidth(),
                            )
                            if (newRoom.isEmpty()) Text("Nazwa nowego pokoju", color = Sh.textMuted, fontSize = 15.sp)
                        }
                        Spacer(Modifier.width(8.dp))
                        Box(
                            Modifier.clip(RoundedCornerShape(12.dp)).background(Sh.graphiteBtn)
                                .clickable(noRipple(), null) {
                                    val n = newRoom.trim()
                                    if (n.isNotEmpty() && n !in rooms) { rooms.add(rooms.indexOf(NO_ROOM).coerceAtLeast(0), n); newRoom = "" }
                                }.padding(horizontal = 18.dp, vertical = 11.dp),
                        ) { Text("Dodaj", color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.W500) }
                    }
                },
            )
        }
        confirm?.let { c -> ConfirmDialog(c.title, c.msg, "Usuń", c.onOk) { confirm = null } }
        notice?.let { m -> NoticeDialog("Nie udało się", m) { notice = null } }

        // Popup JOIN sterowany KOLEJKĄ: joinCtx = pierwszy oczekujący. Po obsłużeniu jednego
        // (approve/repair/restore/reject) NIE zamykamy — bramka usuwa go z joins, joinCtx
        // przechodzi do następnego; pusta kolejka (joinCtx == null) → dialog znika sam.
        // key(factory) resetuje stan dialogu przy każdej zmianie noda.
        val jc = joinCtx
        if (showJoin && jc != null) {
            key(jc.factory) {
                JoinDialog(
                    factory = jc.factory, type = jc.type,
                    isRestore = isRestore, restoreName = restoreName ?: "",
                    nameSuggestion = nameSuggestion, hasReplaceTargets = replaceTargets.isNotEmpty(),
                    onRestore = {
                        scope.launch {
                            val res = when {
                                detachedMatch != null -> store?.repairNode(jc.factory, detachedMatch.id)
                                trashMatch != null -> store?.restoreAndRepair(jc.factory, trashMatch.id)
                                else -> null
                            }
                            res?.onFailure(::fail)
                        }
                    },
                    onCreate = { name -> scope.launch { store?.approveJoin(jc.factory, name)?.onFailure(::fail) } },
                    onReplace = { replacePick = true },
                    onReject = { scope.launch { store?.rejectJoin(jc.factory) } },
                    onDismiss = { showJoin = false },
                )
            }
        }
        // "Wymień istniejące": aktywny → replace, detached → repair, kosz → restore+repair.
        if (replacePick && jc != null) {
            SheetPicker(
                title = "Wymień / odzyskaj urządzenie",
                options = replaceTargets.map { t ->
                    val suffix = when (t.kind) {
                        TargetKind.ACTIVE -> ""
                        TargetKind.DETACHED -> " · odłączony"
                        TargetKind.TRASH -> " · z kosza (odzyska historię)"
                    }
                    PickerOption(t.name + suffix, selected = false, onPick = {
                        scope.launch {
                            val res = when (t.kind) {
                                TargetKind.ACTIVE -> store?.replaceNode(jc.factory, t.address)
                                TargetKind.DETACHED -> store?.repairNode(jc.factory, t.nodeId)
                                TargetKind.TRASH -> store?.restoreAndRepair(jc.factory, t.nodeId)
                            }
                            res?.onFailure(::fail)
                        }
                        replacePick = false   // showJoin zostaje → kolejka jedzie dalej
                    })
                },
                onDismiss = { replacePick = false },
            )
        }
    }
}

// ---------------- LISTA ----------------

@Composable
private fun DevList(
    devices: List<Device>, rooms: List<String>, typeNames: Map<String, String>, filter: String,
    pending: Int,
    onFilter: (String) -> Unit, onJoin: () -> Unit, onTrash: () -> Unit, onOpen: (Device) -> Unit,
) {
    Column(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.statusBars).verticalScroll(rememberScrollState())) {
        Column(Modifier.padding(start = 24.dp, end = 24.dp, top = 12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Urządzenia", color = Sh.textPrimary, fontSize = 28.sp, fontWeight = FontWeight.W500, letterSpacing = (-0.2).sp, modifier = Modifier.weight(1f))
                Box(
                    Modifier.clip(CircleShape).clickable(noRipple(), null) { onTrash() }.padding(8.dp),
                    contentAlignment = Alignment.Center,
                ) { Icon(ShIcons.Trash, "Kosz", tint = Sh.textSecondary, modifier = Modifier.size(22.dp)) }
            }
            val onlineCount = devices.count { it.online }
            Text("${devices.size} urządzeń · $onlineCount online", color = Sh.textMuted, fontSize = 13.sp, modifier = Modifier.padding(top = 4.dp))
            // JOIN jest realny: node zgłasza się sam po wciśnięciu przycisku, a bramka
            // wypycha zdarzenie po WS. Tu pokazujemy oczekujących + ręczne odświeżenie.
            Row(
                Modifier.padding(top = 12.dp).clip(RoundedCornerShape(14.dp)).border(1.5.dp, Sh.dashed, RoundedCornerShape(14.dp))
                    .clickable(noRipple(), null) { onJoin() }.padding(horizontal = 14.dp, vertical = 9.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(ShIcons.JoinRing, null, tint = Sh.textPrimary, modifier = Modifier.size(16.dp))
                Spacer(Modifier.width(8.dp))
                Text(
                    if (pending > 0) "Oczekujące na dodanie: $pending" else "Wciśnij JOIN na urządzeniu, aby je dodać",
                    color = Sh.textPrimary, fontSize = 13.sp, fontWeight = FontWeight.W500,
                )
            }
        }

        // chipy filtra
        Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(start = 24.dp, end = 24.dp, top = 12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FilterChip("Wszystkie", filter == "Wszystkie") { onFilter("Wszystkie") }
            rooms.forEach { r -> FilterChip(r, filter == r) { onFilter(r) } }
        }

        val shown = if (filter == "Wszystkie") devices else devices.filter { it.room == filter }
        val groups = rooms.filter { room -> shown.any { it.room == room } }

        Column(Modifier.padding(start = 20.dp, end = 20.dp, top = 12.dp, bottom = 100.dp), verticalArrangement = Arrangement.spacedBy(18.dp)) {
            when {
                devices.isEmpty() -> DevEmpty("Brak urządzeń", "Aby dodać urządzenie, wciśnij na nim przycisk JOIN — pojawi się tutaj automatycznie do skonfigurowania.")
                shown.isEmpty() -> Column(Modifier.fillMaxWidth().padding(top = 44.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                    Text("Brak urządzeń w tym pokoju", color = Sh.textPrimary, fontSize = 17.sp, fontWeight = FontWeight.W500)
                    Text("„$filter\" nie ma jeszcze przypisanych urządzeń.", color = Sh.textMuted, fontSize = 14.sp, modifier = Modifier.padding(top = 6.dp))
                    Spacer(Modifier.height(18.dp))
                    OutlineButton("Pokaż wszystkie", Modifier.wrapContentWidth()) { onFilter("Wszystkie") }
                }
                else -> groups.forEach { room ->
                    Column {
                        Text(room.uppercase(), color = Sh.textMuted, fontSize = 13.sp, fontWeight = FontWeight.W500, letterSpacing = 0.4.sp, modifier = Modifier.padding(start = 4.dp, bottom = 8.dp))
                        Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(22.dp)).background(Sh.surface)) {
                            shown.filter { it.room == room }.forEach { d -> DevRow(d, typeNames[d.type] ?: d.type) { onOpen(d) } }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun FilterChip(label: String, active: Boolean, onClick: () -> Unit) {
    Box(
        Modifier.clip(RoundedCornerShape(14.dp))
            .background(if (active) Sh.graphite else Sh.surface)
            .then(if (active) Modifier else Modifier.border(1.dp, Sh.divider, RoundedCornerShape(14.dp)))
            .clickable(noRipple(), null) { onClick() }.padding(horizontal = 15.dp, vertical = 8.dp),
    ) {
        Text(label, color = if (active) Color.White else Sh.textSecondary, fontSize = 13.sp, fontWeight = FontWeight.W500, maxLines = 1)
    }
}

@Composable
private fun DevRow(d: Device, typeName: String, onClick: () -> Unit) {
    val col = deviceColor(d.type)
    Row(
        Modifier.fillMaxWidth().clickable(noRipple(), null) { onClick() }.padding(horizontal = 16.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(40.dp).clip(RoundedCornerShape(14.dp)).background(col.bg), contentAlignment = Alignment.Center) {
            Icon(deviceIcon(d.type), null, tint = col.c, modifier = Modifier.size(22.dp))
        }
        Spacer(Modifier.width(14.dp))
        Column(Modifier.weight(1f)) {
            Text(d.name, color = Sh.textPrimary, fontSize = 15.sp, fontWeight = FontWeight.W500)
            Text("$typeName · ${if (d.online) "online" else "offline"}", color = Sh.textMuted, fontSize = 12.sp)
        }
        Box(Modifier.size(8.dp).clip(CircleShape).background(if (d.online) Sh.online else Sh.dangerAlt))
        Spacer(Modifier.width(10.dp))
        Icon(ShIcons.ChevronRight, null, tint = Sh.andMuted, modifier = Modifier.size(18.dp))
    }
}

@Composable
private fun DevEmpty(title: String, desc: String) {
    Column(Modifier.fillMaxWidth().padding(top = 48.dp), horizontalAlignment = Alignment.CenterHorizontally) {
        Box(Modifier.size(76.dp).clip(RoundedCornerShape(24.dp)).background(Color(0xFFEDE8DE)), contentAlignment = Alignment.Center) {
            Icon(ShIcons.Monitor, null, tint = Sh.textSecondary, modifier = Modifier.size(38.dp))
        }
        Spacer(Modifier.height(18.dp))
        Text(title, color = Sh.textPrimary, fontSize = 19.sp, fontWeight = FontWeight.W500)
        Text(desc, color = Sh.textMuted, fontSize = 14.sp, lineHeight = 21.sp, modifier = Modifier.padding(top = 8.dp, start = 30.dp, end = 30.dp), textAlign = androidx.compose.ui.text.style.TextAlign.Center)
    }
}

// ---------------- EDYTOR ----------------

@Composable
private fun DevEditor(
    draft: DevDraft, typeName: String,
    onChange: (DevDraft) -> Unit, onCancel: () -> Unit, onOpenRoom: () -> Unit, onDelete: () -> Unit, onSave: () -> Unit,
) {
    val col = deviceColor(draft.type)
    Column(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing)) {
        Row(Modifier.fillMaxWidth().padding(start = 2.dp, end = 14.dp, top = 2.dp, bottom = 10.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(44.dp).clip(CircleShape).clickable(noRipple(), null) { onCancel() }, contentAlignment = Alignment.Center) {
                Icon(ShIcons.ChevronLeft, "Wstecz", tint = Sh.textPrimary, modifier = Modifier.size(24.dp))
            }
            Column(Modifier.padding(start = 6.dp)) {
                Text(if (draft.id == null) "Nowe urządzenie" else "Edytuj urządzenie", color = Sh.textPrimary, fontSize = 20.sp, fontWeight = FontWeight.W500)
                if (draft.joining) Row(Modifier.padding(top = 1.dp), verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(7.dp).clip(CircleShape).background(Sh.online))
                    Spacer(Modifier.width(6.dp))
                    Text("Wykryto przez bramkę", color = Sh.online, fontSize = 12.sp)
                }
            }
        }

        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(start = 20.dp, end = 20.dp, top = 4.dp, bottom = 20.dp)) {
            Box(Modifier.fillMaxWidth().padding(vertical = 8.dp), contentAlignment = Alignment.Center) {
                Box(Modifier.size(72.dp).clip(RoundedCornerShape(22.dp)).background(col.bg), contentAlignment = Alignment.Center) {
                    Icon(deviceIcon(draft.type), null, tint = col.c, modifier = Modifier.size(38.dp))
                }
            }
            Spacer(Modifier.height(12.dp))
            InputField("Nazwa", draft.name, { onChange(draft.copy(name = it)) }, placeholder = "np. Lampa salon")
            Spacer(Modifier.height(14.dp))
            PickerRow("Typ urządzenia · wykryty automatycznie", typeName, locked = true)
            Spacer(Modifier.height(12.dp))
            PickerRow("Pokój", draft.room, onClick = onOpenRoom)
            if (draft.id != null) {
                Spacer(Modifier.height(24.dp))
                Row(
                    Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).border(1.5.dp, Color(0xFFE7C7BF), RoundedCornerShape(16.dp))
                        .clickable(noRipple(), null) { onDelete() }.padding(13.dp),
                    horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically,
                ) {
                    Icon(ShIcons.Trash, null, tint = Sh.danger, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(8.dp))
                    Text("Usuń urządzenie", color = Sh.danger, fontSize = 14.sp, fontWeight = FontWeight.W500)
                }
            }
        }

        Row(Modifier.fillMaxWidth().background(Sh.bg).padding(horizontal = 20.dp, vertical = 12.dp), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            OutlineButton(if (draft.joining) "Odrzuć" else "Anuluj", Modifier.weight(1f), onCancel)
            Box(
                Modifier.weight(1.4f).clip(RoundedCornerShape(16.dp)).background(Sh.graphiteBtn)
                    .clickable(noRipple(), null) { onSave() }.padding(vertical = 14.dp),
                contentAlignment = Alignment.Center,
            ) { Text("Zapisz", color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.W500) }
        }
    }
}

// ---------------- JOIN (dodaj / wymień / przywróć) ----------------

@Composable
private fun PrimaryBtn(text: String, onClick: () -> Unit) {
    Box(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(15.dp)).background(Sh.graphiteBtn)
            .clickable(noRipple(), null) { onClick() }.padding(vertical = 14.dp),
        contentAlignment = Alignment.Center,
    ) { Text(text, color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.W500) }
}

/**
 * Popup po JOIN. Klasyfikacja wg factory_id chipa (aktywne bramka wycisza — tu nie docierają):
 *  - **isRestore** (chip należy do noda z kosza/detached): nazwa noda read-only + „Przywróć" / „Odrzuć".
 *  - inaczej (nowy chip): krok 1 przyciski „Dodaj nowe" / „Wymień istniejące"(gdy jest cel) / „Odrzuć";
 *    „Dodaj nowe" → krok 2 z nazwą (auto-propozycja, edytowalna).
 * Tło (poza kartą) = odłóż (banner dalej pokazuje kolejkę). „Odrzuć" = skasuj żądanie.
 */
@Composable
private fun JoinDialog(
    factory: String, type: Int,
    isRestore: Boolean, restoreName: String,
    nameSuggestion: String, hasReplaceTargets: Boolean,
    onRestore: () -> Unit,
    onCreate: (String) -> Unit,
    onReplace: () -> Unit,
    onReject: () -> Unit,
    onDismiss: () -> Unit,
) {
    var naming by remember { mutableStateOf(false) }         // tryb "new": krok 2 (nazwa)
    var name by remember { mutableStateOf(nameSuggestion) }
    val uiType = NodeTypes.toUiType(type)
    val col = deviceColor(uiType)
    Box(
        Modifier.fillMaxSize().background(Color(0xFF201B13).copy(alpha = 0.45f))
            .clickable(noRipple(), null) { onDismiss() }.padding(28.dp),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier.widthIn(max = 340.dp).clip(RoundedCornerShape(24.dp)).background(Sh.surface)
                .clickable(noRipple(), null) {}
                .padding(start = 22.dp, end = 22.dp, top = 24.dp, bottom = 18.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(52.dp).clip(RoundedCornerShape(16.dp)).background(col.bg), contentAlignment = Alignment.Center) {
                    Icon(deviceIcon(uiType), null, tint = col.c, modifier = Modifier.size(26.dp))
                }
                Column(Modifier.padding(start = 14.dp)) {
                    Text(if (isRestore) "Wykryto znane urządzenie" else "Nowe urządzenie", color = Sh.textPrimary, fontSize = 19.sp, fontWeight = FontWeight.W500)
                    Text("${NodeTypes.label(type)} · ${factory.takeLast(6).uppercase()}", color = Sh.textMuted, fontSize = 12.sp)
                }
            }
            Spacer(Modifier.height(18.dp))

            when {
                isRestore -> {
                    Text("Ten chip należał do usuniętego urządzenia:", color = Sh.textMuted, fontSize = 12.sp)
                    Spacer(Modifier.height(6.dp))
                    Box(Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Sh.fieldBg).border(1.dp, Sh.divider, RoundedCornerShape(14.dp)).padding(horizontal = 14.dp, vertical = 13.dp)) {
                        Text(restoreName.ifBlank { NodeTypes.label(type) }, color = Sh.textPrimary, fontSize = 16.sp)
                    }
                    Spacer(Modifier.height(16.dp))
                    PrimaryBtn("Przywróć") { onRestore() }
                    Spacer(Modifier.height(10.dp))
                    OutlineButton("Odrzuć", Modifier.fillMaxWidth()) { onReject() }
                }
                naming -> {
                    InputField("Nazwa", name, { name = it })
                    Spacer(Modifier.height(16.dp))
                    PrimaryBtn("Dodaj") { onCreate(name.trim().ifBlank { nameSuggestion }) }
                    Spacer(Modifier.height(10.dp))
                    OutlineButton("Wstecz", Modifier.fillMaxWidth()) { naming = false }
                }
                else -> {
                    PrimaryBtn("Dodaj nowe") { naming = true }
                    if (hasReplaceTargets) {
                        Spacer(Modifier.height(10.dp))
                        OutlineButton("Wymień istniejące", Modifier.fillMaxWidth()) { onReplace() }
                    }
                    Spacer(Modifier.height(10.dp))
                    OutlineButton("Odrzuć", Modifier.fillMaxWidth()) { onReject() }
                }
            }
        }
    }
}

// ---------------- KOSZ ----------------

/**
 * Kosz = soft-usunięte nody na mirrorze (retencja 60 dni; twarde usunięcie tylko cronem —
 * ZERO "opróżnij kosz"). Przywrócenie → node wraca lokalnie jako `detached`.
 */
@Composable
private fun TrashScreen(
    items: List<com.aitronic.smarthome.data.net.TrashNodeDto>, nowS: Long,
    onBack: () -> Unit, onRestore: (com.aitronic.smarthome.data.net.TrashNodeDto) -> Unit,
) {
    Column(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.statusBars)) {
        Row(Modifier.fillMaxWidth().padding(start = 2.dp, end = 14.dp, top = 2.dp, bottom = 10.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(44.dp).clip(CircleShape).clickable(noRipple(), null) { onBack() }, contentAlignment = Alignment.Center) {
                Icon(ShIcons.ChevronLeft, "Wstecz", tint = Sh.textPrimary, modifier = Modifier.size(24.dp))
            }
            Column(Modifier.padding(start = 6.dp)) {
                Text("Kosz", color = Sh.textPrimary, fontSize = 20.sp, fontWeight = FontWeight.W500)
                Text("Usuwane automatycznie po 60 dniach", color = Sh.textMuted, fontSize = 12.sp)
            }
        }
        if (items.isEmpty()) {
            Column(Modifier.fillMaxSize().padding(bottom = 60.dp), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center) {
                Box(Modifier.size(76.dp).clip(RoundedCornerShape(24.dp)).background(Color(0xFFEDE8DE)), contentAlignment = Alignment.Center) {
                    Icon(ShIcons.Trash, null, tint = Sh.textSecondary, modifier = Modifier.size(36.dp))
                }
                Spacer(Modifier.height(16.dp))
                Text("Kosz jest pusty", color = Sh.textPrimary, fontSize = 18.sp, fontWeight = FontWeight.W500)
                Text("Usunięte urządzenia pojawią się tutaj.", color = Sh.textMuted, fontSize = 14.sp, modifier = Modifier.padding(top = 6.dp))
            }
        } else {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(start = 20.dp, end = 20.dp, top = 6.dp, bottom = 100.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                items.forEach { t ->
                    val uiType = NodeTypes.toUiType(t.type)
                    val col = deviceColor(uiType)
                    val days = if (nowS > 0 && t.archivedAt > 0) ((nowS - t.archivedAt) / 86400L).coerceAtLeast(0) else -1L
                    Row(
                        Modifier.fillMaxWidth().clip(RoundedCornerShape(18.dp)).background(Sh.surface).padding(horizontal = 14.dp, vertical = 12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Box(Modifier.size(40.dp).clip(RoundedCornerShape(14.dp)).background(col.bg), contentAlignment = Alignment.Center) {
                            Icon(deviceIcon(uiType), null, tint = col.c, modifier = Modifier.size(22.dp))
                        }
                        Spacer(Modifier.width(14.dp))
                        Column(Modifier.weight(1f)) {
                            Text(t.name.ifBlank { NodeTypes.label(t.type) }, color = Sh.textPrimary, fontSize = 15.sp, fontWeight = FontWeight.W500)
                            Text(
                                if (days >= 0) "${NodeTypes.label(t.type)} · w koszu ${days}d" else NodeTypes.label(t.type),
                                color = Sh.textMuted, fontSize = 12.sp,
                            )
                        }
                        Box(
                            Modifier.clip(RoundedCornerShape(12.dp)).border(1.5.dp, Sh.fieldBorder, RoundedCornerShape(12.dp))
                                .clickable(noRipple(), null) { onRestore(t) }.padding(horizontal = 14.dp, vertical = 8.dp),
                        ) { Text("Przywróć", color = Sh.textPrimary, fontSize = 13.sp, fontWeight = FontWeight.W500) }
                    }
                }
            }
        }
    }
}
