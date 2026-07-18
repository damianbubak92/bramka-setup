# CLAUDE.md — Operating Manual dla Claude Code

> **Ten plik jest czytany automatycznie przez Claude Code na początku każdej sesji.**
> Zawiera kontekst projektu, konwencje, aktualny stan i pending tasks.
> Aktualizuj go po każdej istotnej zmianie — to "shared memory" między sesjami.

## Project Overview

**Bramka IoT** — profesjonalna bramka dla systemu smart-home/PV/automatyki, sprzedawana setki/tysiące sztuk w PL→UE.

**Dev kit**: SK-AM62B-P1 (Texas Instruments, AM62 SoC, HS-FS silicon, $260)
**Target production**: Toradex Verdin AM62 (SoM) + custom carrier board z Microchip KSZ9897 switch, M.2 NVMe, M.2 modem LTE/5G

### Architektura SoC (AMP — Asymmetric Multi-Processing)

- **A53 quad-core** → Linux (Arago 2025.01, kernel 6.18.13-ti) — services, automation, UI, cloud
- **M4F (Cortex-M4F)** → bare-metal MCU+ SDK 12.00 — real-time control, safety-critical
- **Komunikacja Linux↔M4F**: RPMsg (kernel rpmsg framework) z binarnym protokołem
- **Filozofia**: M4F i A53 to **partnerzy, nie rodzic/dziecko**. M4F bootuje niezależnie z OSPI flash (production), Linux z eMMC.

### Repo struktura

> **MONOREPO `C:\SmartHome`** (zmigrowane z `bramka-setup` 2026-06-29). Ścieżki w
> starszych wpisach **Session Log** odnoszą się do układu **sprzed migracji**
> (mapowanie „było X" niżej). Projekty CCS/Android są teraz **w repo** (nie mirrory);
> nagłówki protokołu mają jedno źródło w `Shared/Protocol/` (CCS include-path tam celuje).

```
SmartHome/
├── README.md / CLAUDE.md                       # docs (TEN PLIK)
├── Gateway/                                     # bramka: A53 Linux + M4F + CC1310 RF
│   ├── Software/rpmsg-service/                  # Go: RPMsg bridge + HTTP/WS API (cgo)   [było go-services/]
│   ├── Firmware/
│   │   ├── M4F/     → projekt CCS gateway_m4f      # engine + RPMsg + SPI master         [było m4f-firmware/]
│   │   └── CC1310/  → projekt CCS gateway_cc1310   # concentrator RF + SPI slave         [było cc1310-firmware/]
│   ├── Setup/                                   # setup.sh, modules/, systemd/, tools/, config.sh   [było root repo]
│   └── Hardware/                                # carrier board (TODO)
├── Nodes/
│   ├── TempHumNode/
│   │   ├── Firmware/  → projekt CCS temphum_node   # CC1310+SHT35 (+BQ35100 rev1)        [było cc1310-th-hw-firmware/]
│   │   └── Hardware/                            # KiCad rev2 (TODO)
│   ├── LightSwitchNode/, SolarControllerNode/   # szkielety (przyszłe nody)
├── Apps/MobileApp/
│   ├── SmartHome/                            # ⭐ AKTUALNA apka: KMP + Compose Multiplatform
│   │   │                                     #   com.aitronic.smarthome; :shared (commonMain UI+logika
│   │   │                                     #   / androidMain / iosMain) + :androidApp + :iosApp
│   │   └── (iOS: target jest, build wymaga Maca)   [[smarthome-app-kmp]]
│   ├── design_handoff_smart_home/            # źródło prawdy designu (prototyp HTML + tokeny)
│   └── AndroidApp/SmartHomeV2/               # STARA apka (com.example.smarthomev2) — tylko referencja
├── Shared/
│   ├── Protocol/                                # node_protocol.h, protocol.h, spi_frame.h, automation.h — SINGLE SOURCE   [było shared/]
│   └── KiCadLib/                                # wspólne symbole/footprinty (TODO)
└── Docs/                                        # [było docs/]
```
Setup bramki (`Gateway/Setup/`): `modules/01-network..09-timezone.sh`, `systemd/rpmsg-service.service`, `tools/m4f-watch`+`m4f-reload`, `setup.sh`, `config.sh`. Symulowany node (provisioning-referencja) jest off-repo / w git-history skasowanego `cc1310-node-th-firmware` — patrz [[node-provisioning-reference]].

## Tech Stack & Conventions

### Languages
- **C** dla M4F firmware (MCU+ SDK 12.00, TI ARM Clang toolchain)
- **Go 1.23+** dla Linux services
- **Bash** dla setup scripts (POSIX-compatible gdzie możliwe, bashism OK gdy potrzebne)
- **PowerShell** dla deployment helpers na laptopie deweloperskim

### Code style
- C: K&R braces, 4-space indent, snake_case dla funkcji
- Go: standard `gofmt`, camelCase, no globals jeśli można uniknąć
- Bash: `set -e`, `set -u`, idempotent (re-run safe)
- Komentarze: **angielski** w kodzie produkcyjnym, **polski** w komentarzach narzędziowych/setup
- Logi: prefix `[Module]` lub `[Test]` dla łatwego grep, czas mikrosekundowy w Go

### Git workflow
- **Branch model**: trunk-based (main only), feature dev w lokalnym brancie krótko, merge szybko
- **Commit messages**: imperative mood, krótki opis czego (nie jak)
- **Pliki**: `*.sh` ZAWSZE LF (enforce via `.gitattributes`)
- **Permissions**: `git update-index --chmod=+x` na bash scripts (Windows nie zapamiętuje)

### Deployment workflow (PowerShell helpers na laptopie)
- `Deploy-Go -ServiceName X -Build` — scp + build na bramce
- `Deploy-M4F` — scp custom firmware + m4f-reload (build w CCS Theia osobno)
- `Install-GoService -ServiceName X` — systemd unit deploy + daemon-reload + restart
- `Watch-M4F` — live trace
- `Connect-Bramka` — quick SSH
- `$BRAMKA_HOST = "root@192.168.2.170"`

## Communication Protocol (Linux ↔ M4F)

### Wire format (big-endian)
```
[ type:1B ][ seq:2B ][ payload_len:2B ][ payload:N bytes ][ CRC16-CCITT:2B ]
```

### Message types (z `shared/protocol.h`)
```c
#define MSG_HELLO         0x01u  // Connection request, payload = identifier string
#define MSG_HELLO_ACK     0x02u  // Reply with identifier
#define MSG_PING          0x03u  // Heartbeat probe (idle-triggered)
#define MSG_PONG          0x04u  // DEPRECATED — PING now uses MSG_ACK reply
#define MSG_ACK           0x11u  // Universal acknowledgment (for DATA, EVENT, PING)
#define MSG_DATA          0x10u  // Linux→M4F: control commands
#define MSG_EVENT         0x20u  // M4F→Linux: sensor data, alerts
#define MSG_DEBUG_CRASH   0xF0u  // DEBUG ONLY: force M4F hardfault
#define MSG_DEBUG_HANG    0xF1u  // DEBUG ONLY: force M4F infinite loop
```

### Reliability mechanism
- **ACK universal**: każda wiadomość requiring confirmation czeka na `MSG_ACK seq=N`
- **Retry**: 3× retransmisja przy 1s timeout = ~4s do giveup
- **Idempotency**: M4F sprawdza `seq` żeby odrzucić duplikaty po retry

### Heartbeat (smart, idle-triggered)
- **Direction**: TYLKO Linux→M4F (jednokierunkowy)
- **Trigger**: gdy Go nie widział żadnej wiadomości od M4F > 5s
- **Detection**: 5s idle + 4s retries = ~9s do wykrycia martwego M4F
- **Recovery**: Go wymusza `m4f-reload` (sysfs remoteproc stop+start), exit, systemd restart

## System Architecture — Recovery Flows

Cztery scenariusze awarii, każdy ma jasnego ownera detekcji i akcji:

| Scenariusz | Detection | Recovery action | Czas total |
|---|---|---|---|
| **M4F silent hang** | Go heartbeat (~8s) ✅ | clean `reboot` (`recoverByReboot`: sync + systemctl reboot) ✅ zweryfikowane 15.06.2026. Stary `remoteproc stop` wieszał SoC — usunięty. Backup: Warstwa D | ~70s |
| **M4F hardfault** | M4F custom hardfault handler | `SOC_generateSwWarmResetMcuDomain` (cały SoC reset) ✅ zweryfikowane 15.06.2026 | ~70s (pełny boot) |
| **Linux kernel panic** | systemd HW watchdog `/dev/watchdog0` (Warstwa D, `RuntimeWatchdogSec=30`) | HW reset całego SoC ✅ zweryfikowane 15.06.2026 (test z `panic=0` → tylko watchdog uratował) | ~70-90s |
| **Go service hang** | systemd software watchdog `WatchdogSec=10s` | SIGABRT + Restart=on-failure | ~12-15s |

### Asymetria heartbeat — DLACZEGO tylko Linux pinguje

- Go potrafi zrestartować M4F (sysfs remoteproc) → ma sens że pinguje i wykrywa
- M4F NIE potrafi zrestartować Linuxa (Warstwa C/DMSC reset to TODO i tak last resort)
- Linux watchdog kernela łapie Linux panic
- Systemd watchdog łapie Go hang
- M4F po SoC reset wstaje szybciej niż Linux → M4F **nie powinien** pingować przy starcie bo Linux jeszcze ładuje

## Critical Conventions (NEVER VIOLATE)

### Systemd units
- **NEVER** `Restart=always` — zawsze `Restart=on-failure`
- **ALWAYS** `StartLimitBurst=3` + `StartLimitIntervalSec=60` (safety net przed reboot loops) — **w sekcji `[Unit]`, nie `[Service]`** (nowoczesny systemd ignoruje je w `[Service]`; do 16.06.2026 były w złym miejscu i nie działały)
- **NEVER** crash tests w `ExecStart` (`-test crash-m4f`, `-test silent-hang`, `-test hang`)
- Production default: `-test hello` lub prawdziwy service mode
- Crash tests są **interactive only**: `ssh -t ... ad-hoc -test X`

### Filesystem & power
- Consumer SD cards (GOODRAM, ogólne) **NIE NADAJĄ się** do dev pracy z crash testami
- Production: industrial eMMC w Verdin + supercap PLP + PWR_FAIL GPIO na carrier board
- Dev: industrial SD (Samsung Pro Endurance, SanDisk Industrial XI)
- Po każdym milestone: backup karty `dd | xz -9 -T 0` (trzymaj 3 ostatnie generacje)

### M4F specifics
- **Per-core reset M4F NIE jest możliwy** na AM62 → hardfault używa `SOC_generateSwWarmResetMcuDomain` (cały SoC)
- M4F MUSI mieć custom hardfault handler — bez tego silent crash
- Firmware path: `/lib/firmware/ti-ipc/am62xx/ipc_echo_test_mcu2_0_release_strip.xer5f`
- `setup.sh module 03-m4f-firmware.sh` tylko BACKUPS default → trzeba osobno `Deploy-M4F`
- **`m4f-reload` zatrzymuje `rpmsg-service` przed `echo stop` M4F i restartuje po starcie (przez `trap EXIT`)** — bo P2 fast-fail: serwis widzący zniknięcie `/dev/rpmsg` robi natychmiastowy reboot bramki, co rozwalało deploy w trakcie. Restart tylko jeśli serwis był aktywny. Dotyczy też `Deploy-M4F` (woła `m4f-reload`) — helper PowerShell bez zmian. Po `git pull` na bramce: `cd Gateway/Setup && sudo ./setup.sh` regeneruje `m4f-reload`.

### Disaster recovery procedure
1. Świeży flash karty SD (Etcher na Win 10, sprawdzony workflow)
2. `git clone https://github.com/damianbubak92/bramka-setup` (repo wciąż nazwany `bramka-setup`; może zostać przemianowany na `SmartHome`)
3. `cd bramka-setup/Gateway/Setup && sudo ./setup.sh` (network + tools + M4F backup + Go install) — setup żyje teraz w `Gateway/Setup/`
4. `reboot`
5. Z laptopa: `Deploy-M4F` (custom firmware) + `Deploy-Go -Build` + `Install-GoService`
6. Plus DHCP reservation w routerze (MAC `22:F4:99:37:A5:12` → IP `192.168.2.170`)

## Current Status (UPDATE PO KAŻDEJ SESJI)

### ✅ Done
- Setup.sh działa po świeżym flash (network, tools, M4F backup, Go install, watchdog)
- Custom M4F firmware działa (deploy manual via Deploy-M4F; M4F NIE inicjuje heartbeatu — opcja A)
- Go service zbudowany i działa
- Systemd unit safe (`-test hello`, `Restart=on-failure`, `StartLimitBurst=3`)
- HELLO/HELLO_ACK handshake działa, heartbeat Linux→M4F tickuje (RTT 4-6ms)
- Watchdog systemd kickowany co 5s
- Repo z executable scripts + LF endings
- DHCP reservation w routerze
- **Priorytet 1 (15.06.2026, zweryfikowane na bramce)**: M4F heartbeat-init usunięty (opcja A) — M4F nie inicjuje PINGów, tylko odpowiada `sendAck()` na PING od Go (jednokierunkowy heartbeat Linux→M4F). `m4f-watch` potwierdza: zero `TX heartbeat PING`, jest `RX heartbeat PING - replying ACK`.
- **Priorytet 1 (15.06.2026, zweryfikowane)**: Go HELLO retry z exponential backoff (1/2/4/8s, 5 prób) w `helloWithRetry()` (`go-services/rpmsg-service/main.go`). Log startowy: `Sending HELLO (with retry)...`.
- **Bugfix (15.06.2026)**: M4F `case MSG_PING` odpowiadał deprecated `MSG_PONG` → zmienione na `sendAck()`. Go czeka na `MSG_ACK`, więc PONG groził fałszywym `PEER DEAD` → restart loop.
- `protocol.h` zsynchronizowane (`shared/` == `m4f-firmware/`).
- **Warstwa D w setup (15.06.2026, commit dabee5b)**: `modules/05-watchdog.sh` (idempotentny, `RuntimeWatchdogSec=30`) + posprzątany `setup.sh`. Zweryfikowane: `lsof /dev/watchdog0` → systemd (PID 1) trzyma device, `wdctl` busy = OK.
- **Cold-boot race fix (15.06.2026)**: `rpmsg-service` padał po reboocie (`OpenTransport: no rpmsg_chrdev`, race startowy → `StartLimitBurst`). Fix w `transport.go`: `waitForM4FChrdev` czeka na `/dev/rpmsg*` do 20s (margines pod `TimeoutStartSec=30s`). Zweryfikowane: serwis wstaje sam po reboocie.
- **P2 transport fast-fail (16.06.2026, zweryfikowane)**: device-gone (`Transport.DeviceGone()` → `deviceGoneWatcher` → `signalPeerDead`) → PEER DEAD natychmiast (~3ms zamiast ~9s heartbeat) → clean reboot. `findM4FChrdev` number-agnostic (skan po HW path).
- **m4f-reload service-aware (16.06.2026, zweryfikowane)**: `m4f-reload` zatrzymuje `rpmsg-service` przed `echo stop` M4F i restartuje po (trap EXIT) — deploy firmware bez przypadkowego reboota bramki (skutek uboczny P2 fast-fail).
- **EVENT/tick cleanup (16.06.2026, zweryfikowane)**: `doPeriodicTick` — zakomentowany log `Tick #` (spam) + testowy EVENT co 10s (scaffolding). `m4f-watch` czysty.
- **panic_on_oops=1 (16.06.2026, zweryfikowane)**: `modules/06-kernel-panic.sh` — oops → pełny panic → łapie Warstwa D. Domknięta luka „oops bez panic".
- **Boot accounting (16.06.2026, zweryfikowane)**: `modules/07-boot-accounting.sh` — licznik bootów + atrybucja + alarm reboot-storm (`/etc/bramka/boot-accounting.conf`, default >3/24h). Podgląd: `bramka-reboots`. Klasyfikacja na **trwałym markerze** `/var/lib/bramka/clean_shutdown` (NIE na journalu): breadcrumb→`CONTROLLED go`, marker→`CONTROLLED clean shutdown`, brak→`UNEXPECTED hard reset`. Zweryfikowane: go-peer-dead ✓, clean shutdown ✓. Persistent journal (diagnostyka) jako best-effort oneshot bind — działa (`journalctl -b -1` po reboocie).
- **Non-root hardening (16.06.2026, ZWERYFIKOWANE NA ŻYWO)**: `modules/08-hardening.sh` + przerobiony `rpmsg-service.service`. Serwis jako user `bramka` (nie root), zero capabilities. Device przez udev (grupa `bramka`), reboot przez wzorzec path-unit (`/run/bramka/reboot-request` → `bramka-reboot.path` → `bramka-reboot.service` robi czysty `systemctl reboot`; serwis nie ma roota ani CAP_SYS_BOOT). Binarka przeniesiona `/root/bramka-services` → `/opt/bramka` (Deploy-Go cel zmieniony). `StartLimit*` przeniesione do `[Unit]` (były ignorowane w `[Service]`). Go `recoverByReboot` pisze trigger (fallback systemctl/syscall dla root). **Weryfikacja**: `User=bramka` + proces uid `bramka` + `/dev/rpmsg0` grupa `bramka`; krok 7: `echo stop` → non-root serwis → trigger → path-unit → czysty reboot → `boot#1 CONTROLLED go-peer-dead`, serwis wraca jako `bramka`.
- **Faza 3 / telemetria → DB (21-23.06, zweryfikowane)**: telemetria nodów → SQLite (`node_param` stan bieżący +
  `solar_history` z dzienną akumulacją + VIEW `solar_state`), provisioning (JOIN/approve/remove) i **kanał WebSocket** (`/ws`).
- **HW: node T&H rev2 (12.07)**: schemat + PCB 4-warstwowe **wysłane do JLCPCB** (3 zmontowane prototypy, DHL DDP).
  Footprinty scalaków przepisane wg datasheetów (CC1310: pady 0.24 + roundrect 0.05 + windowpane pasty omijający via array;
  NPTH w USB). Pliki produkcyjne w repo. Protoypy w drodze → bring-up + kalibracja LUT SoC. [[rev2-th-node-protos-ordered]]
- **Apka `SmartHome` — pełne UI (16.07)**: KMP + Compose Multiplatform (`com.aitronic.smarthome`), 6 ekranów 1:1 z designu
  (Dashboard, Klimat, Solar ze schematem instalacji i animowanymi pompami, Automatyzacje + edytor, Urządzenia).
  Architektura warstwowa (`data`/`domain`/`ui`), ikony jako `ImageVector` z realnych ścieżek SVG, wykresy na `Canvas`.
  iOS: target gotowy (build wymaga Maca). [[smarthome-app-kmp]]
- **Apka ŻYJE z bramką — Stage 2 (16-17.07, ZWERYFIKOWANE NA ŻYWO)**: Ktor + pinning (`expect/actual`), kaskada LAN→zdalnie,
  WS z reconnectem, `GatewayStore` (zasiew z bazy + live, zero pollingu). **Solar live**, **sterowanie pompą z potwierdzeniem
  z noda**, **reguły z/do bramki**, **urządzenia z `listnodes`** (nazwa+pokój trwałe). Nowe komendy Go: `state`, `updatenode`;
  migracja `node.room`.
- **Sniff gen1 (17.07, pomysł usera)**: gen2 zbiera telemetrię nodów adresowanych do gen1 (`0xF0`) **bez ACK-owania** —
  solar/bufor działają w apce **bez ruszania firmware nodów**, gen1 niezakłócona. Rozwiązanie tymczasowe do czasu reflashu.

### 🔜 NASTĘPNA SESJA — zacznij tu

**STAN (17.07): apka `SmartHome` (KMP + Compose Multiplatform) STERUJE REALNĄ INSTALACJĄ.**
Pętla live domknięta w obie strony: `node → RF → CC1310 → SPI → M4F → RPMsg → Go → SQLite → WS → apka`
oraz `apka → HTTPS :9443 → Go → M4F → SPI → CC1310 → RF → node` (z potwierdzeniem z noda).
Apka: [[smarthome-app-kmp]] · kontrakt HTTP/WS: [[remote-access-contract]] · design: `Apps/MobileApp/design_handoff_smart_home/`.

**Co realnie DZIAŁA (zweryfikowane na żywo):**
- **Sniff gen1 (tymczasowy, pomysł usera)** — `radio_task.c`: gen2 zbiera ramki adresowane do gen1 (`0xF0`)
  i **NIGDY ich nie ACK-uje** (`GEN1_CONCENTRATOR_ADDRESS`). Dzięki temu mamy telemetrię solar (241) i bufor (242)
  **bez ruszania firmware nodów**; gen1 działa niezakłócona. **Gen1 MUSI zostać włączona** — to jej ACK-i trzymają nody.
  Docelowo: reflash nodów na gen2 (`0x00`) + provisioning → sniff wypada.
- **Solar live**: temperatury, `flowRate` (pompa obiegowa read-only, kręci się gdy >0), uzysk, **moc od razu po otwarciu**
  (z VIEW `solar_state`, nie po 2 min).
- **Pompa dodatkowa**: toggle → realne `PUMP_ON/OFF`; **trójkąt rusza dopiero po potwierdzeniu z noda** (`pumpState`),
  spinner w zarezerwowanym miejscu, timeout 6s → powrót toggle. Zero pollingu (WS).
- **Reguły**: `getrules`/`setrules` z bramki; katalog **przycięty do możliwości silnika** (warunki: solar `T1-T4` + bufor
  `sBuforTemp`; akcja: **tylko solar → przekaźnik ON/OFF** = pompa dodatkowa). Design apki był robiony na przyszłość —
  `pv`/`climate`/`Tcol`/`moc` wrócą, gdy rozszerzymy protokół.
- **Urządzenia**: lista z `listnodes` (tabela `node`), online z `last_seen`, **nazwa+pokój trwałe** (`updatenode`),
  usuwanie przez `removenode` (graceful). JOIN jest realny (node zgłasza się sam → `join_pending` po WS).

**Nowe komendy HTTP dodane 16-17.07:** `state` (ostatnia telemetria z `node_param` + `powerKw` z VIEW),
`updatenode` (nazwa+pokój). Reszta kontraktu bez zmian.

**✅ ZROBIONE 17.07 (druga część dnia) — DB monitor + sprzątanie bazy + historia solara + 2 fixy niezawodności:**
- **DB monitor `/db`** (dev-only, flaga `-db-monitor` default ON): podgląd całej bazy na żywo + konsola SQL + dziennik
  zdarzeń. Dziennik z **update-hooka SQLite** (`dbmonitor.go`, driver `sqlite3_hooked`) — łapie KAŻDY insert/update/delete
  na każdej tabeli (też przyszłych), nieblokująco. UI dwa taby: „Na żywo" (auto-refresh) + „Zapytania" (telemetria NIE
  przykrywa wyników; przycisk Kopiuj → markdown). `Deploy-Go` kopiuje teraz `*.html` (go:embed). URL: `/db?token=...`.
- **Baza wysprzątana na żywo**: duplikaty `solar_history` skasowane (były bursty PO hangu M4F — CC1310 retransmituje
  w górę, po recoverym backlog wchodzi naraz; noc = `delta 0`, uzysk NIE zawyżony), `energy_gain`/`pump_runtime`
  **przeliczone od zera** per lokalny dzień (`SUM(delta)==max(energy_gain)` ✓), sieroty 17/18/19 usunięte,
  `sample` DROP. Guard w Go: `recordSolarHistory` odrzuca pełny odczyt <60s od poprzedniego (`solarMinIntervalS`) +
  `UNIQUE(node_id,reading_time)` **nie-fatalnie** (brudna baza nie blokuje startu).
- **Historia solara — PRAWDZIWE WYKRESY + live-refresh na WS** (apka: wykresy z bramki, ostatni słupek rośnie sam co 2 min,
  przeglądanie starszych okresów zachowane). `command=history&range=day|month|year|total` zwraca liczby + etykietę PL.
- **🔑 REDESIGN HISTORII na model gen1 (18.07, `solaragg.go`) — zastąpił `solar_rollup`**: user słusznie zganił mój
  compute-on-read (zapytania 8s na 500k wierszy). Teraz **3 tabele agregatów per node** `solar_hourly`/`solar_daily`/
  `solar_monthly` (uzysk w **kWh** REAL, pompa w min), model 1:1 z gen1 (`SolarSystem{Daily,Monthly,Annual}Stats`).
  `solar_history` = **bufor 2h** (kumulatyw `energy_gain`); agregacja **diff kumulatywu na granicach godzin** (`hour_yield =
  max(0, kum − poprz_kum)`), **dzienny total = `SUM(hour_yield)`** (odporne na reset gen1 wpadający w środek mojej doby —
  offset strefy ~1h). **Event-driven na ingest po commit** (błąd agregacji nie cofa surówki), retencja drop >2h po udanej
  agregacji. Total z `solar_monthly`. Kompaktowość: `DeleteNode`→`dropSolarNode` (kasuje surówkę+3 agregaty). Zapytania w ms.
  **NIE różnicujemy delt per odczyt** (to dawało bug 300 kWh). [[solar-aggregation-model]]
- **🔑 SYNC GEN1 — 2 LATA HISTORII WCIĄGNIĘTE (18.07, zweryfikowane co do grosza)**: `gen1import.go` + PHP endpoint
  `Gateway/Software/server/solar-export.php` (read-only JSON, klucz, sekrety w gitignorowanym `secrets.php` — realne creds tylko na
  serwerze). `-test import-gen1 -gen1-url ... -gen1-key ... [-gen1-insecure]` (cert hosta nie pasuje → HTTP albo insecure).
  **Pełny recompute**: czyść surówkę noda → wciągnij całość (extraTemp→`energy_gain`, pumpRuntime→`pump_runtime` verbatim,
  BEZ rekonstrukcji delt) → `RebuildSolarAggregates` → drop surówki. Idempotentne, dev-only. Import PRZED serve (robi migrację
  + build agregatów). **Weryfikacja**: wszystkie dni zgadzają się z gen1 co do grosza; gdzie się różnią — to gen1 gubił dni
  (forward-only cron: przegapiony reset → `hourYield` sklamrowane do 0, np. 06-20 = 20 kWh w rawie ale 0 w monthly). **Nasza
  metoda jest odporniejsza i poprawniejsza niż gen1.** [[gen1-server-scripts]]
- **FIX pompy (18.07)**: `pump_runtime +2 min gdy energyGain z noda ≠ 0` (nie flow>0) — reguła gen1 (pompa może stać przy
  resztkowym uzysku).
- **🔑 FIX MOCY (apka)**: `energyGain` w telemetrii to **surowy przyrost 2-min**, NIE narastający. Apka różnicowała dwa
  kolejne przyrosty → szum „0,12 kW". Teraz `moc = 30*energyGain/10000` wprost (jak VIEW). Uzysk dzienny z bramki
  (`energyDayKwh` z VIEW `solar_state` — jeden przyrost nie da doby).
- **🔑 FIX PROTOKOŁU `protocol.go` (commit 6577cd1, osobny)**: po hangu/reconnekcie M4F telemetria była **dropowana ~20 min**.
  `HELLO_ACK` resetował `theirLastSeq=0`, ale zaległa ramka tuż po resecie wbijała go wysoko, a świeża sesja M4F (seq od 1)
  leciała pod próg jako „duplikaty". Retransmisja sięga max `MAX_PENDING_ACKS`(8) wstecz → skok wstecz >16 = nowa sesja/wrap,
  nie duplikat (`isDuplicateSeq`, `seqResyncWindow=16`). **To maskowało hangi M4F jako dłuższe** — prawdopodobnie
  długoletni bug. Zweryfikowane: po reconnekcie seq=1 zapisuje się od razu. [[m4f-seq-resync-on-reconnect]]

- **Wykresy solar — dopięte (18.07 wieczór, commit `09bedc7`)**: strzałki przeglądają **całą historię** (`command=history`
  bez `count` → wszystkie okresy z danymi, liczone z `solarSpan`; blokada na realnym początku). Live-refresh **tani** — apka
  dociąga tylko bieżący okres (`count=1`), pełną listę raz na zmianę zakresu. **Czas pracy pompy działa** — trzeba było
  dodać `pumpRuntime` do `solar-export.php` na serwerze (był wgrany bez niego) + re-import. `solar-export.php` w repo
  **zsanityzowany** (placeholdery — realne creds tylko na hostingu; gdyby user chciał lokalną kopię z creds → poza repo / gitignore).

- **🔑 LIVE BACKUP + RESTORE + wzorzec sekretów (18.07 wieczór, ZWERYFIKOWANE NA ŻYWO)**: bramka pushuje cały durable-state
  na zewn. mirror (osobna baza gen2 `baza23202_smarthome`). **Mechanizm**: triggery SQLite na durable tabelach → `backup_queue`
  → worker co 15s dosyła do `gw-backup.php` z retry (offline → trzyma i dosyła gdy serwer wróci). Seed pełnej bazy raz (marker
  `backup_seeded`). Probe `json_object` (bez JSON1 backup off, nie psuje zapisów). Flagi `-backup-url`/`-backup-key`. **Restore**
  (`-test restore -restore-url gw-restore.php`) — świeża bramka odtwarza się z mirrora (zweryfikowane: 17483 wierszy → config/
  nody/stan/historia). **Wymiana noda** (`replacenode&factory=<new>&target=<addr>`): nowy chip przejmuje STARY adres, historia
  (kluczowana po `node_id`=adres) zostaje przypięta automatycznie (`ReplaceNode` + `dropSolarNode` na removie). Kod gotowy,
  testnięcie z realnym chipem odłożone. [[gen2-backup-mirror]]
- **🔑 WZORZEC SEKRETÓW (18.07)**: serwer skonsolidowany do **`Gateway/Software/server/`** (jeden katalog, bo na hostingu i tak
  razem). Wszystkie skrypty PHP `require secrets.php` (**gitignored**) — jeden plik z creds do OBU baz (`$GEN1_*`/`$EXPORT_KEY`
  dla importu + `$GW_*`/`$BACKUP_KEY` dla mirrora). Repo ma `secrets.example.php` (szablon) + `.htaccess` (`Require all denied`
  na secrets.php, `-Indexes`). Koniec ręcznego sanityzowania przy commitach. Stare `server-gen1`/`server-gen2` scalone.

**👉 PLAN NA NASTĘPNĄ SESJĘ:**
1. **Test wymiany noda** z realnym nowym chipem (`replacenode` — kod gotowy).
2. **Apka: read-from-mirror gdy bramka nieosiągalna** — kaskada LAN→zdalnie→mirror (apka ma już abstrakcję; `gw-restore.php`/
   dedykowane read-endpointy z mirrora zwracają te same kształty co bramka). [[gen2-backup-mirror]]
3. **Dopracowanie JOIN**: generowanie/usuwanie tabel per node przy provisioningu (agregaty solar mają to przez `dropSolarNode`).
4. **Dopracowanie automatyzacji.**
5. Produkcja: przenieść `-backup-url`/`-backup-key` do systemd unitu; usunąć sekcję gen1 z `secrets.php`.
5. **Hang M4F** (znany, ~raz/kilka h, koreluje z ruchem SPI — sniff gen1 go nasilił). Recovery działa. Diagnoza czeka na
   zrzut rejestrów hardfaultu (retained-RAM). User: „niebawem, ale najpierw plan".

**Nadal ATRAPY w apce (świadomie):** Klimat (wykresy + interwał), PV, Dashboard poza kaflem solarnym.
**Luki bramki blokujące:** brak komendy interwału pomiaru, brak noda/telemetrii PV, `reading_time`=czas ODBIORU nie pomiaru
(po hangu backlog dostaje zły stempel — wymaga timestampu w protokole noda; wróci przy klimacie).
**Nod T&H (rev2)** — protoypy w drodze; gdy dojdzie: rozszerzenie protokołu o `climate` → wykresy klimatu + interwał.

**Odłożone long-term:**
- **Warstwa C (DMSC reset)** — „prawdopodobnie tak, później" (M4F trzyma żywe sterowanie → crash Linuxa nie może go
  zabić na ~70s). OTA (RAUC A/B). Health monitoring (eMMC wear → wpina się w `bramka-reboots`/alarm). Carrier board.
- **Zewnętrzny hosting** (backup/relay) — architektura ustalona: bramka pushuje, kaskada
  **LAN → port-forward (dziś) / relay na VPS (docelowo) → mirror MySQL tylko gdy bramka nieosiągalna**; 3 pakiety
  (economy/standard/premium). Apka ma już abstrakcję (`SmartHomeRepository` + `GatewayClient`).
- **iOS**: target jest w projekcie, **pinning w `HttpClientFactory.ios.kt` to TODO** — nie wypuszczać bez niego.

**Opcjonalne domknięcie testów boot-accounting** (mechanizm ten sam, nie-zweryfikowane na żywo): klasyfikacja kernel-panic i clean-shutdown (manual reboot), realne odpalenie alarmu >3/24h.

### ✅ DONE — Recovery fix + crash testy (15.06.2026)
- **Recovery silent-hang**: `forceM4FReload` (remoteproc stop, wieszał SoC) → `recoverByReboot()` (`syscall.Sync()` + `systemctl reboot`, fallback kernel reboot, last resort Warstwa D). Zasada: zawsze clean reboot na PEER DEAD. Commit cb61155.
- **Warstwa D** (HW watchdog): `modules/05-watchdog.sh` (`RuntimeWatchdogSec=30`) — była zgubiona przy re-flashu, przywrócona. `lsof /dev/watchdog0` → systemd.
- **README/docs uspójnione**, `system/configure-watchdog.sh` usunięty (redundantny z modułem 05).
- **4 crash testy PASS** (interaktywnie, ad-hoc):
  1. `heartbeat-busy` — zero PINGów przy busy traffic (regresja opcji A)
  2. `silent-hang` — PEER DEAD 7.95s → clean reboot → auto-recovery ~70s
  3. `crash-m4f` — hardfault → SOC reset → auto-recovery ~70s, bez korupcji FS
  4. `kernel panic` (`echo c`, `panic=0`) — HW watchdog (Warstwa D) zresetował SoC → auto-recovery
- Pattern crash testów (NIGDY pod systemd):
  ```powershell
  ssh root@bramka "systemctl stop rpmsg-service"
  ssh -t root@bramka "/opt/bramka/rpmsg-service/rpmsg-service -test <mode>"
  # reset-testy (silent-hang/crash-m4f) rebootują bramkę; przedtem: ssh ... "sync"
  ```

### ⏳ Pending — Priorytet 3 (nice-to-have)
> Cały P3 ZROBIONY 16.06.2026 (EVENT cleanup, panic_on_oops, persistent restart counter, non-root hardening). Szczegóły w Session Log. Non-root: zamiast „m4f-reload.service" wyszedł wzorzec path-unit dla reboota (`modules/08-hardening.sh`) — czeka na Deploy-Go (ścieżka `/opt/bramka`) + Install-GoService + restart.

### ⏳ Pending — Long-term (poza obecnym sprintem)
- **Warstwa C (DMSC reset)**: M4F triggeruje DMSC reset tylko A53 cluster (Linux), bez resetu siebie. Wymaga TI-SCI API research w MCU+ SDK 12.00 (`Sciclient_procBootRequestProcessor` + reset sequence dla TISCI_DEV_A53SS0_0..3). Fallback po 30s: pełny SoC reset.
- **OTA updates**: RAUC dla Linux (A/B partitions na eMMC), custom dla M4F (A/B w OSPI + ECDSA P-256 signing)
- **Bazy**: SQLite (config, na eMMC) + InfluxDB/TimescaleDB (telemetria, na M.2 NVMe industrial)
- **Health monitoring service**: eMMC wear `/sys/block/mmcblk0/device/life_time`, alarmy gdy zużycie >70%
- **Carrier board production**: supercap PLP + PWR_FAIL GPIO + kernel sync on signal + **RTC z podtrzymaniem (MUST-HAVE)** — silnik pracuje na wstrzykniętym czasie; bez netu (NTP down) RTC to jedyne źródło czasu offline, inaczej reguły `COND_TIME` martwe (fail-safe). Patrz [[rtc-must-have-carrier]].

## Working Style (dla Claude Code)

### Co robić proaktywnie
- **Read CLAUDE.md + README.md na start każdej sesji** — kontekst projektu
- **Sprawdź `git log --oneline -10` na start** — co już zrobione od ostatniej sesji
- **Zaproponuj plan przed dużymi zmianami** — multi-file refactor, deletion, restructure
- **Run `go build` / `bash -n` po zmianach** — sanity check że nic nie zepsute
- **Update Current Status w CLAUDE.md na koniec sesji** — co zrobione, co dalej

### Czego NIE robić
- **NIE commituj automatycznie** — pokaż diff, czekaj na "tak commituj"
- **NIE push do main bez potwierdzenia** — main jest production
- **NIE modyfikuj `protocol.h` zmieniając wartości MSG_*** — kompatybilność wsteczna, M4F i Go muszą się zgadzać
- **NIE używaj `Restart=always`** w żadnym systemd unit — to MUSI być `on-failure` + burst limit
- **NIE dodawaj crash testów do systemd ExecStart** — interaktywne only

### Communication style
- **Polski** w odpowiedziach (chyba że pytanie po angielsku)
- **Concise** — nie tłumacz w kółko tego co user już wie (15+ lat embedded)
- **Konkretne diffy zamiast prozaicznych opisów** — pokazuj exact code
- **Verify, then act** — czytaj plik zanim modyfikujesz

## Hardware Context (dla debug zagadek)

- **SK-AM62B-P1 silicon**: AM62X SR1.0 **HS-FS** (High Security Field Securable)
  - NIE robić podmiany `tiboot3.bin` mimo instrukcji TI Quick Start — default HS-FS jest właściwy
  - JTAG debug M4F: secure debug authentication error -1274 (workaround: UART debug + OSPI flash)
- **Network**: eth0 (WAN, do routera), eth1 (LAN, do nodów switch) — production: KSZ9897 7-port
- **M4F MAC**: `22:F4:99:37:A5:12` (locally administered, bit 2 in first byte)
- **Hostname**: `bramka-01`
- **IP**: `192.168.2.170` (DHCP reservation w routerze)
- **Karta SD**: GOODRAM 64GB Class 10 UHS-I — consumer-grade. Decyzja (16.06.2026): NIE kupujemy industrial SD — produkcja na eMMC (Verdin), dev na zapasowych kartach consumer (uszkodzona = re-flash).

## Useful Commands Cheatsheet

```bash
# M4F live trace:
m4f-watch

# Restart M4F firmware (bez reboot bramki):
m4f-reload

# Sprawdź state M4F:
cat /sys/class/remoteproc/remoteproc0/state    # running / offline / crashed
cat /sys/class/remoteproc/remoteproc0/firmware # aktualnie załadowany firmware

# Service status:
systemctl status rpmsg-service
journalctl -u rpmsg-service -f
journalctl -u rpmsg-service -n 50 --no-pager

# Manual stop/start service:
systemctl stop rpmsg-service
systemctl start rpmsg-service

# /dev/rpmsg detection:
ls /dev/rpmsg*  # M4F zwykle na /dev/rpmsg2 ale numer może się zmienić

# Network info:
ip link show eth1
cat /sys/class/net/eth1/addr_assign_type  # 3 = SET (good), 1 = RANDOM (bad)

# Reboot accounting (licznik + przyczyny + alarm):
bramka-reboots                            # status: licznik, historia, alarm
cat /var/lib/bramka/boot_history.log      # pełny ledger
journalctl -t bramka-boot                 # wpisy/alarmy w journalu
$EDITOR /etc/bramka/boot-accounting.conf  # próg/okno/wyłączenie alarmu
```

## Session Log (NEWEST FIRST)

> Format: data — co zrobione, ważne decyzje, lessons learned

### 2026-07-16/17 — apka SmartHome ŻYJE: Stage 2 (sieć+pinning+live solar+sterowanie) + sniff gen1 ✅
- **Apka steruje realną instalacją** (commity `52a54a0`, `ee56054`). Stack: Ktor 3.3.1 + kotlinx.serialization +
  Multiplatform Settings; **pinning przez `expect/actual`** (Android: OkHttp TrustManager = port `CertPin.java`;
  iOS: TODO). `GatewayClient` (kontrakt 1:1 z `httpapi.go`, kaskada **LAN→zdalnie** z zapamiętanym base, WS z reconnectem),
  `GatewayStore` (jeden `StateFlow`: nody/joins/telemetria; **zasiew z bazy przy otwarciu**, potem live po WS, zero pollingu).
- **🔑 SNIFF GEN1 (pomysł usera, kluczowy odblokowywacz)**: gen2 podsłuchuje ramki do `0xF0` i **nie ACK-uje ich**
  (`GEN1_CONCENTRATOR_ADDRESS` w `radio_task.c`). Filtr RX jest **programowy** (radio i tak odbiera wszystko), więc podsłuch
  **nic nie kosztuje na antenie**, a gen1 ackuje normalnie → zero kolizji. Efekt: telemetria solar/bufor **bez ruszania
  firmware nodów**. Tymczasowe — docelowo reflash nodów na `0x00`. Retransmisje (rzadkie) mogą zawyżyć uzysk — świadomie odpuszczone na testy.
- **Nowe komendy Go**: `state` (ostatnia telemetria z `node_param` + `powerKw` z VIEW `solar_state`), `updatenode` (nazwa+pokój).
  Migracja `node.room`. Nody 241/242 zarejestrowane ręcznie (**`UPDATE`, nie `INSERT OR REPLACE`** — wiersze już istniały
  przez `RecordTelemetry`, replace skasowałby `last_seen`).
- **🔑 LEKCJE (każda kosztowała realny debug):**
  1. **`engine.c`: `OP_MORE_THAN` czyta `thresholdMin` (`mn`), `OP_LESS_THAN` czyta `thresholdMax` (`mx`)**.
     Wpisanie wartości w złe pole → reguła się zapisuje, wysyła i **NIGDY nie odpala**. Kodek (`RulesCodec.kt`) to uwzględnia.
  2. **`node_type 0` TO sterownik solarny** → `COALESCE(node_type, 0)` sprawiał, że **osierocone wiersze `node_param`
     udawały solar** (w bazie realnie siedzą sieroty 17/18/19!). Teraz `-1` = nieznany.
  3. **Wybór noda `firstOrNull{type}`** brał pierwszego z mapy (kolejność = rosnące `node_id`) → **stary nod wygrywał
     z żywym 241**: UI pokazywał zera i **ignorował telemetrię z WS**. Teraz **najświeższy** (`maxByOrNull{ts}`).
  4. **Telemetria „pump-only"** (`SEND_PUMP_STATUS`) niesie **sam `pumpState`** → podmiana całej mapy parametrów kasowała
     temperatury. Teraz **scalanie** (`old + new`), a baza delty `energyGain` przesuwa się **tylko przy nowym `energyGain`**
     (inaczej pump-only zerowałby moc).
  5. **Fallback na dane przykładowe kłamał**: brak telemetrii → `repo.solar()` miał `pumpOn=true` → **trójkąt kręcił się zawsze**.
     Zasada: **brak danych = `—` i pompy stoją**, nigdy atrapa (dot. też „0%").
  6. **Spinner nie może rozpychać layoutu** — status pompy ma zarezerwowaną stałą wysokość.
- **Pułapka pamięci**: skopiowałem firmware do `workspace_ccstheia` wg pamięci **sprzed migracji monorepo**. Po migracji
  **projekty CCS są W REPO** (`Gateway/Firmware/CC1310` ma `.project`) — stare `workspace_*` to leftovery. Pamięć poprawiona
  ([[cc1310-ccs-project-paths]]).
- **Audyt bazy (17.07)**: `node`(5) `node_param`(26, w tym sieroty 17/18/19) `param_def`(16) `solar_history`(~177, 21.06→17.07,
  **duplikaty `reading_time`**) `config`(2: rules + legacy addr_watermark) `sample`(511, **MARTWA → DROP**) + VIEW `solar_state`.

### 2026-06-27 — DECYZJA: architektura zasilania rev-2 (indoor PV + LiFePO4 + voltage-LUT; drop BQ35100/TPL5111)
- **Kontekst**: bateryjny T&H node rev-1 (CR123A + BQ35100 + TPL5111) działa E2E (telemetria temp/hum/napięcie), ale BQ35100 to ślepa uliczka do SoC/pomiaru zużycia.
- **BQ35100 — PORZUCONY.** (a) Nie rozróżnia naszego malutkiego impulsowego prądu: w trybie ACC `AccumulatedCapacity` zamarzał (~3 µAh), `Voltage()` pinował się (2797 mV) = gauge nie próbkuje aktywnie; nieskalibrowany CC-offset + kadencja próbkowania zjadają sub-µAh/cykl. (b) Dla ogniwa pierwotnego SoH/coulomb i tak obarczone błędem → zero wartości dodanej. **Lekcja: BQ to zły instrument na ten pomiar; per-cykl → PPK2 (kupowany) + szacunek analityczny.** Bug po drodze naprawiony (per-cykl GAUGE_START/STOP zerował akumulator → fix: jedna ciągła sesja), ale i tak za grube.
- **Pobór noda (zbieżne z 2 źródeł)**: ~**0.3 µAh/cykl → ~0.14 mAh/dobę → ~6 µA średnio**. Node = ultra-low-power → harvest + mały bufor z dużym naddatkiem.
- **Architektura rev-2 (z userem):**
  - **Filozofia „Casio": indoor PV harvest + bufor** (nie „bateria do ładowania"); USB tylko rzadki backup/serwis.
  - **Chemia = LiFePO4** (bezpieczna, długie cykle, nowsza tech, indoor=brak cold-charge, łagodne ładowanie pod prosty harvester). **Odrzucone:** superkap 100F/2.7V (~26 mAh + leakage → tylko ~tygodnie ciemności, nie miesiące); Li-ion (lepszy voltage-SoC, gorsze bezpieczeństwo/cykle); **Na-ion** (najnowszy, ale brak małych ogniw IoT-format + niedojrzały ekosystem ładowania + zaleta niskotemp. zmarnowana indoor → **watchlist, nie teraz**).
  - **Łańcuch:** `indoor PV → BQ25570 (MPPT, boost-charger, VBAT_OV≈3.6V) → LFP 100-300 mAh → CC1310 (+SHT35)`. CC1310 **wprost z LFP** (2.5-3.6V mieści się w 1.8-3.8V) → buck zbędny. `VBAT_OK`/charge-status → GPIO.
  - **Drop TPL5111 + BQ35100.** Zamiast power-cycle: **CC1310 STANDBY + wybudzenie RTC** (~0.6-1 µA, harvest pokrywa) → RAM się trzyma (LUT/historia kierunku), prościej, bez zimnego bootu.
  - **SoC po NAPIĘCIU (bez gauge'a):** ADC LFP **w stałym quiescent point** + oversampling → **LUT z 2 gałęziami (charge/discharge)** wybieranymi wg charge-status (łagodzi histerezę LFP ~20-40 mV). Dokładnie na końcach, grubo na plateau — wystarcza na „pełna/spada/wymień". Indoor=stabilna temp pomaga. (User słusznie: płaska krzywa ≠ brak SoC; precyzyjny pomiar + model to da.)
  - **Sizing:** LFP 100-300 mAh + samorozładowanie ≈ **miesiące-rok ciemności**; mały panel indoor offsetuje ~0.5 mAh/dobę.
- **DOPRECYZOWANE (27.06, wieczór):**
  - **Ogniwo = IFR 18350 LiFePO4 ~700-800 mAh** (ten sam wymiar 18×35 co dotychczasowy LiMnO2 → pasuje do obudowy). **KLUCZOWE: dobierać po NISKIM samorozładowaniu, nie po mAh** — runtime bez PV ma sufit ≈ 1/(stopa SD): 3%/mc → ~2.7 lata, 1-2%/mc → ~4-8 lat, **niezależnie od pojemności** (SD skaluje się z C). Limiterem bez PV jest samorozładowanie (~0.5-0.7 mAh/d), nie node (0.14). Grubsze/szersze ogniwo (20350 itp.) bez sensu: niestandardowy LFP + marny zysk.
  - **PV = OPCJONALNY** (decyzja: bez PV ~2-3 lata na USB to dobry UX; z PV → kasuje samorozładowanie → lata/„never-touch"). **Bonus bez PV: SoC czystszy** (powolne rozładowanie → jedna gałąź discharge-LUT, brak histerezy harvestu → ostry low-warning).
  - **Prototyp rev-2: jedna płytka, oba tryby** — **jumper/suwak na WEJŚCIU PV do BQ25570 `VIN_DC`** (zwarty=harvest, rozwarty=sam LFP; NIE na szynie baterii). Topologia LFP-centryczna: `USB→ładowarka LFP (CN3058E)→LFP` + `PV→[jumper]→BQ25570→LFP` + `LFP→CC1310 wprost` + `dzielnik→ADC`. Produkcja: populate/DNP wybiera wariant.
  - **Caveaty schematu**: 2 ładowarki na 1 ogniwie (USB+BQ25570, obie na ~3.6 V, anti-back-feed Schottky); kierunek dla SoC-LUT: BQ25570 charge-status (PV) lub CHG ładowarki USB (bez PV) → OR/wybór w firmware.
- **DESIGN rev-2 DOPIĘTY na poziomie komponentów (sesja 27-28.06) — pełna spec w [[rev2-battery-architecture]].** Kluczowe ustalenia: **18500 LFP unprotected** (dobór po NISKIM SD; user testuje SD sam) **+ USB-only (MCP73123 3.6 V, STAT→GPIO)**, **PV → DNP/rev-3**; **TPS63900** buck-boost always-on 3.3 V (~75 nA; bez bypassa — 75 nA to już stan snu); **PERIPH_EN** bramkuje SHT35 (TPS22860) + dzielnik ADC; **SoC = MCP3421** (18-bit I2C, dzielnik ÷2 1 MΩ 0.1% gated + 100 nF + 1-pkt cal, discharge-LUT, pomiar co N-ty cykl w quiescent); **on-board PCM** (LFP-threshold protection IC low-Iq + dual FET + PTC, **NIE DW01**) + **reverse-polarity MOSFET**; **NVS = wewn. flash CC1310** (zero zewn. kości); **debug**: cJTAG 10-pogo-pad + **UART przez tap BC-UART z gold-pinów LaunchXL** (NIE po JTAG-u — osobne linie do MSP432E); **JOIN button** (wake-from-standby + RC/SW debounce, wake-source RTC vs button). Runtime ~2.3-3.9 lat/ładowanie USB (>2 lata pewne).
- **NASTĘPNE:** user robi **PCB rev-2** (zna layout RF CC1310). Do zrobienia po stronie firmware/softu: **sprzątnięcie firmware pomiarowego** (usunąć ACC-mode/`acc_uah`/diagnostykę OpConfig z noda; `soh_pct`/`acc_uah` niezacommitowane w repo — zdecydować co zostaje), **kalibracja LUT** z realnej krzywej rozładowania, **integracja JOIN** z provisioningiem bramki. **PPK2** (kupowany) → dokładny pomiar µAh/cykl.

### 2026-06-26 — REALNY bateryjny node T&H (CC1310+SHT35+BQ35100, CR123A) DZIAŁA E2E ✅
- **ZWERYFIKOWANE NA ŻYWO (na baterii, bez JTAG)**: prototyp PCB bateryjnego noda temp/wilgotność → power-cycle co ~3 min → SHT35 + BQ35100 → RF → bramka → DB. `telemetry node 26 type 6: 3 param(s)`: temperatura 25.9°C, wilgotność 58.9%, **batt_mv 2875 mV** (multimetr 2917 — różnica ~1,4% = obciążenie+offset, OK).
- **HW**: CC1310 + SHT35 (I2C 0x45) + BQ35100 fuel gauge (I2C 0x55) + CR123A (Li-MnO2 ~1400-1550 mAh). Power-cycling **TPL5111** (DRVn→TPS22860 load-switch + TPS61291 boost/bypass). Projekt CCS (off-repo): `workspace_v12\TemperatureHumidityNode_CC1310_tirtos_ccs` (był to `rfWsnNode`/EasyLink — przerobiony).
- **Architektura zasilania**: timer rządzi snem; MCU tylko impuls **DONE (DIO23)** = „gotów do odcięcia". TPL5111 VDD/EN/DELAY na **always-on** (bateria), przez bramkę idzie tylko DRVn. BQ35100 VDD always-on (liczy zawsze), GE na VCC_SWITCH (gauge aktywny w oknie pracy MCU). Firmware **liniowy**: boot → pomiar → wyślij → DONE → halt.
- **🔑 Radio przepięte z EasyLink na nasz raw-RF + PHY z gen2-node** → gada z bramką (ramka `[0x00]['D'][src][MessageStruct][seq][crc8]`). Adres stały **0x1A** (provisioning później). Pliki: `rfWsnNode.c` (main+task, `NODE_MODE_DONE` 1=power-cycle / 0=bench-loop z JTAG), `radio.c/h` (jednorazowy blokujący send), `sensors.c/h` (SHT35 + BQ35100).
- **🔑 LEKCJE (godziny debugowania)**: (1) **power-cycle testować TYLKO na baterii** — JTAG **zasila płytkę od tyłu** (przez diody IO) → szyna/DRVn trzymają 3,3V, wygląda „switch nie gasi"; logikę debuguj w bench-loop, cykl na baterii. (2) **TPL5111 DONE = krótki impuls, idle LOW** (nie trzymanie HIGH) + `GPIO_init()`/PRCM dla zegara GPIO. (3) **BQ35100 sekwencyjnie wybredny** (driver u-blox): GE settle → GAUGE_START przez **MAC 0x3E←0x0011** → poll `CONTROL_STATUS` bit0 (GA)=1 → `Voltage()` 0x08 (2B LSB-first). Zimny odczyt=`0xBEBE`; GAUGE_START przez Control 0x00 → NAK. GAUGE_START **co wybudzenie** (GE pada przy śnie; AccumulatedCapacity persystuje na always-on VDD). (4) Schemat był OK — winą JTAG + DONE-hold, nie projekt.
- **Telemetria**: `thData` rozszerzone o `batt_mv` (`shared/node_protocol.h`, `telemetry.go` dekoder, `store.go` param_def typ 6). MessageStruct dalej 44B. M4F forwarduje surowo (bez reflasha).
- **Test stałego 0x1A**: bramka ignoruje nieznane nody → wstaw wiersz `INSERT OR REPLACE INTO node(node_id,node_type,name,factory_id,status) VALUES(26,6,'TH-proto','aabbccddeeff0011','active')`.
- **Mirror**: `cc1310-th-hw-firmware/` (źródła + README z lekcjami). To NIE to samo co `cc1310-node-th-firmware` (tamten symulowany na LAUNCHXL).
- **NASTĘPNE: coulomb/EOS** (Krok 3): AccumulatedCapacity `0x02` (4B µAh) + GAUGE_STOP/sesja + chemia Li-MnO2 + kalibracja; potem provisioning power-cycle, produkcyjne cięcie UART/poboru.

### 2026-06-23 — WebSocket live channel (Faza A) + apka provisioning event-driven + mirror do repo ✅
- **ZWERYFIKOWANE NA ŻYWO**: kanał WS telefon↔bramka działa; JOIN noda → **okienko dodawania само wyskakuje** w apce, statusy live, usuwanie znika od razu. Zero pollingu.
- **Decyzja architektury (z userem)**: **WS = kanał live podczas używania apki** (dwukierunkowy, instant: telemetria, zdarzenia provisioningu, w Fazie B komendy z `reqId` = async odpowiedzi). **FCM = alerty w tle** (apka zamknięta; awaria czujnika/przegrzanie — user ma to w gen1) — później. Kładziemy WS teraz jako fundament pod kolejne funkcje. [[app-push-events-todo]].
- **Bramka (Go)**: `wshub.go` (NOWY — hub gorilla/websocket, `/ws` na tym samym TLS :9443, auth tokenem przed upgrade, ping/pong, drop wolnych klientów), `httpapi.go` (route `/ws`), `main.go` (hub w `runServe`, drenaż publikuje `join_pending`/`telemetry`/`node_status`; JOIN publikowany przy KAŻDYM wciśnięciu). Zależność: `go get github.com/gorilla/websocket`.
- **Apka (`GatewayWs`/`GatewayApp`)**: singleton OkHttp WS z **tym samym pinningiem** (`CertPin.getPinnedTrustManager()` dodany), connect na foreground / disconnect w tle, reconnect 3 s, listenery na wątku main. `DevicesActivity` implementuje listener: `join_pending`→auto-okienko, `node_status`→odśwież.
- **🔑 LEKCJE apka**: (1) **`NetworkOnMainThreadException`** — `baseWsUrl()` robi blokujący TCP-probe (`lanReachable`), więc `connect()` MUSI iść na wątek tła (inaczej WS nigdy nie dochodzi do bramki, a HTTP działa bo leci z własnych wątków). (2) **Launcher odpalał gen1 zamiast gen2** — bo kopiowany projekt miał ten sam `namespace com.example.gatewaycommunicator` → ten sam domyślny `taskAffinity` → Android wynosił task gen1. Fix: **pełny rename package → `com.example.gatewaycommunicatorgen2`** (namespace+applicationId+wszystkie źródła). (3) OkHttp WS = OkHttp (HttpsURLConnection nie umie WS); pin wpięty przez `sslSocketFactory(factory, trustManager)`.
- **UX (decyzje usera)**: usunięte przyciski „Odśwież"/„+Dodaj" (zbędne — JOIN event-driven); `pending_remove` **wykluczone z `listnodes`** (znika z listy od razu, bramka po cichu dokańcza); toasty ujednolicone na dole (tylko finalne komunikaty); ikona gen2 niebieska.
- **Mirror apki do repo: `android-app/`** — istotne pliki (java/kt + res + manifest + gradle config), BEZ `build/`/`.gradle`/wrappera/`local.properties`/`cert.pem` (sekret, out-of-band). Wzór jak `cc1310-*firmware`. Źródło budowania: `C:\Users\damia\AndroidStudioProjects\GatewayCommunicatorGen2`.

### 2026-06-22 (cd.2) — graceful remove + lifecycle statusów + listnodes + apka provisioning ✅
- **ZWERYFIKOWANE NA ŻYWO (cała macierz)**: add (z natychmiastowym confirm), remove online (potwierdzenie→cisza→zwolnienie), remove offline (`pending_remove`→re-REMOVE po powrocie→zwolnienie), re-add reużywa adres (usunąłem 0x14, dodałem → dostałem 0x10).
- **Two-phase commit z potwierdzeniem read-back (decyzja z userem)**. Stan w `node.status`: `pending_join` | `active` | `pending_remove`. **Adres zarezerwowany dopóki istnieje wiersz** → wiersz kasujemy DOPIERO po potwierdzeniu noda.
  - **ADD**: approve → wiersz `pending_join` + addr. Node JOIN_ACCEPT → `identity_persist` (zapis+**read-back**) → adres przyjęty TYLKO po weryfikacji → **natychmiastowy confirm** (`sendReading` od razu, nie czeka na tick 60 s). Bramka na 1. telemetrii z addr → `MarkActive` (pending_join→active).
  - **REMOVE (graceful)**: apka → `removenode` → `SetPendingRemove` (NIE kasuje) + `SendRemove`. Node: zapis 0xFF zweryfikowany → **confirm ze STAREGO adresu** (`cmd=CMD_REMOVE`) → milknie. Bramka demux `cmd=CMD_REMOVE` od noda → `DeleteNode` (zwolnienie). Offline: wiersz zostaje, na każdą telemetrię z `pending_remove` → **re-send REMOVE** (self-heal).
- **Alokacja: najniższy wolny (reużycie)** — monotonik (watermark) wprowadzony i **wycofany** jako zbędny: graceful confirm sam gwarantuje, że zwolniony adres = node milczy → reużycie bezpieczne; offline trzyma wiersz → adres i tak zarezerwowany. (Stary klucz `addr_watermark` w `config` zostaje, ignorowany.)
- **Unprovisioned = 0xFF = cisza**: świeży/usunięty node domyślnie 0xFF i **nie wysyła telemetrii** (tylko JOIN na button). Default w firmware zmieniony `TH_NODE_ADDRESS`→`ADDR_UNPROVISIONED`.
- **Pliki Go**: `store.go` (status+migracja, `NodeStatus`/`MarkActive`/`SetPendingRemove`/`GetNode`/`DeleteNode`/`ListNodes`, alloc lowest-free), `httpapi.go` (`listnodes`/`removenode`), `nodecmd.go` (`SendRemove`), `main.go` (drain state-machine), `join.go` (`CmdRemove`), `telemetry.go` (`NodeMsgId`). `shared/node_protocol.h`: `CMD_REMOVE 6u`.
- **Node**: `th_sensor_task.c` (identity 0xFF default + cisza, `identity_persist`→bool+read-back, JOIN immediate-confirm, REMOVE→0xFF+confirm).
- **Apka (off-repo `GatewayCommunicatorGen2`)**: `DevicesActivity` + `activity_devices.xml` (lista `listnodes` ze statusem `… dodawanie`/`… usuwanie`/online/offline; dodaj=`listjoins`+`approvejoin`; usuń=tap→dialog→`removenode`; toast „Usunięto" gdy zniknie z listy), `NetworkClient` (`removeNode`), main button „Zarządzaj urządzeniami". **Polling wycofany** na rzecz jednorazowego odświeżenia po akcji (user: polling słaby UX).
- **NASTĘPNE (do omówienia z userem)**: event-driven UI — JOIN noda ma SAM wyskoczyć w apce (okienko dodawania) + statusy live, bez „Odśwież". Opcje: SSE/long-poll/WebSocket z bramki, albo FCM push. [[app-push-events-todo]].
- **Drobne**: po REMOVE-confirm node 3× retransmituje (flip na 0xFF → nie rozpoznaje własnego ACK) — nieszkodliwe, do wyciszenia.

### 2026-06-22 (cd.) — provisioning Faza 1 krok 4 (approve + JOIN_ACCEPT) ✅ + cutover bramki na 0x00 ✅
- **ZWERYFIKOWANE NA ŻYWO (gen1 WŁĄCZONA, nie przeszkadza)**: `listjoins` → JSON pending, `approvejoin` (factory+name) → `address:16` (0x10, alloc z puli) → wiersz `node` → `JOIN_ACCEPT` w dół (`TX 0x33` ACK M4F) → CC1310 TX do 0xFF. Telemetria 243 leci nieprzerwanie obok.
- **Approve API (tylko Go)**: `store.go` (`ProvisionNode` idempotentne po factory_id — re-approve/rejoin reużywa adresu; `allocAddr` najniższy wolny `0x10–0xEF`; migracja `ALTER TABLE node ADD COLUMN factory_id`), `nodecmd.go` (`SendJoinAccept` — `msg_make_join_accept`: id=0xFF, cmd=JOIN_ACCEPT, joinAcceptData, len 13; jedzie `MSG_NODE_CMD` jak SendPump), `join.go` (`Get`/`Remove`, json-tagi, `factoryHexToBytes`), `httpapi.go` (`command=listjoins`/`approvejoin`, `url.ParseQuery` na query+body), `main.go` (joins→StartHTTPAPI).
- **🔑 CUTOVER BRAMKI NA 0x00 (decyzja usera — izolacja gen1/gen2 na czas dewelopmentu)**: `CONCENTRATOR_ADDRESS` 0xF0→**0x00** w OBU plikach CC1310 (`cc1310-firmware/radio_task.c` gateway + `cc1310-node-th-firmware/rfEchoTx.c` node). Adres bramki żyje WYŁĄCZNIE jako lokalny `#define` w firmwarze CC1310 (NIE w MessageStruct.id, NIE w M4F/Go — `ADDR_GATEWAY` w `node_protocol.h` był doc-only, zmieniony 0xF0→0x00 dla spójności). Filtr RX `rxMsg[0]==CONCENTRATOR_ADDRESS` (`radio_task.c:334`) robi izolację: stary solar/bufor (→0xF0) odbiera tylko gen1; gen2 TH node (→0x00) odbiera tylko gen2; gen1 dropuje ramki gen2 → **koniec duplikatów, gen1 może zostać włączona**. **Flashować bramkę+noda RAZEM** (inaczej telemetria stanie). Stare nody dostaną 0x00 przy reflashu z provisioningiem.
- **Lesson — PowerShell `curl`**: alias na `Invoke-WebRequest` (brak `-k`); używać `curl.exe -k`. PS 5.1 IWR nie ma `-SkipCertificateCheck` (to PS 6+).
- **✅ KROK 5 ZROBIONY I ZWERYFIKOWANY NA ŻYWO (strona noda CC1310)**: parsowanie `JOIN_ACCEPT` w `rfEchoTx.c`→`node_handle_rx_command()` (match `joinAcceptData.factory_id` vs własne FCFG) → `gNodeAddress = assigned_addr` → node raportuje pod nowym adresem. **Dowód**: po approve `telemetry node 16 type 6` zamiast 243; node loguje `[TH] JOIN_ACCEPT: assigned addr 0x10 (now reporting under it)` + nadaje `src 0x10`. Adres = global `gNodeAddress` (th_sensor_task.c, extern w rfEchoTx.c), filtry RX i `msg->id` telemetrii go używają; JOIN dalej pod `0xFF`.
- **Persystencja adresu — driver NVS TI (ZWERYFIKOWANA: power-cycle 30 s → node sam zaczytał 0x10)**: BEZ syscfg (CC1310 go nie ma). Region `flashBuf @ 0x1A000` (`#pragma LOCATION`+`NOINIT`, 4 sektory) + `NVSCC26XX_HWAttrs`/`NVS_config[]`/`Board_NVSINTERNAL` **już są w board file noda** (`CC1310_LAUNCHXL.c`, kopia z LAUNCHXL) — kod tylko woła `NVS_init`/`NVS_open(Board_NVSINTERNAL)`/`NVS_read`/`NVS_write(NVS_WRITE_ERASE|NVS_WRITE_POST_VERIFY)`. `IdentityRec{magic,addr,_pad[3]}` (8 B, multiple-of-4). `identity_init` czyta+waliduje (magic + addr w puli) → `gNodeAddress`; `identity_persist` zapisuje. Driver sam ogarnia flash FAPI + cache VIMS. **Wzorzec wzięty 1:1 z SDK `nvsinternal_CC1310_LAUNCHXL_tirtos_ccs`** ([[cc1310-flash-persistence-nvs]]).
  - **Ślepa uliczka (zanotowane)**: pierwsza próba = surowy `FlashSectorErase`/`FlashProgram` na własnej `static const __attribute__((aligned(4096)))` tablicy → `st=3` (`FAPI_STATUS_INCORRECT_DATABUFFER_LENGTH`). Powód: **legacy TI compiler (v20.2.6.LTS) nie honoruje `aligned(4096)` na const w `.const`** → baza niesektoro-wyrównana. TI rezerwuje region przez `#pragma LOCATION` na sztywny adres, nie przez `aligned`. Lekcja: na CC13xx do NV używać drivera NVS, nie ręcznego FAPI.
- **Lesson — PowerShell `curl`**: alias na `Invoke-WebRequest` (brak `-k`); używać `curl.exe -k`.
- **Następne: apka Android pod provisioning** (lista pending + approve+nazwa). Endpointy Go gotowe: `command=listjoins` (JSON `[{factory,type,firstSeen,lastSeen,count}]`) + `command=approvejoin&factory=<hex>&name=<label>` (→ `{address,factory,name,type}`). Token + pinning jak reszta API (port 9443).

### 2026-06-22 — provisioning Faza 1 (kroki 1-3): JOIN uplink E2E ✅ + fix dedup po reconnect
- **ZWERYFIKOWANE NA ŻYWO**: button na nodzie → `[Serve] JOIN request: node type 6 factory ee467e22004b1200 (awaiting approval)`. Pełna ścieżka uplink: node (factory ID z FCFG) → RF (src 0xFF) → CC1310 → M4F (forward all-types) → Go demux `cmd=JOIN_REQUEST` → rejestr pending. Telemetria leci równolegle.
- **Protokół (`shared/node_protocol.h`)**: `CMD_JOIN_REQUEST`(4)/`CMD_JOIN_ACCEPT`(5); `joinData{factory_id[8]}` (node→gw), `joinAcceptData{factory_id[8],assigned_addr}` (gw→node); adresy `ADDR_GATEWAY 0xF0` (legacy zostaje — cutover do 0x00 odłożony żeby nie psuć solar/bufor), `ADDR_UNPROVISIONED 0xFF`, pula `0x10-0xEF`. MessageStruct dalej 44B.
- **Node (`cc1310-node-th-firmware/` + CCS)**: `read_factory_id()` z FCFG1 (`HWREG(FCFG1_BASE+FCFG1_O_MAC_15_4_0/_1)` — **zbudowało się i czyta poprawny IEEE**), **button = JOIN** (`sendJoinRequest`, id=0xFF), `rfEchoTx.c` src ramki = `tempMsg.id` (telemetria 0xF3 / JOIN 0xFF), RX akceptuje `0xFF`.
- **Gateway CC1310 (`cc1310-firmware/radio_task.c`, mirror)**: fix hardcoded TX — `frame[0]=tempMsg->id` (było 0xF1) — kompatybilne wstecz (SendPump i tak id=0xF1), odblokowuje JOIN_ACCEPT→0xFF + komendy multi-node. (Niepotrzebne do uplinku, potrzebne do kroku 4/5.)
- **Go**: `telemetry.go` (`NodeMsgCmd`/`DecodeJoinRequest` cgo), `join.go` (NOWY — rejestr pending, dedup po factory_id), `main.go` (demux JOIN vs telemetria).
- **🔑 BUG FIX (pre-existing, niezawodność) — dedup po reconnect**: `protocol.go` `theirLastSeq` rósł monotonicznie i NIE resetował się przy reconnect. M4F **zeruje swój seq przy nowym HELLO**, więc po dużym backlogu (seq 18-25) żywe eventy seq 1+ leciały jako „duplikaty" → wyrzucane przed `EventRx` → drain ich nie widział (ginęła telemetria I JOIN). **Fix**: reset `theirLastSeq=0` przy `MSG_HELLO_ACK`. Objaw mylił: `[Protocol] RX` widać, `[Serve]` cicho; to NIE był panic (`tail` = czysto).
- **Następne (krok 4)**: approve API na telefon (lista pending + approve{nazwa}) → alokacja adresu z puli + wiersz `node` + `JOIN_ACCEPT` w dół (tu wchodzi reflash gateway-CC1310 z fixem TX). Potem krok 5: node odbiera ACCEPT → NVS → przełączenie adresu.

### 2026-06-21 (wieczór) — provisioning Faza 0: nowy typ noda temp/humidity E2E ✅
- **ZWERYFIKOWANE NA ŻYWO**: nowy node TH (typ 6) → bramka → DB. m4f-watch `[SPI] RX node data -> engine (type=6 cmd=0)` → `TX 0x40`; Go `telemetry node 243 type 6: 2 param(s) stored`; DB `node_param` 243 = temperature 25.81, humidity 45.05. 3 nody naraz (241 solar / 242 bufor / 243 TH).
- **🔑 Bug M4F (naprawiony)**: telemetria do Linuksa była bramkowana `if (engine_update_node())` — true tylko dla znanych typów (solar/bufor folding do `NodesData`). Typ 6 → false → telemetria nie leciała (CC1310 forwardował, Go nic nie dostawał). Fix w `ipc_rpmsg_echo.c`: **raw telemetria (→DB) dla KAŻDEGO typu**, niezależnie od silnika; folding+reguły osobno dla znanych typów. Skopiowane do CCS freertos.
- **Strona bramki (commit Fazy 0)**: `shared/node_protocol.h` (`NODE_TH_SENSOR 6u` + `thData{temperature,humidity}` w unii — rozmiar MessageStruct bez zmian, 44B), `telemetry.go` (`DEC_KIND_TH` + case), `store.go` (seed `param_def` typ 6, archive=0), `ipc_rpmsg_echo.c` (fix wyżej).
- **Firmware noda TH** (off-repo, CCS `workspace_v12\SubGHzTempHumNodeGen2_CC1310` — [[cc1310-ccs-project-paths]]): kopia solar-noda oczyszczona z ADC/PWM/ePaper/fontów; `th_sensor_task.c` (timer 60s → losowa temp 18-26°C/wilg 30-70% → MessageStruct typ 6 → radioQueue; button=instant send), `rfEchoTx.c` (NODE_ADDRESS 0xF3), `main_tirtos.c`. `smartrf_settings` nietknięte (PHY = bramka).
- **LEKCJE bring-up noda**: (1) **`&` w nazwie projektu CCS = flash nie działa cicho** (DSLite/Windows ucina komendę na `&`) → rename `SubGHzTemp&Hum…`→`SubGHzTempHum…`; nigdy `&`/spacje w nazwach projektów. (2) Stare `.launches/*.launch` + wiele `.out` po kopii projektu mylą „Flash" → użyj **Run→Debug** (pokazuje błędy) + rename. (3) f2/f3 słabo słyszane = **zasięg** (RSSI), nie kod — przesunięcie 30 cm pomogło; CC1310 RX bez filtra adresu, kod gen1=gen2 bajtowo. [[am62-mcu-pin-traps]], [[cc1310-ccs-project-paths]].
- **NOWE TODO firmware**: (a) **debounce buttona** na nodzie (multi-send na 1 wciśnięcie); (b) `m4f-reload` number-agnostyczny ([[m4f-remoteproc-numbering]] — remoteproc0 czasem R5F po reboocie); (c) hardcoded TX `frame[0]=0xF1` w gen2 `radio_task.c` → adres z `message.id` (multi-node/provisioning, [[provisioning-model]]); (d) kosmetyczny `%s`-garbage w logu TX noda.
- **Następne: provisioning Faza 1** (commissioning) — JOIN/ACCEPT, adresacja 0x00 bramka/0xFF join/0x10-0xFE pula, factory ID (CC1310 FCFG)→NVS. Model: [[provisioning-model]].

### 2026-06-21 — Faza 3 #1 + rdzeń #2: telemetria nodów → SQLite + dedykowana tabela solar ✅
- **ZWERYFIKOWANE NA ŻYWO (gen1 off)**: telemetria nodów dekoduje się i ląduje w bazie E2E. `MSG_NODE_TELEMETRY` (0x40) → dekoder cgo → `node_param` (stan bieżący) + (dla solar) `solar_history` z dzienną akumulacją.
- **Schemat (decyzja z userem, zmiana w trakcie)**: zrezygnowany generyczny `sample` (i cała maszyneria `archive`/`archived`). Model: **jedna tabela stanów bieżących `node_param` (UPSERT, wszystkie typy) + dedykowana tabela per typ dla nodów z obróbką** (`solar_history`). Nowy typ z historią = własna tabela przy provisioningu.
- **Pliki (tylko Go)**: `telemetry.go` (NOWY — dekoder cgo `MessageStruct` unia→`[]NodeParam`, `msg_decode` per type+cmd: solar full / pump-only / bufor), `store.go` (schemat `node`/`param_def`/`node_param`/`solar_history`/VIEW `solar_state`, `RecordTelemetry`, `recordSolarHistory` z `sameLocalDay`), `main.go` (drain `MsgNodeTelemetry`, `OpenStore(dbPath, loc)`).
- **Solar — akumulacja dzienna**: `energy_gain = prev(ten sam dzień) + delta`, `pump_runtime = prev + (flow>0 ? 2min : 0)`, reset o północy Europe/Warsaw (`sameLocalDay` w `s.loc`). `solar_state` VIEW = ostatni rekord + `generated_power_kw = 30*energy_gain_delta/10000`. **Dowód**: `energy_gain` 322→644→969, `pump_runtime` 2→4→6, `generated_power_kw` 0.975 (=30*325/10000), odczyty co 120s.
- **🔑 energyGain = kWh×10000 (int dla radia), 2-min DELTA**; legacy akumulował dziennie (ostatni rekord = cały dzienny uzysk, crash-safe). `flowRate` = `%` (nie l/min). **2 pompy**: `flowRate` = pompa obiegowa zmiennoobrotowa (do bufora 1, sterownik solarny ją trzyma, my read-only); `secondPumpState` = pompa on/off do bufora 2 (TO ją steruje automatyka/telefon `TURN_PUMP_ON_OFF`). `pump_runtime` liczony z `flowRate>0`. Szczegóły: [[solar-node-data-model]].
- **Duplikaty = współbieżna gen1** (potwierdzone): gen1 on → ten sam odczyt 2-3× (kolizja ACK RF, node retransmituje); gen1 off → seq noda 40,41,42,43 bez powtórek, zero dup. **Decyzja: zero dedup w Go** — to robota CC1310 (którego dedup w obecnym buildzie nie działa → firmware TODO). Testy solara z gen1 off.
- **sqlite3 CLI**: `opkg` ma martwy feed TI (`lcpd.itg.ti.com`, host wewnętrzny). Skompilowany z amalgamacji `sqlite.org` (autoconf tarball, `gcc shell.c sqlite3.c`) → `/usr/local/bin/sqlite3` + symlink `/usr/bin`. ~10 min build na A53 (`-O2`).
- **M4F crashe pod RF** (2× peer-dead + 1× hard reset w ~2h) — odłożone (user: najpierw bazy). Handler hardfaultu to czarna skrzynka (zero zrzutu rejestrów) → plan: retained-RAM dump + log-on-next-boot. Recovery działa za każdym razem.
- **Lesson — quoting PS 5.1**: `\"` to NIE escape w PowerShell → SQL z `'%'` rozjeżdżał się (`incomplete input`). Obejście: `DELETE FROM param_def` + reseed zamiast `UPDATE` z literałami; albo `ssh -t ... sqlite3` interaktywnie.

### 2026-06-19 — reguły czasowe E2E z telefonu: guard solarny zdjęty + snap czasu ±4s ✅
- **ZWERYFIKOWANE NA ŻYWO**: reguły `COND_TIME` → `SET_RELAY` pompa, dodane z apki, sterują pompą wg harmonogramu (ON o min X, OFF o min Y) — **co do minuty**. Pełna pętla: telefon → reguła → SQLite → push → silnik tickuje `:00` → fire → SPI → CC1310 → node → pompa.
- **Guard `sBuforTemp < 0` usunięty** (`engine.c`, zakomentowany z notką gen1): blokował każdą solar `SET_RELAY` dopóki node bufora nie zgłosi `sBuforTemp ≥ 0` (init −20) — w teście był tylko node pompy → nic nie odpalało. Dedup `pumpState` ZOSTAJE (feedback/anty-spam). Przywrócić przy telemetrii z `BUFOR_CONTROLLER`.
- **Bug: OFF spóźniony o minutę** (zaplanowany 22:45, fire 22:46): `evaluateTimeCondition` czytał surowy `wall_now`, a tick przez rozjazd faz wybudza się chwilę PRZED `:00` → floor czytał minutę N-1 → okno startujące na granicy nie pasowało → fire dopiero przy N+1. **Fix: `wall_now_rounded()` snap ±4s** (`WALL_TICK_BIAS_S=4`, `:56…:04 → pełna minuta`), używany spójnie w warunku ORAZ dedupie (jeden „czas ticka"). Reguły autorowane w pełnych minutach → trafiają idealnie. Szczegóły: [[engine-eval-cadence]].

### 2026-06-19 — strefa czasowa: silnik na Europe/Warsaw (embedded tzdata) ✅
- **Problem**: Arago domyślnie UTC + **minimalna tzdata (brak Europe/Warsaw**, `timedatectl set-timezone` failuje). Silnik bierze czas z `time.Now()` Go → reguły `COND_TIME` leciały ~2h za wcześnie (PL lato = UTC+2).
- **Fix silnika (właściwy, w Go)**: `import _ "time/tzdata"` (wbudowana baza IANA w binarce) + flaga `-tz` (default `Europe/Warsaw`) + `runServe` ładuje strefę jawnie (`time.LoadLocation`), `syncClock` liczy `time.Now().In(loc)`. → reguły poprawne **niezależnie od systemowej tzdata**, DST CET/CEST automatyczny. ZWERYFIKOWANE: log systemowy 19:32 UTC, ale `time-sync -> 21:32:51 (Europe/Warsaw)`.
- **Fix systemu (moduł `09-timezone.sh` + `config.sh TIMEZONE` + export w `setup.sh`)**: ustawia systemową strefę (date/logi/boot-accounting). Bo brak zoneinfo na obrazie → moduł **wyciąga strefę z tzdata Go** (`/usr/local/go/lib/time/zoneinfo.zip` przez `go run` mały extractor), ustawia, włącza NTP; gdy się nie uda → WARN + `exit 0` (NIE wywala setupu, silnik i tak ma własną tzdata).
- **Kontrola dryfu (dni/tygodnie)**: NTP systemowy (`systemd-timesyncd`, `timedatectl` „NTP active") koryguje zegar Linuksa ciągle; `serve` re-syncuje M4F **co 10 min** (`syncClock` ticker) → zero kumulacji dryfu online. Offline (RTC-less) → [[rtc-must-have-carrier]].

### 2026-06-19 (remote, Faza 2) — CRUD reguł z telefonu + SQLite ✅
- **ZWERYFIKOWANE NA ŻYWO**: `getrules`/`setrules` z apki działają z **SQLite** jako źródłem prawdy. Flow: telefon dodaje regułę → `setrules` → `parseAppRules` → `store.SetRules` (zapis do `/var/lib/bramka/bramka.db`) → `PushRules` na M4F (`crc32` OK). `getrules` zwraca regułę (round-trip app JSON). Restart serve → `pushed 1 rule(s) from DB` = persystencja + push na starcie.
- **Pliki (tylko Go)**: `rulesjson.go` (app JSON ↔ `Rule`, klucze `hS/mS/d/p/op/mn/mx`, ordinale 1:1), `store.go` (`mattn/go-sqlite3` cgo, WAL+synchronous=NORMAL, tabela `config(key,value)`, `GetRules/GetRulesJSON/SetRules`), `httpapi.go` (`handleSetRules` + `extractRulesField` z body `command=setrules&rules=…&authToken=…`, getrules zwraca blob z DB), `main.go` (flaga `-db`, `runServe` otwiera store, push **z bazy** na connect zamiast pustego).
- **Build/deploy**: `go.mod`/`go.sum` NIE w repo (żyją w `/opt/bramka/rpmsg-service` na bramce). Dep dodany przez `go get github.com/mattn/go-sqlite3` **w katalogu buildu** (`/opt/bramka/rpmsg-service` — nie myl ze starym `/root/bramka-services`!). Deploy-Go rozszerzony o `go mod tidy` (samonaprawialne deps). **Pierwszy build ~15 min** (gcc kompiluje `sqlite3.c` ~9 MB single-TU na A53; potem Go build-cache → szybko). Internet+gcc na bramce są.
- **LEKCJE**: (1) `pkill -f 'rpmsg-service -test serve'` **self-matchuje powłokę** odpalającą tę samą komendę → ubija launchera zanim `setsid` wystartuje; restart serve rób **dwoma osobnymi** ssh (kill, potem start). (2) Go `json.Marshal(map)` sortuje klucze alfabetycznie — apka czyta po nazwach, więc bez znaczenia. (3) app action niesie tylko `value` (int, brak `msg`) → reguły SEND_MESSAGE z apki nie mają tekstu (ograniczenie apki).
- **TODO**: baza powstaje jako root (serve interaktywny) → przy przejściu na systemd (user `bramka`) `chown bramka:bramka /var/lib/bramka/bramka.db*`. Reguła „pompa ON 10-14h solar" jest **czasowa SET_RELAY** → z time-syncem realnie odpali i sama przełączy pompę w oknie (autonomiczny silnik E2E — do sprawdzenia).

### 2026-06-19 (remote, Faza 1) — pompa z telefonu E2E ✅ (telefon → node)
- **ZWERYFIKOWANE NA ŻYWO**: pełny flow **telefon (apka Android) → Go HTTPS :9443 → `MSG_NODE_CMD` (0x33) → M4F → SPI → CC1310 → RF → node → pompa fizycznie ON/OFF**. Node po komendzie odsyła stan (`type=SOLAR(0) cmd=SEND_PUMP_STATUS(1)` → `NODE_TELEMETRY` do Linuxa) = gotowe sprzężenie zwrotne dla silnika. Stara bramka gen1 zgaszona, RF czyste.
- **Strona Go (tylko Go, M4F/CC1310 nietknięte)**: `nodecmd.go` (builder `MessageStruct` cgo + `SendPump`), `httpapi.go` (serwer HTTPS, token, routing 1:1 z gen1: PUMP_ON/OFF/getrules/setrules), tryb `-test serve` w `main.go` (HELLO → time-sync z zegara systemowego co 10 min → push reguł na connect → drain eventów → serwer). `.gitignore`: `*.pem/*.key/*.crt`.
- **🔑 LEKCJA (klucz dnia) — tłumaczenie pompy**: node NIE reaguje na surowy `SMARTPHONE/SEND_TEXT_MSG/"PUMP_ON"`. gen1 `coreTask.c` (l.261-277) tłumaczył to na `MessageStruct{id=0xF1, type=SOLAR_CONTROLLER, cmd=TURN_PUMP_ON_OFF, pumpData.pumpState=1/0, length=5}`. W gen2 tłumaczenie robi **Go** (`msg_make_pump`), bo M4F forwarduje `MSG_NODE_CMD` surowo. Objaw przed fixem: telefon `OK`, transport+RF OK, node ACK-uje ramkę, ale **pompa nie rusza** (zły type/cmd). Po fixie CC1310 loguje `type=0 cmd=2` (było `type=5 cmd=3`). Cały kontrakt: [[remote-access-contract]].
- **TLS/pinning**: apka pinuje SHA-256 leaf-certa → bramka serwuje **ten sam `cert.pem`+`key.pem`** z CC3235 (w `/etc/bramka/tls/`, root:bramka 0640). Zero zmian w pinningu apki. Apka gen2 = kopia z osobnym `applicationId` (side-by-side z gen1), IP→`.170`, `pickUrl` na **TCP-connect** (ICMP `isReachable` bywa false-blokowane → spychało na public/hairpin). Port-forward 9443→`.170` pod public/komórkę.
- **LEKCJA — event-storm/wedge**: serve startowany na świeżo po długim odłączeniu wsysał backlog setek nieACK-owanych `RULE_FIRED`/telemetrii (leftover reguła `fire-smoke` strzelała godzinami) → `rxChan/eventRx full` + raz doprowadziło do zawieszki M4F (nikt nie pingował, bo serve był ubity → brak auto-reboota). FIX: serve **drenuje eventy przed HELLO** + **pushuje zdefiniowany zestaw reguł na connect** (Faza 1: pusty → czyści resztki). `pushed 0 rules` w logu potwierdza.
- **LEKCJA — recovery „won't stop"**: po zawieszce `m4f-reload` nie zatrzyma M4F (remoteproc stop timeout, state `running`). Działa: `mv /lib/firmware/.../ipc_echo...xer5f /tmp; sync; reboot` → po reboocie offline → `Deploy-M4F`. Sam `reboot` wystarczył by odzyskać. Dopisane jako follow-up (teardown IRQ na stop).

### 2026-06-19 (cd.) — prawdziwy GPIO-IRQ na SLAVE_READY + fix podwójnego fire ✅
- **ZWERYFIKOWANE NA ŻYWO**: SLAVE_READY na **realnym przerwaniu** (nie pollu). `m4f-watch`: `master task started (SLAVE_READY mode = IRQ, OUTP=4)`, `RX node data -> engine` edge-driven, E2E noda działa. Transfer MCSPI dalej INTERRUPT+CALLBACK; poll został tylko jako backstop 50ms.
- **🔑 KOREKTA BŁĘDNEJ DIAGNOZY**: wniosek z 19.06 „DMSC nie przyznaje M4F introutera obok Linuksa (r=-1)" był **NIEPRAWDZIWY**. Board config Linuksa przyznaje hostowi `M4_0` cały `WKUP_MCU_GPIOMUX_INTROUTER0` OUTP **4–7** (źródło: SDK `sciclient_default_boardcfg/am62x/sciclient_defaultBoardcfg_rm.c`, `TISCI_HOST_ID_M4_0`, IR_OUTPUT start 4 num 4). Sweep `Sciclient_rmIrqSetRaw` dla OUTP 4..7 → **r=0 dla wszystkich**. Prawdziwa przyczyna `r=-1` z 19.06: **zły `src_index`** (kopia przykładu = bank 0/30, a SLAVE_READY = MCU_GPIO0 pin 16 = **bank 1** → src_index 31). `GPIO_GET_BANK_INDEX`: `GPIO_PINS_PER_BANK_SHIFT=4` → ÷16.
- **Metoda (analiza przykładu, nie wymyślanie koła)**: przeczytany `gpio_input_interrupt` (`board.c` `Sciclient_gpioIrqSet`, `gpio_input_interrupt.c` HwiP + bank ISR) + potwierdzenie postem forum TI. Sweep OUTP 4..7 (diagnostyka, route+release idempotentnie) dał twardą odpowiedź zamiast zgadywania, czy ruszać boot-firmware. **UWAGA**: ręczny zapis rejestru introutera → SOC exception; routing TYLKO przez Sciclient; NIE asertować przy r!=0 (boot hang — to był nasz hang z 19.06).
- **Implementacja `spi_master.{c,h}`**: `spi_setup_slave_ready_irq()` zajmuje 1. przyznany OUTP (4→7), `GPIO_setTrigType(FALL)`, `GPIO_bankIntrEnable`, `HwiP_construct(intrNum=(OUTP-4)+16)`; ISR `sr_bank_isr` czyści bank status + `SemaphoreP_post(gSrSem)`. `spi_tx_cmd` (scenario A): w IRQ czeka na `gSrSem` zamiast pollu (z finalnym re-checkiem poziomu). `spiTask` czeka na `gSrSem` (ISR/`post_cmd`/backstop 50ms). Każda porażka (brak grantu / HwiP) → `gIrqActive=false` → czysty fallback na poll. `src_index = BANK_0 + GPIO_GET_BANK_INDEX(pin)`.
- **Fix podwójnego fire (regresja odsłonięta przez task SPI)**: NIE stale-build (`engine.o` świeży). Realna dziura w dedupie TIME: sen liczony `engine_ms_to_next_minute` w `ClockP`-µs, ale `xQueueReceive` śpi w **tickach FreeRTOS** → rozjazd faz wybudzał engine **~7ms PRZED** `:00`; `wall_now` floor'uje sekundy → minuta N (`:59`) → fire #1, potem realne `:00` → N+1 → fire #2 (goły klucz `h*60+m` nie łapie). Task SPI zmienił fazę i to ujawnił (18.06 faza była przypadkiem łaskawa). **FIX `engine.c`**: klucz dedupu z **biasem +2s** → `:59.99` i `:00.01` mapują się na tę samą minutę → JEDEN fire. Warunki `COND_TIME` dalej na dokładnym `wall_now`. Zweryfikowane: `RULE_FIRED` pojedynczo co 60s, też 1. granica.
- **Lessons**: (1) M4F GPIO-IRQ obok Linuksa DZIAŁA — nie wierzyć w „RM-denied" bez sprawdzenia grantu w boardcfg_rm.c + sweepu; (2) `src_index` to **bank** (pin÷16), nie pin; (3) FreeRTOS-tick vs ClockP to dwa zegary — dedup czasu musi tolerować fazę (bias), nie zakładać że timeout ląduje dokładnie na `:00`; (4) `GPIO_pinRead` na input zwraca pad OK (caveat dotyczył output). [[am62-mcu-pin-traps]], [[engine-eval-cadence]].
- **TODO domknięcia SPI (bez zmian)**: scenario-A RX-routing edge-case, NODE_TELEMETRY→DB, `pending`/drenaż serii, wyciszenie logów SPI. Obserwacja RF (osobny temat): CC1310 `Kolizja... radio` / `Not ACKED` — retry RF noda, sam się podnosi.

### 2026-06-19 — SPI/CC1310 link ŻYWY dwukierunkowo + E2E z nodem RF ✅
- **ZWERYFIKOWANE NA ŻYWO**: pełny most **M4F (SPI master) ↔ CC1310 (SPI slave)** w obie strony, z realnym nodem RF. M4F→node: `RX cmd -> radio` → `[Gateway RF TX] Sent Message ACKED` + node ACK. node→M4F→Linux: `Received data from Node` → `sent node data up` → m4f-watch `RX node data -> engine` → `TX 0x40` (NODE_TELEMETRY do Linuxa). Pętla domknięta: engine→SPI→CC1310→RF→node ORAZ node→RF→CC1310→SPI→engine→RPMsg→Linux.
- **Architektura**: M4F=MCSPI master `MCU_SPI0`, mode1 (CPOL0/CPHA1), 1MHz, **INTERRUPT+CALLBACK, non-DMA**. CC1310=`SPI_SLAVE` CALLBACK (port gen1 CC3235 `spiTask.c`). Ramka `shared/spi_frame.h` 128B (magic 0xA5/type/seq/pending/len/CRC16-CCITT — `protocol_crc16`, identyczny obu stron). Handshake 2-liniowy active-low: MASTER_READY (M4F out → CC1310 IRQ), SLAVE_READY (CC1310 out → M4F in). Cykl/retry: pojedyncze okno 500ms (M4F) / hold 300ms (slave).
- **Piny FINALNE** (po bojach, niżej): M4F SPI A7=CLK, D9=D0/MOSI, C9=D1/MISO, B8=CS1. Handshake: **MASTER_READY = E5 (MCU_MCAN1_TX, pin10) → CC1310 DIO15**, **SLAVE_READY = D4 (MCU_MCAN1_RX, pin11) ← CC1310 DIO21**. CC1310 SPI: CLK DIO10, MOSI DIO9, MISO DIO8, CS DIO11. Masa wspólna.
- **Pliki**: `shared/spi_frame.h` (NOWY), `m4f-firmware/spi_master.{c,h}` (NOWY, wpięte w `ipc_rpmsg_echo.c`: `nodeTxSink`→`spi_master_post_cmd`, `spi_master_init(gNodeInQueue)`), `cc1310-firmware/` (NOWY katalog w repo: `spi_slave_task.c` + `spi_frame.h`; lustrzany workflow do M4F — cp do projektu CCS `SubGHzGatewayGen2_CC1310_LAUNCHXL_tirtos_ccs`). `rfWsnConcentrator.c` (projekt CC1310, poza repo): `spiMasterTaskInit`→`spiSlaveTaskInit` (2 linie, edytowane ręcznie w CCS).
- **🔑 LEKCJA HW (klucz całego dnia)**: **A6/B6 (MCU_GPIO0_7/8) to pady MCU_UART0 CTS/RTS — podłączone do wbudowanego mostka USB-UART na SK-AM62B**. Użyte jako GPIO handshake → kontencja z mostkiem: A6 jako output dawał 0,4V (nie 3,3V), B6 jako input nie czytał czysto. Objawiało się jako „nie steruje"/„slave not ready". **Fix: handshake na WOLNYCH padach MCAN1 (E5/D4)** — MCAN nieużywany. Patrz [[am62-mcu-pin-traps]].
- **LEKCJA SW1 — GPIO-IRQ M4F obok Linuksa**: przerwanie MCU_GPIO dla M4F idzie przez introuter (WKUP_MCU_GPIOMUX), który DMSC **nie przyznaje M4F** gdy współrezyduje z Linuksem (`Sciclient_rmIrqSetRaw` → r=-1). Dlatego **SLAVE_READY POLLowany** (lekki odczyt poziomu co 2ms), a nie na IRQ. Sam transfer dalej INTERRUPT+CALLBACK (nie „słaby POLLED" busy-wait).
- **LEKCJA SW2 — livelock pollingu**: re-assert MASTER_READY co retry (3×300ms) tworzył dryf faz okien M4F vs slave → nigdy się nie nakładały. Fix: **assert MASTER_READY RAZ + ciągły poll w jednym oknie** (duch gen1, który łapał zbocze SLAVE_READY IRQ natychmiast). User słusznie nakierował na gen1.
- **LEKCJA SW3 — `SPI_transfer arm failed`**: `SPICC26XXDMA_transfer` zwraca false gdy `currentTransaction != NULL` (zatkany poprzedni). Fix: `SPI_transferCancel()` defensywnie na wejściu `slave_do_transfer` + po timeoucie (samonaprawa); konsumpcja stale `EVENT_SPI_DONE` z callbacku cancela.
- **LEKCJA SW4 — „M4F won't stop"**: task SPI nie respektował `gbShutdown` → `Drivers_close()` ścigał się z transferem MCSPI → shutdown wisiał. Fix: `gbShutdown` globalne, task SPI kończy na nim (`vTaskDelete`), `doShutdown` czeka 1,2s przed `Drivers_close`.
- **Deploy gotchas**: `Deploy-M4F` przy zawieszonym M4F nie zatrzyma → recovery `rm /lib/firmware/.../ipc_echo_test_mcu2_0_release_strip.xer5f; sync; reboot` → po reboocie offline → deploy. **Po `cp` do CCS rób CLEAN rebuild** (stale-build, [[ccs-project-separate-from-repo]]).
- **TODO domknięcia**: (1) scenario A RX-routing edge-case — gdy slave uzbroił się dla scenario B (NODE_DATA) a M4F akurat śle NODE_CMD, slave nie routuje odebranego cmd (gubi go); (2) telemetria NODE_TELEMETRY → Linux DB; (3) `pending`/drenaż serii; (4) zdjąć/wyciszyć logi operacyjne SPI gdy dojrzeje. Następny duży krok: **remote access** (telefon→Go→`PushRules`/`MSG_NODE_CMD`).

### 2026-06-18 — kadencja hybrydowa + bucketing + wyrównany tick (level-trigger + feedback)
- **DECYZJA z userem (zmiana mojego planu)**: silnik **NIE edge-trigger** tylko **level-trigger + sprzężenie zwrotne ze stanu noda**. Reguła spełniona → wysyłamy żądanie **do skutku** co tick, ale **przed wysłaniem** porównujemy żądany stan z aktualnym stanem noda; jeśli node już raportuje pożądany stan → pomijamy. Chroni przed zgubioną 1. komendą (edge by ją stracił; level retransmituje aż node potwierdzi). To uogólnienie istniejącego guardu solar `pumpState`. `SEND_MESSAGE` nie ma stanu noda (telefon = HTTPS client pollujący zewnętrzną DB co min) → semantyka odłożona; na teraz leci co tick (decyzja przy remote-access). Zapisane w [[engine-eval-cadence]].
- **Kadencja hybrydowa + bucketing** (`engine.c`): `engine_evaluate(EngineEvalScope)` — `ENGINE_EVAL_TIME` (reguły z `COND_TIME` + zero-cond) na ticku minutowym, `ENGINE_EVAL_NODE` (`PARAMETER`/`DELTA`) na napływ danych. Reguła mieszana = oba kubełki. `rule_matches_scope()` filtruje → tick nie rusza reguł bez czasu (oszczędność CPU, wg usera).
- **Wyrównany tick do `:00`**: `engineTask` timeout `xQueueReceive` = `engine_ms_to_next_minute()` (przeliczany co pętlę → event w pół minuty NIE przesuwa ticku). **Liczone w MILISEKUNDACH** (nie całych sekundach) — integer-flooring sekund jest kruchy na granicy minuty (tik o włos przed `:00` czyta `second=59` → mikro-sen 1s → podwójny fire; bug złapany w teście 18.06, patrz niżej). Wymaga sub-sekundowego zegara → **`MSG_TIME_SYNC` rozszerzony o sekundy** (h,m,**s**; additive, 0x34 bez zmian; Go `SendTimeSync(h,m,s)`).
- **Zegar dolicza upływ** (`engine.c`): `engine_init(...,EngineClockFn)` wstrzykuje monotoniczny zegar (`ClockP_getTimeUsec`); `engine_set_time(h,m,s)` kotwiczy ścianę, `wall_now()` dolicza deltę. **Naprawia latentny bug**: wcześniej `g_time` był zamrożony na minucie ostatniego sync → realne „odpal o 17:32" nie działałoby bez ciągłego re-sync.
- **Wake-on-sync**: COMMS task po `MSG_TIME_SYNC` wrzuca sentinel `ENGINE_NODEIN_TIME_RESYNC` (0xEE, poza `NODE_*`, nigdy na drucie) na `gNodeInQueue` → ENGINE task budzi się, robi `EVAL_TIME` od razu i wyrównuje tick natychmiast (bez tego pierwszy tick po syncu byłby skośny — czekałby do końca trwającego snu).
- **Iteracje testu 18.06 (`-test fire-smoke`, sync 12:00:50, reguła TIME[10-14h] SEND_MESSAGE)** — UWAGA: fire'y trafiają w **ścianę czasu silnika** (12:01:00, 12:02:00…), NIE w realny czas Linuksa (timestampy logów to realny czas — mylące; w produkcji time-sync z NTP → realne `:00`):
  - (a) tick na całych sekundach (`60 - second`): jitter tika na granicy → raz `second=59` → mikro-sen 1s → **lock na `:01` + podwójny fire**. FIX: `engine_ms_to_next_minute()` liczy do `:00` w **ms** (sub-sekundowo).
  - (b) po ms-fix: ściany fire'ów `12:01:00.001` / `12:02:00.002` = **dokładnie `:00` ✓**, ale **podwójny fire na 1. granicy** (seq=1 i seq=2, 3ms). Próba 1: sentinel „tylko wybudza, bez eval" — podwójny fire ZOSTAŁ (wzór identyczny → albo firmware nieprzebudowany, albo inny mechanizm; nie ustalono jednoznacznie).
  - (c) **FIX kuloodporny — dedup TIME na minutę** (`engine.c`, `g_last_time_min`): `EVAL_TIME` odpala reguły **maks. raz na daną minutę ściany czasu** (klucz `h*60+m`), reset w `engine_init` i `engine_rules_commit`. To dokładnie model usera („co minutę"), niezależny od liczby triggerów. FreeRTOS timeout budzi się „nie wcześniej" niż zadany → ląduje na/po `:00`, klucz minuty zawsze świeży. Sentinel-bez-eval zostaje (obrona w głąb).
- **✅ ZWERYFIKOWANE NA ŻYWO (18.06, po czystym rebuildzie)** — DIAG-i potwierdziły: `wake=TIME (ms 9997)→FIRE→seq=1` (ściana 12:01:00), `wake=TIME (ms 59999)→seq=2` (12:02:00), `wake=TIME (ms 59998)→seq=3` (12:03:00). **Jeden** fire na każdej granicy `:00`, równo co 60s, `wake=RESYNC` = no-op, heartbeaty cicho. Pętla domknięta poprawnie. DIAG-i zdjęte.
- **Wyciszone heartbeaty** (zakomentowane = łatwe do odkomentowania): M4F — generyczny log RX pomija `MSG_PING` + „RX heartbeat PING" zakomentowany. Go (`protocol.go`) — `TX heartbeat PING` zakomentowany, log ACK pomija `MSG_PING`, generyczny RX pomija `MSG_ACK`. Logika nietknięta.
- **⚠️ LEKCJA — stale build w CCS**: podwójny fire utrzymywał się mimo poprawnego `engine.c` (dedup) → przyczyną był **niezrekompilowany `engine.o`**. Po zewnętrznej podmianie pliku (`cp` repo→CCS) Theia/CDT potrafi NIE wykryć zmiany i przebudować tylko część (`ipc_rpmsg_echo.o` świeży, `engine.o` stary). **Po `cp` do CCS rób `Project → Clean` / `gmake clean all`** — inaczej testujesz mix stary+nowy. Patrz [[ccs-project-separate-from-repo]].
- **Lint**: `engine.c`/`engine_rpmsg.c` czysto `-Wall -Wextra` (TI ARM clang). `ipc_rpmsg_echo.c` (SDK+FreeRTOS) + Go (cgo) — build w CCS / na bramce. Pliki zsynchronizowane repo→CCS.
- **Lesson**: time-sync nie wybudza śpiącego engine taska sam z siebie (idzie do COMMS, nie na `gNodeInQueue`) → bez sentinela pierwszy tick po syncu skośny. Sentinel na kolejce taska = czyste wybudzenie (kolejka to jedyny kanał blokady taska; notyfikacje FreeRTOS nie wybudzą `xQueueReceive`).

### 2026-06-17 — engine M4F (FreeRTOS multi-task) + enkoder rule-push Go
- **Przeczytany cały gen1 pod port**: `automationRules.{c,h}` (ewaluator), `coreTask.c` (folding NodesData + routing + getDeviceParameterValue + initExampleRules), `messageProtocol.h`, `spiTask.h`. Engine gen1 = czysty C bez zależności SDK → port ~1:1.
- **Rdzeń `m4f-firmware/engine.{c,h}`** (RTOS-agnostyczny): ewaluator (port 1:1, parytet D6 + guardy solar: dedup pumpState, sBuforTemp<0), `engine_update_node` (folding z coreTask), atomic swap reguł (double-buffer + indeks aktywny podmieniany atomowo + CRC32 IEEE = `hash/crc32` Go), `engine_set_time` (TODO wołacz). Akcja przez callback.
- **`m4f-firmware/engine_rpmsg.{c,h}`** (glue): dispatch `RULE_BEGIN/ITEM/COMMIT`+`NODE_CMD`, reportery `NODE_TELEMETRY/STATE/RULE_FIRED`. Sygnalizacja wyniku: `reply(MSG_ACK|MSG_ERROR, seq)` echo-seq (fire-and-forget) — Go koreluje po seq i natychmiast zwraca błąd na odrzucony COMMIT.
- **DECYZJA (zmiana): od razu FreeRTOS multi-task, NIE NoRTOS** (user: „nie ma sensu potem zmieniać, testy mają być adekwatne do RTOS"). `ipc_rpmsg_echo.c` przebudowany na 2 taski + kolejki (wzór gen1 coreTask, **lock-free bez mutexów**): ENGINE task (evaluate na `gNodeInQueue` 1s timeout / dane nodu; akcje→`gOutboxQueue`) + COMMS task (= dotychczasowa pętla; **jedyny właściciel** `RPMessage_send`+`gPendingAcks`; drenuje RX + outbox + retry). Recovery/heartbeat/hardfault/shutdown **nietknięte**. `xTaskCreateStatic`/`xQueueCreateStatic` (bez sterty). Punkty strojenia: `ENGINE_TASK_STACK_WORDS/PRIORITY`, `OUTBOX_DEPTH`, `NODEIN_DEPTH`.
- **Enkoder Go `go-services/rpmsg-service/rules.go`**: model Rule/Condition/Action + `encodeRule()` przez **cgo** (layout `AutomationRule` własności kompilatora C z `automation.h` → bajtowo identyczny z M4F, zero ryzyka offsetów). `PushRules()` = BEGIN/ITEM/COMMIT (reliable), CRC32 IEEE. `init()` asercje rozmiarów ABI (rozjazd → `abiOK=false` → push odmawia). Tryb `-test push-rules`. `protocol.go`: `sendReliableTyped()` + obsługa `MSG_ERROR` (korelacja po seq) + routing reporterów 0x40–0x42 jak EVENT.
- **Weryfikacja lokalna** (TI ARM clang z CCS, Cortex-M4F): `engine.c`+`engine_rpmsg.c` czysto `-Wall -Wextra`; preambuła cgo z `rules.go` (helpery na `automation.h`) czysto. Rozmiary: AutomationRule=196B (RULE_ITEM 198B≤480), RuleAction=68B (RULE_FIRED 70B≤128), MessageStruct/NodesData=44B. `ipc_rpmsg_echo.c` (TI SDK+FreeRTOS) i Go (cgo, brak toolchaina lokalnie) — build dopiero w CCS / na bramce.
- **Lesson**: lock-free przez kolejki (gen1 coreTask) > mutexy — ENGINE task nigdy nie woła `RPMessage_send`, tylko wrzuca na outbox; comms task jest jedynym właścicielem send+pending.
- **✅ ZWERYFIKOWANE NA ŻYWO (17.06)**: projekt CCS FreeRTOS (zaimportowany `ipc_rpmsg_echo_..._freertos_ti-arm-clang`) zbudowany + Deploy-M4F + Deploy-Go + `-test push-rules`. M4F boot: `Engine task started`. Push: `RX 0x30 → 0x31×3 (198B) → 0x32`, każda ACK; Go: `[rules] pushed 3 rules (crc32=0x0A83CF84) - M4F committed`. COMMIT→ACK (nie ERROR) = count+CRC32 OK → atomic swap. **Dowodzi wire-ABI cgo↔M4F bajtowo identyczne + lock-free swap działa na FreeRTOS.** Commit po zielonym (ten wpis).
- **Gotchas bring-up (helpery na laptopie, poza repo)**: (1) build M4F failował — `.bss` 77KB > 64KB DRAM (`g_rules` 39KB) → przeniesione do `M4F_DDR` przez sekcję `.bss.engine_rules` w `linker.cmd` projektu; (2) undefined `uart_echo_read_callback` (świeży syscfg ma debug UART w trybie CALLBACK) → no-op stub w `ipc_rpmsg_echo.c`; (3) cgo preambuła działa **per-plik** → `rules.go` musiał dostać `#include "protocol.h"` (nie tylko `automation.h`); (4) `Deploy-Go` bez `-ServiceName rpmsg-service` szedł na `protocol-test`; rozszerzony o kopiowanie wszystkich `shared/*.h` w pętli; (5) `Deploy-M4F` wskazywał stary projekt CCS → flashował stary firmware (objaw: `Unknown msg type 0x30`) → ścieżka zmieniona na nowy projekt freertos. Weryfikacja właściwego obrazu: boot banner `Engine task started`.
- **Build lokalny (bez IDE)**: `cd Debug && gmake -j8 all` z `C:/ti/ccs2051/...` przechodzi → `.appimage.hs_fs`. Lint przenośnych TU: `tiarmclang -fsyntax-only` ([[local-ti-arm-clang]]).
- **✅ time-sync + firing ZWERYFIKOWANE NA ŻYWO (17.06)**: dodany `MSG_TIME_SYNC 0x34` (Linux→M4F, u8 hour+minute → `engine_set_time`) — realna funkcja z roadmapy zamiast hacka. Go: `SendTimeSync()` + tryb `-test fire-smoke` (time-sync 12:00 → push reguły TIME[10-14h] SEND_MESSAGE→smartphone, bez solar-guard → fires). Wynik: `RULE_FIRED #1..N` co 1s, M4F `TX 0x42` + `-> node ... (SPI TODO)`. **Pełna pętla engine domknięta**: time-sync→eval TIME→akcja→outbox→comms→RULE_FIRED→Go.
- **⚠️ FOLLOW-UP (potwierdzony obserwacją)**: engine jest **level-triggered** — reguła odpala CO TICK (~1s) dopóki warunek prawdziwy. Solar `SET_RELAY` ma dedup `pumpState`, ale `SEND_MESSAGE`/inne akcje spamują. gen1 robił to samo, ale polling 60s maskował. Na produkcję: **edge-trigger / per-rule „fired-state"** (odpal raz na przejście false→true, ew. re-arm po wyjściu z warunku). Do zrobienia przy dojrzewaniu enginu (przed remote access / realnymi akcjami).
- **DECYZJA — kadencja ewaluacji (17.06, z userem)**: **hybryda**. Reguły **danych** (PARAMETER/DELTA) = event-driven (ewaluacja na napływ danych nodu z `gNodeInQueue`) + edge gdzie trzeba. Reguły **czasowe** (TIME) = periodyczny tick **co ~1 minutę** (jak gen1), NIE co 1s (za często, bez sensu — TIME ma granulację minutową). Zostajemy przy prostym timerze (timeout `xQueueReceive`), BEZ computed-deadline (przedwczesna optymalizacja; czas i tak jest „zdarzeniem", samo-naprawialny przy time-sync/zmianie reguł). Implementacyjnie: zmienić tick `pdMS_TO_TICKS(1000)`→`60000` w `engineTask`. Otwarte (zdecydować przy implementacji): wyrównanie do granicy minuty (`:00`) — dziś `MSG_TIME_SYNC` niesie tylko h+m bez sekund → albo dodać sekundy do time-sync i liczyć delay do następnej minuty, albo zaakceptować ≤60s fazy jak gen1.
- **Uwaga**: po `fire-smoke` reguła zostaje w RAM M4F i leci dalej co 1s (po rozłączeniu Go → RETRY/GIVEUP, nieszkodliwe). Czyści ją reboot M4F albo push pustego zestawu (`RULE_BEGIN(0)+COMMIT`).
- **Następne**: edge-trigger dedup (wyżej); potem SPI/CC1310 (zasili `gNodeInQueue` → `COND_PARAMETER` + telemetria + realne akcje), remote access (telefon/web → `PushRules`).

### 2026-06-16 — analiza starej bramki + architektura gen2 rozpisana
- **Przeanalizowany kod gen1** (CC3235+CC1310, ścieżki w pamięci [[legacy-gateway-code]]): engine (czysty C, ≤3 warunki AND, akcje relay/msg, polling 60s), JSON reguł (ręczny parser), MessageStruct (node↔gw), SPI handshake 2-liniowy (przeczytane `spi_master_task.c` + `spiTask.c`), RF EasyLink, FRAM dual-slot, telemetria HTTP do chmury.
- **Decyzje gen2** (z userem): engine na M4F/RTOS; **M4F=SPI master, CC1310=slave** (po analizie — determinizm enginu, backpressure, multi-drop; user słusznie obronił 2 linie handshake bo sterownik SPI wymaga uzbrojenia slave przed taktowaniem mastera — zalecenie TI z gen1); JSON tylko na Linuxie; FRAME_SIZE=128B; MAX_RULES=100.
- **Spisane `docs/ARCHITECTURE-GEN2.md`**: protokół SPI (handshake A/B, ramka 128B+CRC16+pending), RPMsg (nowe MSG 0x30–0x42, chunked rule push + atomic swap), 4 przepływy E2E, mapowanie portu gen1→gen2, otwarte tematy.
- **Lesson**: gen1 działa 2 lata bezawaryjnie (watchdog się nudzi) — bazujemy na sprawdzonych wzorach, nie wymyślamy od zera. Ulepszenia (CRC na SPI, pending-drenaż, event-driven engine, atomic rule swap) warstwowo na bazie gen1.

### 2026-06-16 — boot-accounting: klasyfikacja na markerze (nie journalu) + persistent journal naprawiony
- **Finding**: `journalctl -b -1` po reboocie → „no persistent journal was found". Root cause: `/var/log` to symlink do `/var/volatile/log` = **tmpfs** (Arago/Yocto) → `Storage=persistent` nie miało gdzie pisać → journald volatile. Konsekwencja: klasyfikacja resetów BEZ breadcrumb (opierała się na `-b -1`) zawsze spadała do „no previous-boot log" — twarde resety nierozróżniane.
- **Decyzja**: klasyfikacja **NIE może zależeć od journala**. Przepisana na trwały **marker clean-shutdown** w `/var/lib` (ext4): `boot-accounting.service` ma `ExecStop=touch /var/lib/bramka/clean_shutdown` (odpala się tylko przy czystym shutdownie). Logika: breadcrumb → `CONTROLLED go`; marker → `CONTROLLED clean shutdown`; brak obu → `UNEXPECTED hard reset` (panic refine z journala best-effort). Niezależne od journal-ordering. **Zweryfikowane**: manualny reboot → `CONTROLLED clean shutdown`.
- **Persistent journal (best-effort, diagnostyka) też naprawiony**: `.mount` unit był ignorowany (nazwa musi pasować do skanonikalizowanej ścieżki przez symlink — ordering hell). Zastąpiony **oneshot** `bramka-journal-bind.service` (`Before=systemd-journal-flush`, bind `/var/lib/journal` → `/var/volatile/log/journal`). **Zweryfikowane**: po reboocie `journalctl -b -1` działa (widać shutdown poprzedniego bootu). Mamy logi cross-boot.
- **Lesson**: na obrazie z volatile `/var/log` nie polegaj na persistent journalu dla core-feature; rób na trwałych plikach w `/var/lib`. Journal jako bonus przez oneshot bind (nie `.mount`).

### 2026-06-16 — non-root hardening (rpmsg-service least-privilege) + decyzja: engine na M4F/RTOS
- **Decyzja architektoniczna**: automation engine pójdzie na **M4F na RTOS** (nie A53/Linux) — determinizm, brak opóźnień round-tripa. M4F: NoRTOS→RTOS. Linux = UI/chmura/config. Warstwa C (DMSC) wraca jako prawdopodobna later (M4F trzyma żywe sterowanie). Roadmapa: engine → remote access → CC1310/SPI → bazy. Zapisane w pamięci [[near-term-roadmap]].
- **Non-root hardening (`modules/08-hardening.sh`)**: rpmsg-service jako user `bramka`, zero capów. (1) /dev/rpmsg* przez udev (grupa bramka, 0660); (2) reboot bez roota/polkit/setuid przez wzorzec **path-unit** — serwis pisze `/run/bramka/reboot-request`, `bramka-reboot.path` odpala `bramka-reboot.service` (root) robiący czysty `systemctl reboot`; (3) stan w `/var/lib/bramka`+`/run/bramka` (own bramka). Binarka `/root/bramka-services`→`/opt/bramka` (user bramka nie wejdzie do 0700 /root → wymaga zmiany celu w Deploy-Go).
- **Bugfix przy okazji**: `StartLimitBurst/IntervalSec` były w `[Service]` → nowoczesny systemd je IGNORUJE. Przeniesione do `[Unit]` (safety-net reboot-loop faktycznie zaczyna działać).
- Unit hardening: `NoNewPrivileges`, `CapabilityBoundingSet=` (puste), `ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`, `ReadWritePaths=/var/lib/bramka /run/bramka`. NIE `PrivateDevices` (musi widzieć /dev/rpmsg), NIE `MemoryDenyWriteExecute` (Go/cgo).
- **ZWERYFIKOWANE NA ŻYWO (16.06)**: po deploy (setup + Deploy-Go cel /opt/bramka + Install-GoService) — `User=bramka`, proces uid `bramka`, `/dev/rpmsg0` = `crw-rw---- root bramka`, połączony z M4F. Krok 7: `echo stop` → non-root serwis napisał trigger → `bramka-reboot.path` → czysty reboot → `bramka-reboots`: `CONTROLLED | go-peer-dead`. Cały P3 domknięty.
- **Spinner przy reboocie — WYJAŚNIONE (nie błąd)**: „A stop job is running for RPMsg..." to normalny progress shutdownu systemd (`???` = mielenie `<->` na konsoli szeregowej). `journalctl -b -1 -u rpmsg-service` potwierdził: SIGTERM → „Received signal: terminated" → „Transport Closed" → „Deactivated successfully" — wszystko w tej samej sekundzie. Serwis wychodzi natychmiast, zero hangu/SIGKILL. `os.Exit` niepotrzebny.

### 2026-06-16 — persistent restart counter + atrybucja przyczyny reboota
- **`modules/07-boot-accounting.sh`** (nowy, idempotentny): licznik bootów (`/var/lib/bramka/boot_count`), ledger (`boot_history.log`: `epoch | iso | boot#N | kind | cause`), alarm reboot-storm. Oneshot `bramka-boot-accounting.service` (per boot) + helper statusowy `bramka-reboots`.
- **Atrybucja „kto zrebootował"**: breadcrumb `/var/lib/bramka/reboot_reason` — Go `recoverByReboot()` zapisuje „go-peer-dead" PRZED sync. Service przy boocie: breadcrumb → CONTROLLED+cause+kasuje; brak → doklasyfikowanie z logu poprzedniego bootu: „Kernel panic" → panic/Warstwa D, sygnatura clean-shutdown → ręczny reboot, nic → hard reset (M4F SOC/HW wdt/power loss).
- **Persistent journald** (drop-in `/etc/systemd/journald.conf.d/bramka.conf`, `Storage=persistent`, `SystemMaxUse=50M`) — bez tego reboot kasuje dowody i nie ma jak czytać `-b -1`. Wymagane do detekcji panic + diagnostyki stormu.
- **Sparametryzowane** w `/etc/bramka/boot-accounting.conf` (`ALARM_ENABLED/ALARM_THRESHOLD/ALARM_WINDOW_HOURS`, default >3/24h). Setup tworzy z defaultami TYLKO gdy brak pliku → re-run nie nadpisuje ustawień admina/health-monitora. `ALARM_ENABLED=0` wyłącza alarm (liczenie leci dalej).
- **Zweryfikowane na bramce**: setup zainstalował moduł, persistent journal wstał. Krok 3-4 (INFO): po fixie „first boot" (był bug — `journalctl --list-boots | wc -l` liczyło nagłówek → fałszywy hard-reset; teraz gate na istnieniu `-b -1`) pokazuje `INFO | no previous-boot log`. Krok 5 (atrybucja): `echo stop` → Go `recoverByReboot` (breadcrumb) → reboot → `boot#1 | CONTROLLED | go-peer-dead (...)` ✓, breadcrumb skonsumowany.
- **Uwaga test**: oneshot+RemainAfterExit → ręczny re-test przez `systemctl restart` (nie `start`, to no-op po 1. boocie).
- **Caveat**: timestampy zależą od zegara (RTC-less + brak NTP offline może je przesunąć). Verdin ma RTC. Reset-cause register (rozróżnienie M4F-SOC-reset vs power-loss) = TODO long-term. Nie-zweryfikowane na żywo (mechanizm ten sam): klasyfikacja kernel-panic i clean-shutdown (manual), realne odpalenie alarmu >3/24h.

### 2026-06-16 — panic_on_oops=1 (domknięcie luki oops)
- **`modules/06-kernel-panic.sh`** (nowy, idempotentny): drop-in `/etc/sysctl.d/60-bramka-panic.conf` z `kernel.panic_on_oops = 1` + `sysctl -w` (apply od razu, też persistent). Podłącza się sam (setup.sh odpala moduły alfabetycznie).
- **Dlaczego**: domyślnie oops (NULL deref/BUG) ubija wątek i jedzie dalej w niespójnym stanie — systemd żyje, dalej klepie /dev/watchdog0 → Warstwa D nie zadziała, Warstwa A łapie tylko Go. Luka: pokaleczony kernel bez panic = bramka „żywa" ale martwa. `panic_on_oops=1` → oops staje się panic → łapie Warstwa D (zweryfikowane testem `echo c`). Fail-fast spójny z designem.
- **NIE ruszono `kernel.panic`** (delay reboota po panic) — recovery zweryfikowane na panic=0 → HW watchdog. Opcja na przyszłość: `kernel.panic = 10` w tym samym drop-inie (szybszy reboot, HW watchdog backup).
- **Zweryfikowane na bramce**: `setup.sh` przeleciał czysto (wszystkie 6 modułów idempotentne), `sysctl kernel.panic_on_oops` = 1, drop-in `/etc/sysctl.d/60-bramka-panic.conf` obecny (persistent).

### 2026-06-16 — m4f-reload service-aware (skutek uboczny P2)
- **Problem**: po P2 fast-fail nie dało się robić `Deploy-M4F`/`m4f-reload` przy działającym serwisie — `echo stop` M4F = device-gone = natychmiastowy reboot bramki w trakcie podmiany firmware.
- **Fix w `modules/02-tools.sh`** (generowany `m4f-reload`): przed `echo stop` M4F zatrzymuje `rpmsg-service` (gdy aktywny), po starcie M4F restartuje przez `trap restore_service EXIT` (też przy błędzie; restart tylko jeśli był aktywny). Pre-flight checki przed stopem serwisu → trywialny błąd nie rusza serwisu. Bonus: zwolniony device = `echo stop` M4F nie potrzebuje fallbacku `pkill/fuser`.
- **Decyzja architektoniczna**: logika w `m4f-reload` (repo, wersjonowany), NIE w `Deploy-M4F` (PowerShell, poza repo) → każdy caller bezpieczny (też ręczny SSH), `Deploy-M4F` bez zmian.
- **ZWERYFIKOWANE NA ŻYWO**: po `git pull && sudo ./setup.sh` na bramce, `Deploy-M4F` przy działającym serwisie pokazał `[*] Stopping rpmsg-service` → `[1/4]..[4/4]` M4F → `[*] Restarting rpmsg-service`, **bez reboota bramki** (SSH trzymał). `m4f-watch` po deployu czysty.

### 2026-06-16 — P2 transport fast-fail (device-gone) PASS + decyzja SD
- **Decyzja: NIE kupujemy industrial SD** — produkcja na eMMC (Verdin), dev na zapasowych kartach consumer (uszkodzona = re-flash). Zdjęte z backlogu. Zapisane do pamięci.
- **P2 zweryfikowane**: brak in-process reconnect by design (device-gone → reboot → świeży proces re-detektuje; `findM4FChrdev` number-agnostic po ścieżce HW `5000000.m4fss`).
- **P2 hardening fast-fail ZROBIONE + PASS**: `transport.go` kanał `DeviceGone()` (`signalDeviceGone()` w `readerLoop`, idempotentny `sync.Once`) → `protocol.go` `deviceGoneWatcher()` (4. goroutine) → `signalPeerDead()`. Device-gone = natychmiastowy PEER DEAD, bez czekania ~9s na heartbeat. Akcja końcowa ta sama (clean reboot).
- **Test PASS**: serwis `-test hello` (connected, nie pod systemd) → `echo stop > /sys/class/remoteproc/remoteproc0/state` na zdrowym M4F → `read /dev/rpmsg0: broken pipe` → `TRANSPORT device gone` → `Issuing systemctl reboot` w **~3ms** (heartbeat nie drgnął). Bramka wróciła sama, M4F auto-load z `/lib/firmware`.
- **Lesson**: ani crash-m4f (SOC reset, Linux pada razem), ani silent-hang (`cpsid i`, remoteproc dalej „running") NIE wyzwalają ścieżki device-gone. Wyzwala ją dopiero rozbiórka rpmsg po stronie kernela (`echo stop`/`m4f-reload` na zdrowym M4F). `m4f-reload` wymaga pliku firmware (hot-swap) — do samego testu wystarczy `echo stop`.
- **M4F EVENT scaffolding cleanup (P3) — zrobione w repo**: w `doPeriodicTick` (`m4f-firmware/ipc_rpmsg_echo.c`) zakomentowany log `Tick #%u` (1Hz spam w m4f-watch) i testowy EVENT co 10s (leciał dopóki `gLinuxEndpoint != 0` = wiecznie → po stopie Linuxa GIVEUP/„ACK for unknown"). `sendEvent()` bez zmian, szkielet demo zostawiony zakomentowany. ⚠️ Czeka na sync repo→CCS Theia + rebuild + Deploy-M4F (firmware buduje się z osobnej kopii w CCS, nie z repo).

### 2026-06-15 (noc, finał+++) — kernel panic PASS, CAŁA macierz recovery zweryfikowana
- **Kernel panic test PASS**: `echo c > /proc/sysrq-trigger` z `kernel.panic=0` (kernel zamarł, brak auto-reboot) → bramka wróciła sama (`uptime: up 0 min`) → **dowód że Warstwa D (HW watchdog) zresetowała SoC**. `Kernel panic - not syncing: sysrq triggered crash` w logu.
- **Wszystkie 4 scenariusze recovery zweryfikowane na żywo**: M4F silent-hang (clean reboot) ✅, M4F hardfault (SOC reset) ✅, Go hang (systemd Warstwa A) ✅, Linux panic (HW watchdog Warstwa D) ✅. Każdy: auto-recovery bez ręcznej interwencji.
- Recovery architecture domknięta. Pozostało: industrial SD przed produkcją, Warstwa C (DMSC, long-term), backlog P2/P3.

### 2026-06-15 (noc, finał++) — crash-m4f PASS, komplet recovery
- **`crash-m4f` PASS**: `TX DEBUG_CRASH` → M4F hardfault → `SOC_generateSwWarmResetMcuDomain` → pełny reset SoC (`uptime: up 0 min`) → bramka wróciła sama ~70s, serwis auto-reconnect, heartbeat OK, bez korupcji FS. Twardy reset (bez sync Linuxa) — zrobiony `sync` przed; consumer SD przeżyła (zaakceptowane ryzyko).
- **Komplet scenariuszy reset-recovery zweryfikowany**: silent-hang (clean reboot) ✅ + crash-m4f (HW SoC reset) ✅. Obie ścieżki: auto-recovery bez ręcznej interwencji.
- Poprawiony mylący tekst w `runCrashM4FTest` (drukował „remoteproc reload" — faktycznie pełny reset SoC).
- Wszystkie 3 crash testy z planu zaliczone: heartbeat-busy ✅, silent-hang ✅, crash-m4f ✅.

### 2026-06-15 (noc, finał) — silent-hang recovery PASS
- **Re-test `silent-hang` po fixie: PASS.** PEER DEAD w 7.95s → `recoverByReboot` → `systemctl reboot` (M4F log: „The system will reboot now!") → bramka wróciła SAMA w ~70s, `rpmsg-service active (running)`, heartbeat tyka. Dokładne przeciwieństwo poprzedniego wedge'a SoC.
- Cała architektura recovery działa end-to-end: detekcja (Go heartbeat) + akcja (clean reboot) + auto-start po boocie (cold-boot fix) + backup (Warstwa D).
- Test zrobiony na consumer SD (GOODRAM) — clean reboot przeżyła; industrial SD nadal TODO przed `crash-m4f` i produkcją.
- **Priorytet 1 NOWY zamknięty** (recovery fix + Warstwa D + docs cleanup, commity cb61155 + ad80a8e).

### 2026-06-15 (noc, najpóźniej) — cold-boot fix + recovery fix
- **Warstwa D zweryfikowana** po reboocie: `lsof /dev/watchdog0` → systemd (PID 1) trzyma, `wdctl` busy = OK. Ostatnia linia obrony wróciła.
- **Cold-boot race fix** (commit 18b7b6e): `rpmsg-service` padał po reboocie (`no rpmsg_chrdev` → StartLimitBurst). `transport.go` `waitForM4FChrdev` czeka na `/dev/rpmsg*` do 20s. Zweryfikowane: wstaje sam po reboocie.
- **Recovery silent-hang fix** (kod): `forceM4FReload` (remoteproc stop, wieszał SoC) → `recoverByReboot()` (sync + `systemctl reboot`, backup Warstwa D). Live re-test `silent-hang` odłożony do industrial SD.

### 2026-06-15 (noc, późno) — silent-hang FAIL + watchdog fix
- **Crash test `silent-hang`: detekcja OK, recovery PADŁ.** Go wykrył (GIVEUP T+9s), ale `forceM4FReload` (remoteproc stop na M4F z `cpsid i`) zawiesił cały SoC → ręczny power cycle. Warstwa D (HW watchdog) NIE zadziałała.
- **Root cause Warstwy D**: `system/configure-watchdog.sh` nigdy nie był wołany przez setup (brak modułu). Świeży obraz = zero HW watchdog. → dodany `modules/05-watchdog.sh` + posprzątany rozgrzebany `setup.sh` (commit dabee5b).
- **Potwierdzone**: brak per-core reset M4F na AM62 — jedyna poprawna reakcja na martwy M4F to pełny reset SoC. Recovery silent-hang → clean reboot (P1 nowy).
- Bonus: stary `setup.sh` w repo był zepsuty (zdublowana pętla + notatki edycyjne łamiące parser) → bramka działała na innej wersji (dryf repo↔device).

### 2026-06-15 (noc) — Crash testy start
- Priorytet 1 zacommitowany + push na main (`a0d5565`).
- **Crash test `heartbeat-busy` PASS**: 12× DATA co 2s, zero `TX heartbeat PING` po obu stronach. Regresja Priorytetu 1 potwierdzona.
- Zaobserwowany (nie-błąd) noise: M4F EVENT-co-10s daje GIVEUP gdy Linux odłączony → dopisane do Priorytet 3 cleanup.
- Następny: `silent-hang` (uwaga: firmware `cpsid i`+`while(1)`, recovery przez m4f-reload niepewne — komentarz w fw mówi że może wymagać full reset; consumer SD ryzyko).

### 2026-06-15 (wieczór) — Priorytet 1 done
- **M4F**: usunięty heartbeat-init (opcja A) — `sendHeartbeatPing()`, `doHeartbeatCheck()`, globale `gLastRxTimeUs`/`gPingInFlight`/`HEARTBEAT_IDLE_US`, blok `MSG_PING` w `processEventRetries`. Zostaje reply `sendAck()` na PING od Go.
- **M4F bugfix**: `case MSG_PING` reply `MSG_PONG` → `sendAck()` (deprecated PONG łamał heartbeat Go).
- **Go**: dodany `helloWithRetry()` (exp backoff 1/2/4/8s) podpięty w `runHelloTest`.
- Zweryfikowane na bramce (Deploy-M4F + Deploy-Go): M4F nie pinguje, Go ma retry, idle stabilny bez restartów, RTT 4-6ms.
- **Lesson learned: projekt CCS Theia M4F to OSOBNA kopia źródeł, niezależna od repo.** Edycja repo `m4f-firmware/*.c` NIE trafia do `Deploy-M4F` bez ręcznego skopiowania do projektu CCS — build leci z CCS, nie z repo. Zawsze sync repo→CCS przed rebuildem.
- **Lesson: czyść terminal przed zbieraniem logów** — stary boot log (relative time od remoteproc reload) zmylił diagnozę „repo ≠ device".
- **TODO sprzątanie (nice-to-have)**: w Go `case MSG_PING` (reply ACK) i `case MSG_PONG` to teraz martwy kod (M4F już nie inicjuje PINGów). `sendHeartbeatPing()` w Go ZOSTAJE (kierunek Linux→M4F).

### 2026-06-15
- Disaster recovery z padniętej karty SD (crash testy + Restart=always = bootloop, FAT corruption)
- Świeży flash karty, recovery przez `git clone + setup.sh`
- Dodany moduł `04-go.sh` (Arago nie ma Go, trzeba instalować z tarball)
- Dodany `.gitattributes` dla LF enforcement
- Heartbeat działa bidirectional ale **decyzja**: usuwamy z M4F (Priorytet 1)
- Analiza 7 luk w recovery — Priorytety 1/2/3 zdefiniowane
- Kupić Industrial SD card (Samsung Pro Endurance albo SanDisk Industrial XI) — TODO

### 2026-06-08..14
- Setup SK-AM62B-P1, SDK 12.00, CCS Theia, JTAG XDS110
- Binary protocol z CRC16-CCITT, HELLO/HELLO_ACK, ACK+retry, idempotency
- Watchdog systemd (Warstwa A) działa, watchdog HW Linux (Warstwa D) działa
- Custom M4F hardfault handler → SOC_generateSwWarmResetMcuDomain
- Per-core reset M4F NIE supported na AM62 (testy potwierdziły)
