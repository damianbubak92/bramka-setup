# Bramka IoT - Setup & Development Repository

Repozytorium konfiguracji i kodu dla bramki IoT na AM62 SoC (SK-AM62B-P1 dev kit).

## Architektura

- **SoC**: TI AM62 (Cortex-A53 quad + Cortex-M4F + Cortex-R5F)
- **Linux**: Arago 2025.01 (kernel 6.18.13-ti, ARM64)
- **Komunikacja**: Linux ↔ M4F przez RPMsg (binary protocol z CRC16)
- **Język aplikacji Linux**: Go 1.23.x (instalowany przez `Gateway/Setup/modules/04-go.sh`)
- **Firmware M4F**: TI MCU+ SDK 12.00, NoRTOS

## Struktura repo

> Ten README opisuje **wczesny stan (sama bramka, RPMsg)**. Projekt urósł do
> **monorepo `SmartHome`** (engine M4F, CC1310 RF, nody, apka Android) — pełny
> aktualny stan, konwencje i Session Log w **`CLAUDE.md`**. (Repo na GitHub wciąż
> nazwany `bramka-setup`; może zostać przemianowany na `SmartHome`.)

```
SmartHome/                (zmigrowane z bramka-setup 2026-06-29)
├── Gateway/              # bramka (A53 Linux + M4F + CC1310 RF)
│   ├── Software/         #   Go: rpmsg-service (RPMsg bridge + HTTP/WS API, cgo)
│   ├── Firmware/         #   CCS: M4F (gateway_m4f) + CC1310 (gateway_cc1310)
│   ├── Setup/            #   setup.sh, modules/, systemd/, tools/, config.sh
│   └── Hardware/         #   carrier board (TODO)
├── Nodes/                # TempHumNode/{Firmware = temphum_node, Hardware}, Light/Solar (szkielety)
├── Apps/                 # MobileApp/AndroidApp/SmartHomeV2 (Android Studio); iOS/, WebApp/ (TODO)
├── Shared/               # Protocol/ (node_protocol.h, protocol.h, spi_frame.h — SINGLE SOURCE), KiCadLib/
└── Docs/
```

## Quick start

### 1. Instalacja bramki (od zera)
```bash
# 1. Świeży flash karty SD (Etcher)
# 2. SSH do bramki:
git clone https://github.com/damianbubak92/bramka-setup
cd bramka-setup/Gateway/Setup
sudo ./setup.sh    # network + tools + m4f firmware backup + GO INSTALL
reboot
# 3. Z laptopa:
Deploy-M4F                                            # custom M4F firmware
Deploy-Go -ServiceName rpmsg-service -Build           # build Go binary
Install-GoService -ServiceName rpmsg-service          # systemd unit
```

### 2. Workflow developerski

**M4F firmware** (z laptopa, PowerShell):
```powershell
# Build w CCS Theia (Ctrl+Shift+B)
Deploy-M4F                         # 5s logów
Deploy-M4F -LogForever             # logi do Ctrl+C
Deploy-M4F -NoLogs                 # bez logów
```
> `m4f-reload` (woła go `Deploy-M4F`) sam zatrzymuje `rpmsg-service` przed
> `echo stop` M4F i restartuje po starcie. Bez tego serwis wykryłby zniknięcie
> `/dev/rpmsg` jako PEER DEAD i zrobił reboot bramki w trakcie podmiany firmware.

**Go service** (z laptopa, PowerShell):
```powershell
Deploy-Go -ServiceName protocol-test -Build -Run
```

### 3. Monitor

```bash
ssh root@bramka-01
m4f-watch                          # live M4F trace
```

## Status (v5)

- ✅ Setup script (Linux configuration, network, tools, M4F firmware)
- ✅ M4F graceful shutdown handler (hot-swap firmware bez reboot)
- ✅ Go runtime (cgo działa, łączy się z shared C header)
- ✅ Binary protocol (CRC16-CCITT, packed wire format, big-endian)
- ✅ HELLO/HELLO_ACK exchange w binary protocol
- ✅ State machine: DISCONNECTED → HELLO_SENT → CONNECTED → DEAD
- ✅ Reliable DATA delivery (ACK + 3 retries z 1s timeout)
- ✅ Idempotency check (duplicate detection w M4F)
- ✅ EVENT (M4F→Linux autonomous events z ACK + retry)
- ✅ Bidirectional reliable messaging w pełni działa (~5ms RTT)
- ✅ **Warstwa A**: systemd watchdog dla Go service (`Restart=on-failure` + `StartLimitBurst=3`)
- ⏸️ **Warstwa B**: M4F self-watchdog (RTI) - skipped, do later
- ⏳ **Warstwa C**: M4F → Linux reset via DMSC
- ✅ **Warstwa D**: Linux HW watchdog (`/dev/watchdog` via systemd, `RuntimeWatchdogSec=30`) - konfigurowany przez `modules/05-watchdog.sh` (był zgubiony przy re-flashu, przywrócony 15.06.2026)
- ✅ **panic_on_oops=1** (`modules/06-kernel-panic.sh`): oops → pełny panic → łapie Warstwa D. Domyka lukę "oops kaleczy kernel ale systemd dalej klepie watchdog → nic nie łapie"
- ✅ **Boot accounting** (`modules/07-boot-accounting.sh`): licznik bootów + atrybucja przyczyny (breadcrumb Go / kernel panic / clean-shutdown / hard reset) + alarm reboot-storm (sparametryzowany `/etc/bramka/boot-accounting.conf`, default >3/24h). Persistent journald (50M). Podgląd: `bramka-reboots`
- ✅ **Non-root hardening** (`modules/08-hardening.sh`): `rpmsg-service` jako user `bramka` (zero capabilities). Device przez udev (grupa `bramka`), reboot przez path-unit (`bramka-reboot.path/.service`, bez roota/polkit). Binarka w `/opt/bramka` (Deploy-Go: cel `/opt/bramka`)
- ✅ **Heartbeat jednokierunkowy Linux→M4F**: Go pinguje M4F co 5s idle; brak odpowiedzi (~9s) = M4F martwy → **clean reboot** (`recoverByReboot`). M4F-initiated heartbeat usunięty (opcja A). Brak per-core reset M4F na AM62 - jedyna recovery to pełny reset SoC.
- ✅ **Cold-boot race fix**: serwis czeka na `/dev/rpmsg*` (`waitForM4FChrdev`) zamiast paść gdy M4F jeszcze się podnosi
- ⏳ Yocto build - later