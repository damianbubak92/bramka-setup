# Zarządzanie nodami gen2 — model danych + kontrakt drutu

> Status: **SPEC ZATWIERDZONY** (19.07.2026, 5 decyzji przyklepane — §11). Kontrakt, na którym opieramy
> Go, firmware i apkę. Nic jeszcze nie zaimplementowane.
> Kolejność wdrożenia: (1) data model + migracja Go, (2) kontrakt drutu → firmware, (3) apka.

## 1. Cel i zasady

Profesjonalny cykl życia noda odporny na realne scenariusze: dodawanie, wymiana zepsutego chipa
z zachowaniem historii, usuwanie z możliwością odzysku, oraz „ożywający trup" nie robiący bałaganu
w eterze. Trzy zasady, z których wszystko wynika:

1. **Tożsamość logiczna ≠ adres ≠ chip.** Trzy osobne pojęcia (§2). Historia i automatyzacje wiszą
   na tożsamości logicznej, nie na adresie.
2. **Egzekwowanie tożsamości jest REAKTYWNE.** Bramka waliduje `(adres, factory_id)` przy KAŻDEJ
   telemetrii; nieznane wiązanie → odsyła „wypisz się i zamilcz". Poprawność nie zależy od dostarczenia
   żadnej komendy remove — system sam się leczy, gdy nod się odezwie.
3. **Kasowanie jest odwracalne w oknie czasu, ale tylko czasowo.** Bramka kasuje twardo (czysto),
   serwer trzyma w „koszu" przez retencję; trwałe usunięcie robi wyłącznie upływ czasu (żadnego
   ręcznego „opróżnij kosz" — dziecko z telefonem nie skasuje klientowi lat historii).

## 2. Tożsamość: `id` / `address` / `factory_id`

| pojęcie | co to | zmienność | gdzie |
|---|---|---|---|
| **`id`** | tożsamość LOGICZNA noda (kotwica historii i reguł) | **stała, NIGDY nie reużywana** (`AUTOINCREMENT`) | tylko baza bramki + mirror; **NIE na drucie** |
| **`address`** | adres RF do routingu (co leci na antenie) | reużywalny, może się zmienić, może być `NULL` (odłączony) | druk + baza |
| **`factory_id`** | aktualny fizyczny chip (CC1310 IEEE MAC z FCFG, 8B, globalnie unikalny) | zmienia się przy wymianie chipa, może być `NULL` | druk + baza |

**Dlaczego rozdzielone:** przywrócony z kosza nod czeka „bez adresu i bez chipa", a przy re-parowaniu
dostaje nowy wolny adres i nowy factory_id — historia trzyma się po `id`. Adresy zwalniają się od razu
przy kasowaniu (pula się nie zamraża). Automatyzacje po `id` przeżywają zmianę adresu.

**Limit `id`:** `INTEGER PRIMARY KEY AUTOINCREMENT` = 64-bit → max ~9,2×10¹⁸. Per bramka; w całym życiu
urządzenia zarejestrujesz setki nodów — granicy nie da się dojść. `AUTOINCREMENT` (nie goły rowid) jest
OBOWIĄZKOWY: gwarantuje, że id skasowanego noda nie wróci do nowego (inaczej kosz/restore by się rozsypał).

## 3. Schemat `node` + statusy

```sql
CREATE TABLE node (
  id             INTEGER PRIMARY KEY AUTOINCREMENT,  -- tożsamość logiczna, stała
  address        INTEGER,          -- 0x10-0xEF, NULL = odłączony; reużywalny
  factory_id     TEXT,             -- hex 8B, NULL = odłączony; unikalny wśród aktywnych
  node_type      INTEGER NOT NULL,
  name           TEXT,
  room           TEXT,
  status         TEXT NOT NULL,    -- pending_join | active | detached
  provisioned_at INTEGER,
  last_seen      INTEGER
);
-- adres i factory_id unikalne TAM GDZIE nie-NULL (nod może mieć naraz jeden adres/chip):
CREATE UNIQUE INDEX ux_node_addr    ON node(address)    WHERE address    IS NOT NULL;
CREATE UNIQUE INDEX ux_node_factory ON node(factory_id) WHERE factory_id IS NOT NULL;
```

**Statusy:**
- `pending_join` — przydzielono adres, czekamy aż nod potwierdzi (pierwsza telemetria pod nowym adresem+factory_id).
- `active` — potwierdzony, żywy (ma adres + factory_id).
- `detached` — przywrócony z kosza: ma `id` + historię, ale `address = NULL`, `factory_id = NULL`; czeka na sparowanie.

`pending_remove` **znika** — w modelu reaktywnym kasowanie jest natychmiastowe (hard-delete lokalnie + kosz
na serwerze), nie trzymamy wiersza czekając na potwierdzenie noda.

## 4. Kluczowanie historii i automatyzacji po `id`

Wszystkie tabele danych (`node_param`, `solar_history`, `solar_hourly/daily/monthly`, przyszłe per-typ)
kluczowane kolumną `node_id` = **`node.id`** (tożsamość logiczna), NIE adresem. Jeden komplet tabel per TYP,
wiele nodów tego typu współdzieli je przez `node_id` (nie per-node tabele — patrz decyzja niżej). Kasowanie
noda = `DELETE ... WHERE node_id = <id>` (kompaktowo, przez `dropSolarNode` uogólnione na wszystkie typy).

Reguły automatyzacji odwołują się do **`id`**. Silnik na M4F pracuje na ADRESACH (bo to jest na drucie);
Go mapuje `id → aktualny address` przy pushu reguł do M4F. Zmiana adresu (re-parowanie) → Go re-pushuje reguły
z nowym adresem → automatyzacja działa dalej bez ingerencji klienta.

## 5. Kontrakt drutu

### 5.1 factory_id w każdej ramce

Dziś ramka legitymuje się tylko adresem. **Dodajemy `factory_id[8]` na poziomie ramki** (poza
MessageStruct, żeby MessageStruct został 44B). Znaczenie: „nod, którego dotyczy ta ramka" —
nadawca przy uplinku (nod→bramka), adresat przy downlinku (bramka→nod).

Proponowana ramka (finalny layout dopinasz w firmware):
```
[ addr:1 ][ factory_id:8 ][ type:1 ][ MessageStruct:44 ][ seq:1 ][ crc:1 ]   // było bez factory_id
```
Koszt: +8B/ramkę. Dla noda co 2 min — airtime bez znaczenia; CC1310 sub-GHz mieści z zapasem.
(Rozważana lżejsza alternatywa — 1-bajtowy `epoch` per adres — ODRZUCONA: factory_id jest autorytatywny
i już istnieje, nie wymyślamy własnego licznika.)

### 5.2 Walidacja

- **Uplink (telemetria/JOIN/potwierdzenia):** bramka przyjmuje ramkę tylko gdy `(addr, factory_id)`
  = zarejestrowane aktywne wiązanie. Inaczej → `MSG_UNREGISTERED` w dół (§5.3). JOIN (addr=0xFF) jest
  wyjątkiem — zawsze akceptowany do rejestru pending (niesie factory_id w payloadzie, jak dziś).
- **Downlink (komendy):** nod wykonuje komendę tylko gdy `factory_id` w ramce = JEGO własne FCFG
  (nie sam adres). To zabija groźny przypadek: stary chip na reużytym adresie NIE wykona cudzej komendy.

### 5.3 Nowa komenda `MSG_UNREGISTERED` (bramka→nod)

Znaczenie: „nie jesteś zarejestrowany pod `(addr, factory_id)` — wykasuj przydzielony adres z NVS, wróć
do stanu fabrycznego, **zamilcz** i czekaj na JOIN". Zachowanie noda:
- Kasuje adres w NVS **raz** (idempotentnie), przechodzi w stan unprovisioned (adres 0xFF).
- **Przestaje nadawać telemetrię.** Nadaje wyłącznie JOIN po wciśnięciu przycisku.
- Gdyby komunikat zginął — nod ponowi telemetrię, dostanie `MSG_UNREGISTERED` znów (self-heal).

To utrzymuje porządek w eterze: żaden „trup" ani skasowany nod nie zapycha pasma niechcianymi danymi.

## 6. Przepływy cyklu życia

### 6.1 Dodaj nowe urządzenie
1. Przycisk JOIN na nodzie → `JOIN_REQUEST` (addr=0xFF, factory_id + typ w payloadzie).
2. Bramka → rejestr pending → apka: okienko „Dodaj nowe / Wymień istniejące".
3. User: **Utwórz nowe** → nazwa + pokój.
4. Bramka: nowy `node` (nowy `id`, `allocAddr`, `factory_id`=chip, `status=pending_join`) → `JOIN_ACCEPT`
   (target=factory_id, assigned address).
5. Nod: waliduje, że JOIN_ACCEPT jest dla jego factory_id → zapisuje adres w NVS → potwierdza (1. telemetria).
6. Bramka: na 1. poprawnej telemetrii → `status=active`.

### 6.2 Podepnij do istniejącego (= wymiana LUB re-parowanie przywróconego)
Jeden mechanizm, dwa wejścia. Apka ma DOKŁADNIE dwie opcje po JOIN: **„Utwórz nowe"** i **„Wymień istniejące"**.
„Wymień istniejące" → dropdown **kompatybilnych (ten sam typ) nodów logicznych**: aktywne (wymiana chipa)
ORAZ odłączone/`detached` (re-parowanie po restore). Pokazujemy nazwę + pokój + „ostatnio widziany".
1. Nowy chip → JOIN → apka „Wymień istniejące" → wybór noda `N`.
2. Bramka: `factory_id = nowy`; adres — jeśli `N` był aktywny, zostaje jego adres; jeśli `detached`,
   `allocAddr` nadaje nowy. `status=pending_join`. `JOIN_ACCEPT` (target=nowy factory_id, address).
3. Nowy chip potwierdza → `active`. **Historia pod `id` = `N` kontynuowana.**
4. Stary chip (jeśli żył): jego `(addr, stary factory_id)` ≠ wiązanie → `MSG_UNREGISTERED` → milknie.

### 6.3 Usuń urządzenie → kosz
1. Apka: „Usuń" → **twarde potwierdzenie** („to trwale usunie [nazwa] i X historii — wpisz nazwę").
2. Bramka: hard-delete `node` + jego historię lokalnie; **adres OD RAZU wolny** (`allocAddr` go odzyska).
   Do mirrora: `archive_node` (ustaw `archived_at`, **NIE kasuj** — kosz).
3. (Opcjonalnie best-effort) `MSG_UNREGISTERED` na ostatni adres `N`, żeby żywy chip zamilkł natychmiast.
4. Jak chip żywy a komunikat nie doszedł: odezwie się → `MSG_UNREGISTERED` → milknie (reaktywnie).

### 6.4 Przywróć z kosza
1. Apka: „Kosz" → lista archived w oknie retencji (nazwa, typ, „usunięto N dni temu", „trwałe za M dni").
2. „Przywróć" → bramka ściąga z mirrora dane noda (`id` + historia) → wstawia lokalnie z `address=NULL`,
   `factory_id=NULL`, `status=detached`. Mirror: czyści `archived_at`.
3. Nod widoczny w Urządzeniach jako „czeka na sparowanie".
4. Sparowanie: jak §6.2 (re-parowanie odłączonego) — nowy chip, nowy adres, historia dalej pod tym `id`.

### 6.5 Reaktywne „unregistered" (rdzeń odporności)
Każda telemetria z `(addr, factory_id)` nie pasującym do aktywnego wiązania → bramka odsyła
`MSG_UNREGISTERED` → nod kasuje adres, milknie. Pokrywa: ożywionego trupa, skasowanego offline-noda,
starego chipa po wymianie.

## 7. Alokacja adresów
`allocAddr` = najniższy wolny w puli 0x10-0xEF; „wolny" = brak `node` z tym `address` (nie-NULL).
Zwalniany OD RAZU przy kasowaniu (nod idzie do kosza z `id`, nie z adresem). `detached` mają `address=NULL`
→ nie trzymają adresu. Więc pula nie zamraża się przez kosz — twój kluczowy postulat.

## 8. Kosz, retencja, przywracanie (serwer)
- `gw_node` dostaje `archived_at BIGINT NULL`.
- `archive_node` (z bramki przy kasowaniu): `UPDATE gw_node SET archived_at=now WHERE id=?`. Historia
  (po `node_id`=id) zostaje — idzie za nodem.
- „Żywy" mirror = `archived_at IS NULL`. Przywracanie = bramka czyta archived noda → serwer czyści `archived_at`.
- **Cron na serwerze** kasuje fizycznie `node` + historię gdzie `archived_at < now - RETENCJA`
  (**60 dni**). **Wyłącznie czasowo**, zero ręcznego „opróżnij kosz".

## 9. Backup/mirror — zmiany
- Klucz mirrora `node_id` = `node.id` (stały) zamiast adresu. Triggery i restore bez zmian koncepcyjnie.
- Nowy kind `archive_node` (soft-delete) zamiast twardego `purge_node`. Trwały purge robi cron serwera.
- **Restore pojedynczego noda** (odzysk z kosza) — nowy endpoint/tryb: pull archived noda z mirrora do bramki.
- Pełny restore bramki (§disaster recovery) bez zmian — zaciąga aktywne nody (archived pomija).

## 10. Migracja z obecnego modelu (`node_id` = adres)
Dziś `node.node_id` = adres, historia kluczowana adresem. Jednorazowa migracja Go:
1. `node`: dodaj kolumny `id` (autoincrement), `address`; dla każdego istniejącego wiersza `address = stary
   node_id`, `id = kolejny autoincrement`.
2. Re-key historii: dla każdego noda `UPDATE <tabele> SET node_id = <new id> WHERE node_id = <stary adres>`.
3. Kilka nodów → trywialne. Mirror analogicznie (jednorazowy re-key albo świeży seed).

## 11. Decyzje — ZATWIERDZONE (19.07)
1. **Layout ramki**: `factory_id[8]` w **nagłówku ramki** (poza MessageStruct, zostaje 44B). Powód: zaraz po
   „wejściu" ramki do noda, potrzebne do sprawdzenia zgodności chipa przed dalszą obróbką. ✅
2. **Retencja kosza**: **60 dni**. ✅
3. **`pending_remove` usunięty** — kasowanie natychmiastowe + reaktywne. ✅
4. **Dokładnie dwie opcje w apce**: „Utwórz nowe" / „Wymień istniejące" (wymiana i re-parowanie pod jedną). ✅
5. **`MSG_UNREGISTERED` przy kasowaniu best-effort** (żywy chip milknie od razu) + reaktywny jako gwarancja. ✅

## 12. Kolejność wdrożenia
1. **Go + data model**: refaktor `node_id`→`id`+`address`, migracja, statusy, `allocAddr`, kosz/retencja/restore.
2. **Kontrakt drutu → firmware**: factory_id w ramce, walidacja, `MSG_UNREGISTERED`, zachowanie noda.
   (Node robisz raz — dlatego kontrakt PRZED firmware.)
3. **Apka**: dialog dodaj/wymień, dropdown kompatybilnych, kosz+przywracanie, confirm na usuwaniu.

Powiązane: [[provisioning-model]], [[gen2-backup-mirror]], [[solar-aggregation-model]], `Docs/ARCHITECTURE-GEN2.md`.
