# Architektura Gen2 — port silnika automatyzacji na AM62

> Żywy dokument projektowy. Bazuje na **sprawdzonej gen1** (CC3235 + CC1310, działa 2 lata
> bezawaryjnie). Gen2 przenosi role na AM62: silnik na M4F (RTOS), host/chmura/UI na A53/Linux,
> CC1310 zostaje jako radio sub-GHz. Kody gen1 (referencja): patrz pamięć `legacy-gateway-code`.

## 1. Podział ról

```
Node RF ──EasyLink──> CC1310 ──SPI──> M4F (RTOS)                 A53 / Linux
                       (radio)         ├─ NodesData (autorytatywny stan)
                                       ├─ ENGINE automatyzacji (deterministyczny)
                                       └─ akcje do nodów                ▲ RPMsg ▼
                                                                  ┌─────┴───────┐
                                                                  │ remote access (telefon/web)
                                                                  │ telemetria → chmura
                                                                  │ SQLite (config+reguły=źródło prawdy)
                                                                  │ NTP/czas
                                                                  └─────────────┘
```

- **CC1310** (slave SPI): koncentrator RF (EasyLink, ACK+retry). „Lean radiomodem" — buforuje pakiety, gada z M4F po SPI. RF bez zmian vs gen1.
- **M4F** (RTOS, SPI master): real-time hub. Odbiera dane nodów, trzyma `NodesData`, **ewaluuje reguły**, wysyła akcje. Raportuje w górę przez RPMsg. Trzyma aktywny zestaw reguł w RAM (cache).
- **A53 / Linux**: config/UI/chmura. SQLite = źródło prawdy (reguły+config). API dla telefonu (CRUD reguł, sterowanie, statystyki). Telemetria do chmury. NTP. **JSON parsowany TYLKO tutaj** — M4F dostaje reguły binarnie.

## 2. Decyzje architektoniczne (log)

| # | Decyzja | Uzasadnienie |
|---|---|---|
| D1 | Silnik na **M4F + RTOS** (FreeRTOS, MCU+ SDK) | Determinizm, brak round-tripa do Linuxa na każdą regułę. M4F = mocny dedykowany rdzeń. |
| D2 | **M4F = SPI master**, CC1310 = slave | Determinizm enginu (master włada timingiem; ISR od slave tylko flaguje), backpressure, multi-drop (drugi radiomodem na CS), lean CC1310. |
| D3 | **2 linie handshake** `MASTER_READY`/`SLAVE_READY` | Sterownik SPI wymaga uzbrojenia slave (`SPI_transfer`) ZANIM master taktuje. Sprawdzone w gen1 (zalecenie TI). |
| D4 | **JSON + persystencja na Linuxie**, M4F dostaje reguły binarnie przez RPMsg | M4F lean/deterministyczny; Linux ma zasoby na rosnące JSON-y i typy nodów (to powód gen2 — CC3235 by się zatkał). |
| D5 | Engine **event-driven** (nie polling 60s jak gen1) | Ewaluacja na napływ danych nodu + tick czasowy. Szybsza reakcja. |
| D6 | **Parytet najpierw**, generalizacja potem | `MessageStruct` jest domenowy (solar/bufor/pompa) — najpierw działać jak gen1, potem uogólnić model nodów. |
| D7 | `SPI FRAME_SIZE = 128 B` | Zapas na nowe typy nodów/pól. |
| D8 | `MAX_RULES = 100` | gen1=5 (limit FRAM). M4F RAM pozwala; realny smart-home. |
| D9 | Linux = źródło prawdy reguł; M4F = cache RAM, sync przez RPMsg | Atomic swap na M4F (wzór: FRAM dual-slot gen1). |

## 3. Protokół SPI — M4F (master) ↔ CC1310 (slave)

Mechanika identyczna jak gen1 (`spi_master_task.c` CC1310 + `spiTask.c` CC3235), role odwrócone.

### Linie GPIO (active-LOW, idle HIGH)
- `MASTER_READY`: M4F out → CC1310 in (IRQ falling) — „master chce nadać, slave uzbrój się".
- `SLAVE_READY`: CC1310 out → M4F in (IRQ falling) — „slave uzbroił `SPI_transfer()`, taktuj TERAZ" (uniwersalny sygnał *go*; też = request-to-send gdy CC1310 ma pakiet RF).

> Sedno (z kodu gen1): slave NAJPIERW woła `SPI_transfer()` (uzbraja bufory), DOPIERO POTEM ściąga
> `SLAVE_READY` w dół. Master taktuje na to zbocze. Bez `MASTER_READY` slave nie wie kiedy uzbroić
> się przy transferze inicjowanym przez mastera.

### Scenariusz A — M4F nadaje (komenda do nodu)
```
M4F:    assert MASTER_READY
CC1310: IRQ → SPI_transfer() [uzbroj] → assert SLAVE_READY
M4F:    IRQ na SLAVE_READY → SPI_transfer() [taktuje] → wymiana
oba:    deassert; M4F czeka na callback TRANSFER_DONE (timeout 300 ms)
```

### Scenariusz B — CC1310 ma pakiet RF (nadaje w górę)
```
CC1310: SPI_transfer() [uzbroj, TX=pakiet] → assert SLAVE_READY   (bez MASTER_READY)
M4F:    IRQ na SLAVE_READY → SPI_transfer() [taktuje] → odbiera pakiet → engine + RPMsg w górę
oba:    deassert
```

### Timeouty / retry (z gen1)
- Cykl: **300 ms**. Retry: **3×**. Potem zgłoszenie błędu (M4F → log/RPMsg).

### Ramka SPI (ulepszenie vs gen1: nagłówek + CRC + drenaż serii)
Stała ramka **128 B**:
```
[0]      magic 0xA5
[1]      type     (FRAME_NODE_DATA | FRAME_NODE_CMD | FRAME_ACK | FRAME_NOP)
[2]      seq
[3]      pending  ile JESZCZE ramek nadawca ma w kolejce → master drenuje aż 0
[4]      len      długość payloadu (≤ 120)
[5..6]   flags/reserved (0)
[7]      reserved (0)
[8..]    payload  (np. MessageStruct)
[126..127] CRC16-CCITT (jak w RPMsg, init 0xFFFF, poly 0x1021)
```
- `pending` rozwiązuje burst (wiele pakietów RF naraz) — kluczowe przy skalowaniu liczby nodów.
- CRC16 na poziomie ramki SPI — gen1 nie miał (polegał na CRC warstwy RF); to twardszy transport.

> **Parytet najpierw**: jak gen1, transfer traktujemy pół-dupleksowo per kierunek (strona „bierna"
> wysyła `FRAME_NOP`). Prawdziwy full-duplex piggyback (komenda M4F + pakiet CC1310 w jednej
> transakcji) = optymalizacja na później.

## 4. Protokół RPMsg — A53/Linux ↔ M4F

Rozszerza istniejący `shared/protocol.h` (sprawdzone: `[type][seq][len][payload][CRC16]` + ACK +
retry + heartbeat). Nowe typy (nie kolidują z 0x01–0x04, 0x10–0x11, 0x20, 0xF0–0xF1, 0xFF):

```c
/* Linux -> M4F (config / sterowanie) */
#define MSG_RULE_BEGIN     0x30u  /* payload: u16 ruleCount — start wgrywania zestawu */
#define MSG_RULE_ITEM      0x31u  /* payload: u16 index + AutomationRule (1 reguła/ramkę) */
#define MSG_RULE_COMMIT    0x32u  /* payload: u16 expectedCount + u32 crc32 — atomowy swap */
#define MSG_NODE_CMD       0x33u  /* payload: MessageStruct — komenda z telefonu → relay do nodu */

/* M4F -> Linux (telemetria / stany) */
#define MSG_NODE_TELEMETRY 0x40u  /* payload: MessageStruct — surowe dane nodu → chmura/DB */
#define MSG_NODE_STATE     0x41u  /* payload: NodesData snapshot — bieżące stany → telefon */
#define MSG_RULE_FIRED     0x42u  /* payload: u16 ruleIndex + RuleAction — audyt automatyzacji */
```
Reguła (~220 B) mieści się w jednej ramce RPMsg (`MAX_PAYLOAD_SIZE=480`). 1 reguła = 1 `RULE_ITEM`.

### Wgrywanie reguł (chunked, skalowalne, atomowe)
```
Linux: RULE_BEGIN(N) → RULE_ITEM(0) → … → RULE_ITEM(N-1) → RULE_COMMIT(N, crc32)
M4F:   buduje shadow-zestaw w RAM → na COMMIT (gdy count i crc32 OK) atomowy swap aktywnego zestawu
       → ACK. Częściowy/uszkodzony zestaw odrzucony (stary pozostaje aktywny).
```
Wzór niezawodności: gen1 FRAM dual-slot (nigdy nie zostajemy z połową zestawu).

### Synchronizacja przy starcie
Po `HELLO_ACK`: Linux czyta reguły z SQLite → `RULE_BEGIN..COMMIT` do M4F. **Linux = źródło prawdy**,
M4F = aktywny cache w RAM. (M4F po SoC reset wstaje szybciej niż Linux — czeka na push po HELLO.)

## 5. Struktury danych

Przeniesione z gen1 (`messageProtocol.h`, `automationRules.h`) — docelowo do `shared/` (wspólne dla
M4F C i Go cgo). **D6: parytet najpierw**, generalizacja modelu nodów później.

- `MessageStruct` (node↔gateway, ~49 B): `id, type, cmd, length, union{solarData|pumpData|buforData|textData}`.
- `NodesData` (snapshot stanów: T1..T4, Tin/Tout/Tcol, energyGain, flowRate, sBuforTemp, pumpState).
- `AutomationRule` (~220 B): `name[64], conditionCount, conditions[3], action`.
  - `RuleCondition`: TIME | PARAMETER (device.param OP threshold) | PARAMETER_DELTA (dev1.p1 − dev2.p2 OP delta). Do 3 warunków AND.
  - `RuleAction`: SET_RELAY (target, on/off) | SEND_MESSAGE (target, tekst).

## 6. Przepływy end-to-end

```
Telemetria:  node → RF → CC1310 → SPI(B) → M4F (NodesData+engine) → RPMsg NODE_TELEMETRY → Linux → chmura/SQLite
Automatyka:  M4F engine (event-driven: NODE_DATA + tick czasowy) → akcja → SPI(A) → CC1310 → RF → node
Edycja reguł: telefon → API Linux → SQLite → RPMsg RULE_BEGIN..COMMIT → M4F swap
Sterowanie:  telefon → API Linux → RPMsg NODE_CMD → M4F → SPI(A) → CC1310 → RF → node
```

## 7. Silnik na M4F (event-driven)

- Task RTOS „engine": czeka na (a) nowy `MessageStruct` z SPI (aktualizuje `NodesData`, ewaluuje),
  (b) tick czasowy (np. 1–10 s) dla warunków `CONDITION_TIME`.
- Ewaluator = port `evaluateAutomationRules()` (czysty C, bez zależności SDK). `getDeviceParameterValue()`
  czyta z `NodesData`. Czas z RTC M4F (źródło czasu TBD — z Linuxa przez RPMsg albo RTC na carrier).
- Akcja → `MessageStruct` → kolejka SPI do CC1310.
- Logika domenowa gen1 do zachowania (dedup pumpState, guard `sBuforTemp<0`) — przenieść lub uogólnić.

## 8. Mapowanie portu gen1 → gen2

| Gen1 (CC3235/CC1310) | Gen2 | Uwagi |
|---|---|---|
| `automationRules.c/.h` | **M4F** (RTOS task) | Port ~1:1, event-driven zamiast 60 s polling |
| `automationRulesJson.c/.h` | **Linux** | JSON tylko po stronie Linuxa |
| `coreTask.c` (orkiestracja) | **M4F** (routing nodów) + **Linux** (chmura/UI) | Rozdzielone |
| `spiTask.c` / `spi_master_task.c` | **M4F SPI master** + **CC1310 SPI slave** | Role odwrócone, mechanika gen1 |
| `wifiTask.c` (SimpleLink) | **Linux** (NetworkManager/wpa_supplicant lub modem) | Przepisać |
| `httpClientTask.c` (telemetria) | **Linux** | URL/api_key do refactoru (hardcoded gen1) |
| `httpsServerTask.c` (API telefonu) | **Linux** | Nowy stack remote access |
| `rulesFramStore.c` (FRAM) | **Linux SQLite** + M4F atomic swap | FRAM → DB; wzór dual-slot zostaje konceptualnie |
| `rtcControl.c` (NTP) | **Linux** (systemd-timesyncd) → czas do M4F | |
| `radio_task.c` / `rfWsnConcentrator.c` | **CC1310 bez zmian** | EasyLink RF zostaje |

## 9. Otwarte / przyszłość

- **Full-duplex piggyback SPI** — gdy ruch wzrośnie (na razie pół-duplex per kierunek, parytet).
- **Generalizacja modelu nodów** — `MessageStruct` domenowy → uogólniony (więcej typów nodów, pól).
- **Źródło czasu M4F** — RTC na carrier vs push z Linuxa przez RPMsg.
- **Warstwa C (DMSC reset)** — staje się realna, bo M4F trzyma żywe sterowanie (crash Linuxa nie może
  go zabić na ~70 s). Decyzja przy dojrzewaniu enginu.
- **Hardcoded gen1 do refactoru**: SSID/hasło Wi-Fi, URL+api_key chmury, auth token, adresy RF, strefa czasowa.
```
