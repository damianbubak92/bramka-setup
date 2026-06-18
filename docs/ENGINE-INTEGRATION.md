# Engine integration (M4F) — gen2 (FreeRTOS)

> Jak zbudować firmware M4F z silnikiem automatyzacji na **FreeRTOS** (MCU+ SDK
> 12.00). Bazuje na `docs/ARCHITECTURE-GEN2.md`. Decyzja: idziemy od razu na
> FreeRTOS + taski/kolejki (nie NoRTOS), żeby bieżące testy były adekwatne do
> docelowej architektury i nie było reworku przy wejściu SPI.

## Pliki i zależności

```
engine.c/.h         rdzeń: NodesData + ewaluator (port gen1) + atomic swap reguł.
                    RTOS-AGNOSTYCZNY (zero SDK/RTOS/RPMsg/SPI). Wspólny dla M4F i
                    Go (Go używa go automation.h przez cgo do enkodera reguł).
engine_rpmsg.c/.h   glue RPMsg: dekoduje RULE_*/NODE_CMD, reportery NODE_*/RULE_FIRED.
                    Decoupled wskaźnikami funkcji (tx_event / reply / tx_node).
ipc_rpmsg_echo.c    COMMS task + ENGINE task + kolejki (FreeRTOS). Setup, recovery,
                    heartbeat, retry — bez zmian względem zweryfikowanej wersji.
shared/automation.h     AutomationRule (196 B), warunki, akcje (kody jako #define).
shared/node_protocol.h  MessageStruct (44 B), NodesData (44 B).
shared/protocol.h       framing RPMsg + MSG_RULE_*/MSG_NODE_* (0x30–0x42).
```

**Do projektu CCS skopiuj płasko**: `engine.c/.h`, `engine_rpmsg.c/.h`,
`ipc_rpmsg_echo.c` + `shared/{automation.h,node_protocol.h,protocol.h}`.
(`m4f-firmware/protocol.h` to mirror `shared/protocol.h` — trzymaj zsynchronizowane.)

**Projekt musi być wariantem FreeRTOS** MCU+ SDK (kernel FreeRTOS w SysConfig).
Wymagane: `configSUPPORT_STATIC_ALLOCATION = 1` (używamy `xTaskCreateStatic` /
`xQueueCreateStatic` — bez sterty). Punkty do strojenia w `ipc_rpmsg_echo.c`:
`ENGINE_TASK_STACK_WORDS`, `ENGINE_TASK_PRIORITY` (ma być **niższy** niż task
comms, żeby RX/heartbeat/recovery nie były głodzone), `OUTBOX_DEPTH`, `NODEIN_DEPTH`.

### Dwie zmiany w projekcie CCS (zweryfikowane buildem 17.06)

1. **`linker.cmd` — zestaw reguł do DDR.** `g_rules[2][MAX_RULES]` to ~39 KB
   (MAX_RULES=100), a M4F DRAM ma tylko 64 KB → `.bss` się nie mieści. `g_rules`
   jest w sekcji `.bss.engine_rules` (atrybut w `engine.c`); dodaj do `SECTIONS`
   w `linker.cmd` mapowanie do DDR (obok `.bss.ipcctrl`):
   ```
   .bss.engine_rules (NOLOAD) : {} palign(8) > M4F_DDR
   ```
   Po zmianie (zweryfikowane): M4F_DRAM 46.5/64 KB, M4F_DDR 55.5/64 KB — z zapasem.
   ⚠️ Reguły są w DDR (czytane przez engine co ~1 s) — dla tej kadencji bez
   znaczenia; gdyby kiedyś trzeba twardego determinizmu, zmniejsz MAX_RULES i wróć
   do DRAM. Ten sam SoC w produkcji (Verdin AM62) → ten sam limit 64 KB DRAM.

2. **Debug UART w trybie CALLBACK.** Świeży `example.syscfg` ustawia
   `debug_log.uartLog.readCallbackFxn = "uart_echo_read_callback"`, więc
   `ti_drivers_open_close.c` odwołuje się do tego symbolu. `ipc_rpmsg_echo.c`
   dostarcza no-op stub (UART input nieużywany) — link się rozwiązuje. (Alternatywa:
   zmienić readMode na BLOCKING w SysConfig.)

### Build CLI (weryfikacja bez IDE)
Po skopiowaniu plików + `Refresh` w CCS makefile’e znają `engine.c`/`engine_rpmsg.c`.
Pełny link można sprawdzić z konsoli:
```
cd Debug && "C:/ti/ccs2051/ccs/utils/bin/gmake.exe" -j 8 all
```
Sukces → `Debug/*.appimage.hs_fs` ("Done !!!").

Lint lokalny (przenośne TU, bez CCS) — TI ARM clang z CCS:
```
tiarmclang -mcpu=cortex-m4 -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
  -fsyntax-only -Wall -Wextra -I shared -I m4f-firmware engine.c engine_rpmsg.c
```
(`ipc_rpmsg_echo.c` ma zależności SDK+FreeRTOS — lint dopiero w CCS.)

## Architektura tasków (lock-free, wzór kolejek z gen1 coreTask)

```
                 RPMessage recvCallback (kontekst IPC RX)
                          | gRxBuffer.pending
                          v
   +----------------- COMMS task (= ipc_rpmsg_echo_main) -----------------+
   | JEDYNY właściciel RPMessage_send + gPendingAcks                      |
   |  - handleLinuxMessage(): HELLO/DATA/ACK/PING + engine_rpmsg_dispatch |
   |  - drainEngineOutbox(): report_rule_fired/telemetry + nodeTxSink     |
   |  - processEventRetries(), doPeriodicTick()                           |
   +----------^-------------------------------------------+--------------+
              | gOutboxQueue (engine->comms)              | gNodeInQueue (SPI->engine; pusta do czasu SPI)
              |                                           v
   +----------+---------------- ENGINE task -------------------------------+
   |  loop: xQueueReceive(qNodeIn, timeout=do najbliższej :00)            |
   |    dane nodu  -> engine_update_node + outbox(TELEMETRY)              |
   |               -> engine_evaluate(ENGINE_EVAL_NODE)  [reguły danych]  |
   |    timeout    -> engine_evaluate(ENGINE_EVAL_TIME)  [reguły czasowe] |
   |  akcja reguły -> engineActionSink -> outbox(RULE_FIRED)   (bez send!)|
   +---------------------------------------------------------------------+
```

## Kadencja ewaluacji (hybryda)

Reguły są **kubełkowane po warunkach** — każdy trigger dotyka tylko reguł, na
które może wpłynąć (bez marnowania CPU):

- **Reguły czasowe** (`COND_TIME`, też puste „always") → ewaluacja na **tick
  minutowy wyrównany do `:00`**. Timeout `xQueueReceive` liczony jako
  `engine_ms_to_next_minute()` (w **milisekundach**, sub-sekundowo — całe sekundy
  jitterują na granicy minuty: tik o włos przed `:00` → mikro-sen 1s → podwójny
  fire), przeliczany co pętlę → wiadomość od noda w pół minuty NIE przesuwa ticku
  (determinizm `:00`).
- **Reguły danych** (`COND_PARAMETER`/`_DELTA`) → ewaluacja **event-driven** na
  napływ danych nodu z `gNodeInQueue`.
- Reguła z czasem **i** danymi jest w obu kubełkach.

**Level-trigger, nie edge**: reguła odpala co przebieg dopóki warunek prawdziwy.
Anty-spam = **sprzężenie zwrotne ze stanu noda** (akcja pomijana, gdy node już
raportuje pożądany stan — guard solar `pumpState`), NIE zatrzask zboczowy: zgubiona
pierwsza komenda jest retransmitowana aż node potwierdzi. Uogólnienie feedbacku na
inne akcje stanowe ląduje z SPI (jedyny realny stan dziś to `pumpState`).
`SEND_MESSAGE` nie ma stanu noda → na razie leci co tick (decyzja odłożona do
remote-access — realne alerty będą param-driven).

**Zegar M4F**: `engine_set_time(h,m,s)` (z `MSG_TIME_SYNC`) ustawia ścianę czasu;
engine **dolicza** upływ z monotonicznego `EngineClockFn` (`ClockP_getTimeUsec`),
więc `COND_TIME` jest dokładny między syncami i tick trzyma się `:00`. Dryf
koryguje okresowy re-sync z Linuxa (NTP — remote-access).

**Wybudzanie przy time-syncu**: COMMS task po `MSG_TIME_SYNC` wrzuca na `gNodeInQueue`
sentinel `ENGINE_NODEIN_TIME_RESYNC` (wartość poza zakresem `NODE_*`, nigdy na drucie).
ENGINE task budzi się, robi `EVAL_TIME` od razu i **przelicza timeout z nowego zegara
→ wyrównanie do `:00` natychmiast** (bez sentinela pierwszy tick po syncu byłby skośny:
czekałby do końca trwającego snu). Dotyczy też re-syncu dryfu.

**Dlaczego bez mutexów** (jak w CC3235): jedyne kanały między taskami to kolejki.
- `RPMessage_send` + `gPendingAcks` dotyka **wyłącznie** COMMS task. ENGINE task
  nigdy nie woła send — wrzuca `outbox_item_t` na `gOutboxQueue`, comms drenuje.
- Aktywny zestaw reguł: lock-free przez konstrukcję — `engine_rules_commit()`
  podmienia indeks aktywnego bufora atomowo, a `engine_evaluate()` czyta go raz
  do lokalnej zmiennej → push w trakcie ewaluacji nie psuje bieżącego przebiegu.
- `gNodeInQueue` / `gOutboxQueue`: pojedynczy producent/konsument przez kolejkę.

## Przepływ wiadomości

```
Linux -> M4F (RULE_*/NODE_CMD):  COMMS recvCallback -> handleLinuxMessage
                                 -> engine_rpmsg_dispatch -> engine_rules_*/nodeTxSink
                                 -> reply MSG_ACK lub MSG_ERROR (echo seq)
Reguła fired (ENGINE):           engineActionSink -> gOutboxQueue
                                 -> COMMS drainEngineOutbox -> report_rule_fired
                                    (audyt do Linuxa) + nodeTxSink (SPI TODO)
Dane nodu (SPI, później):        SPI ISR/task -> gNodeInQueue -> ENGINE
                                 -> engine_update_node + outbox(TELEMETRY)
                                 -> COMMS -> report_telemetry (do Linuxa/chmury)
```

## Sygnalizacja wyniku rule-push (ważne)

- Sukces RULE_BEGIN/ITEM/COMMIT → M4F `sendReply(MSG_ACK, seq)` (echo seq).
- Odrzucenie (zły count/CRC/za duże/za wcześnie) → `sendReply(MSG_ERROR, seq)`.
  Go koreluje po seq i **natychmiast** zwraca błąd (bez czekania na retry/giveup).
  MSG_ERROR jest fire-and-forget (bez retry), echo-seq odrzuconej wiadomości.
- COMMIT robi atomic swap tylko gdy count i CRC32 się zgadzają; inaczej stary
  zestaw zostaje aktywny (wzór gen1 FRAM dual-slot).

## Strona Go (Linux) — enkoder rule-push

`go-services/rpmsg-service/rules.go`:
- Model reguł (Rule/Condition/Action) + `encodeRule()` budujący **bajtowo
  identyczny** obraz `AutomationRule` przez **cgo** (layout własności kompilatora
  C z `automation.h` — zero ryzyka rozjazdu offsetów Go↔C).
- `PushRules()`: `RULE_BEGIN(N)` → `RULE_ITEM(0..N-1)` → `RULE_COMMIT(N, crc32_ieee)`
  (każda reliable, ACK+retry). CRC32 = `hash/crc32.ChecksumIEEE` po obrazach reguł
  w kolejności indeksów = `crc32_calc` na M4F po tablicy shadow.
- `init()` sprawdza rozmiary ABI (`unsafe.Sizeof(C.AutomationRule)` == 196 itd.);
  rozjazd → `abiOK=false` → `PushRules` odmawia (korupcja gorsza niż brak funkcji).
- Test: `rpmsg-service -test push-rules` (przykładowe reguły = gen1 initExampleRules).

⚠️ **Deploy-Go musi kopiować `automation.h`** (poza dotychczasowym `protocol.h`) —
inaczej cgo w `rules.go` się nie zbuduje na bramce. `node_protocol.h` dorzuć, gdy
Go zacznie parsować payloady `NODE_*`.

## Co jest STUB / TODO

- **SPI → CC1310**: `nodeTxSink` tylko loguje; `gNodeInQueue` nikt nie zasila.
  Realne: SPI task (master + 2-linie handshake, ARCHITECTURE-GEN2 §3) →
  `gNodeInQueue` (dane nodu) i drenaż akcji do nodów (`qNodeOut`). Odblokowuje
  realne reguły danych + uogólnione sprzężenie zwrotne dla akcji stanowych.
- **Źródło czasu**: `MSG_TIME_SYNC` (h,m,s) → `engine_set_time` działa. Produkcyjnie:
  okresowy re-sync z NTP Linuxa (korekta dryfu) lub RTC carrier — §9 architektury.

## Testowalny milestone DZIŚ (bez SPI/CC1310)

1. CCS: projekt FreeRTOS z powyższymi plikami → build → `Deploy-M4F`.
   M4F log po starcie: `Engine initialized (0 rules); engine task spawned` +
   `Engine task started`.
2. Linux: `Deploy-Go -Build` (z `automation.h` w kopiowaniu!) →
   `rpmsg-service -test push-rules`.
3. Oczekiwane: każda `RULE_*` z ACK; `RULE_COMMIT` → atomic swap → ACK;
   `[rules] pushed 3 rules ... M4F committed`. `m4f-watch` pokazuje dispatch.
   - Firing reguł `COND_TIME`: `rpmsg-service -test fire-smoke` — time-sync
     12:00:50 + reguła w oknie 10–14h → pierwsze `RULE_FIRED` ~10s (wyrównanie
     do `:00`), potem co minutę. Reguły danych czekają na SPI (`gNodeInQueue`).
