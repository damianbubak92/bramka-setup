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

```
bramka-setup/
├── README.md                    # Dla ludzi - co to za projekt
├── CLAUDE.md                    # Dla Claude Code - operating manual (TEN PLIK)
├── setup.sh                     # Główny orchestrator bootstrap bramki
├── config.sh                    # Zmienne (MAC, hostname, paths) - per-deployment
├── config.sh.example            # Template do skopiowania
├── .gitattributes               # *.sh text eol=lf (CRITICAL dla Windows devs)
├── modules/
│   ├── 01-network.sh            # Stały MAC + hostname (no EEPROM workaround)
│   ├── 02-tools.sh              # m4f-watch, m4f-reload do /usr/bin
│   ├── 03-m4f-firmware.sh       # Backup default firmware do .original
│   ├── 04-go.sh                 # Install Go toolchain (Arago nie ma)
│   └── 99-cleanup.sh            # Final verification + next steps log
├── go-services/
│   └── rpmsg-service/
│       ├── main.go              # Entry point + test modes
│       ├── protocol.go          # Binary protocol + heartbeat
│       ├── transport.go         # /dev/rpmsg* IO
│       ├── systemd_notify.go    # sd_notify dla Type=notify
│       └── go.mod
├── shared/
│   └── protocol.h               # Wspólne dla M4F (C) i Go (cgo) - msg types, structs
├── systemd/
│   └── rpmsg-service.service    # Unit file
└── tools/
    ├── m4f-watch                # Live trace M4F przez remoteproc
    └── m4f-reload               # stop+start M4F firmware (sysfs)
```

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
| **M4F silent hang** | Go heartbeat (5s idle + 4s retries = 9s) | Go pisze `stop`/`start` do remoteproc | ~10-12s |
| **M4F hardfault** | M4F custom hardfault handler | `SOC_generateSwWarmResetMcuDomain` (cały SoC reset) | ~15-20s |
| **Linux kernel panic** | systemd HW watchdog `/dev/watchdog` | Reset całego SoC (60s timeout) | ~60-75s |
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
- **ALWAYS** `StartLimitBurst=3` + `StartLimitIntervalSec=60` (safety net przed reboot loops)
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

### Disaster recovery procedure
1. Świeży flash karty SD (Etcher na Win 10, sprawdzony workflow)
2. `git clone https://github.com/damianbubak92/bramka-setup`
3. `cd bramka-setup && sudo ./setup.sh` (network + tools + M4F backup + Go install)
4. `reboot`
5. Z laptopa: `Deploy-M4F` (custom firmware) + `Deploy-Go -Build` + `Install-GoService`
6. Plus DHCP reservation w routerze (MAC `22:F4:99:37:A5:12` → IP `192.168.2.170`)

## Current Status (UPDATE PO KAŻDEJ SESJI)

### ✅ Done
- Setup.sh działa po świeżym flash (network, tools, M4F backup, Go install)
- Custom M4F firmware z heartbeat working (deploy manual via Deploy-M4F)
- Go service zbudowany z heartbeat patches
- Systemd unit safe (`-test hello`, `Restart=on-failure`, `StartLimitBurst=3`)
- HELLO/HELLO_ACK handshake działa, heartbeat tickuje (RTT 4-6ms)
- Watchdog systemd kickowany co 5s
- Repo z executable scripts + LF endings
- DHCP reservation w routerze
- **Priorytet 1 (15.06.2026, zweryfikowane na bramce)**: M4F heartbeat-init usunięty (opcja A) — M4F nie inicjuje PINGów, tylko odpowiada `sendAck()` na PING od Go (jednokierunkowy heartbeat Linux→M4F). `m4f-watch` potwierdza: zero `TX heartbeat PING`, jest `RX heartbeat PING - replying ACK`.
- **Priorytet 1 (15.06.2026, zweryfikowane)**: Go HELLO retry z exponential backoff (1/2/4/8s, 5 prób) w `helloWithRetry()` (`go-services/rpmsg-service/main.go`). Log startowy: `Sending HELLO (with retry)...`.
- **Bugfix (15.06.2026)**: M4F `case MSG_PING` odpowiadał deprecated `MSG_PONG` → zmienione na `sendAck()`. Go czeka na `MSG_ACK`, więc PONG groził fałszywym `PEER DEAD` → restart loop.
- `protocol.h` zsynchronizowane (`shared/` == `m4f-firmware/`).

### ⏳ Pending — Priorytet 2 (przed produkcją)
- **HW watchdog**: sprawdzić i skonfigurować `/dev/watchdog` (`RuntimeWatchdogSec=30s` w systemd, `panic_on_oops=1`)
- **Transport device discovery**: zweryfikować że `transport.go` re-detect `/dev/rpmsg*` przy reconnect (nie cache)

### ⏳ Pending — Priorytet 3 (nice-to-have)
- Last-resort `systemctl reboot` w `forceM4FReload` jeśli sysfs stop/start padnie
- Persistent restart counter (`/var/lib/bramka/restart_count` + timestamp) z alarmem >3/dzień
- Dedicated `m4f-reload.service` (Type=oneshot) dla security hardening (Go może być non-root)

### ⏳ Pending — Crash testy (NASTĘPNY KROK — Priorytet 1 done)
1. **heartbeat-busy** — regresja, weryfikacja że busy traffic nie wywołuje PINGów
2. **silent-hang** — core test, wykrycie + forceM4FReload + recovery
3. **crash-m4f** — re-verify hardfault → SoC reset

Wszystkie **interaktywnie**, NIGDY autonomicznie pod systemd. Pattern:
```powershell
ssh root@bramka "systemctl stop rpmsg-service"
ssh -t root@bramka "/root/bramka-services/rpmsg-service/rpmsg-service -test <mode>"
ssh root@bramka "systemctl start rpmsg-service"
```

### ⏳ Pending — Long-term (poza obecnym sprintem)
- **Warstwa C (DMSC reset)**: M4F triggeruje DMSC reset tylko A53 cluster (Linux), bez resetu siebie. Wymaga TI-SCI API research w MCU+ SDK 12.00 (`Sciclient_procBootRequestProcessor` + reset sequence dla TISCI_DEV_A53SS0_0..3). Fallback po 30s: pełny SoC reset.
- **OTA updates**: RAUC dla Linux (A/B partitions na eMMC), custom dla M4F (A/B w OSPI + ECDSA P-256 signing)
- **Bazy**: SQLite (config, na eMMC) + InfluxDB/TimescaleDB (telemetria, na M.2 NVMe industrial)
- **Health monitoring service**: eMMC wear `/sys/block/mmcblk0/device/life_time`, alarmy gdy zużycie >70%
- **Carrier board production**: supercap PLP + PWR_FAIL GPIO + kernel sync on signal

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
- **Karta SD**: GOODRAM 64GB Class 10 UHS-I — consumer-grade, **rozsypała się po crash testach**, do wymiany na industrial

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
```

## Session Log (NEWEST FIRST)

> Format: data — co zrobione, ważne decyzje, lessons learned

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
