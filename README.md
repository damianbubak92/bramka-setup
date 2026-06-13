# Bramka IoT - Setup & Development Repository

Repozytorium konfiguracji i kodu dla bramki IoT na AM62 SoC (SK-AM62B-P1 dev kit).

## Architektura

- **SoC**: TI AM62 (Cortex-A53 quad + Cortex-M4F + Cortex-R5F)
- **Linux**: Arago 2025.01 (kernel 6.18.13-ti, ARM64)
- **Komunikacja**: Linux ↔ M4F przez RPMsg (binary protocol z CRC16)
- **Język aplikacji Linux**: Go 1.26.4
- **Firmware M4F**: TI MCU+ SDK 12.00, NoRTOS

## Struktura repo
bramka-setup/
├── setup.sh                      # Główny skrypt instalacyjny
├── modules/                      # Moduły setup (network, tools, m4f, etc.)
├── shared/
│   └── protocol.h                # Wspólny header protokołu (M4F + Go)
├── m4f-firmware/
│   ├── ipc_rpmsg_echo.c          # M4F firmware source
│   └── protocol.h                # Kopia (sync z shared/)
├── go-services/
│   └── protocol-test/            # Go service implementujący protokół
│       └── main.go
├── examples/                     # Wzorce kodu do referencji
└── docs/                         # Dokumentacja
├── M4F_SHUTDOWN.md
├── PROTOCOL.md               # NEW: opis binary protocol
└── TROUBLESHOOTING.md        # NEW: lessons learned

## Quick start

### 1. Instalacja bramki (od zera)
```bash
git clone <repo>
cd bramka-setup
./setup.sh
```

### 2. Workflow developerski

**M4F firmware** (z laptopa, PowerShell):
```powershell
# Build w CCS Theia (Ctrl+Shift+B)
Deploy-M4F                         # 5s logów
Deploy-M4F -LogForever             # logi do Ctrl+C
Deploy-M4F -NoLogs                 # bez logów
```

**Go service** (z laptopa, PowerShell):
```powershell
Deploy-Go -ServiceName protocol-test -Build -Run
```

### 3. Monitor

```bash
ssh root@bramka-01
m4f-watch                          # live M4F trace
```

## Status (v3)

- ✅ Setup script (Linux configuration, network, tools, M4F firmware)
- ✅ M4F graceful shutdown handler (hot-swap firmware bez reboot)
- ✅ Go runtime (cgo działa, łączy się z shared C header)
- ✅ Binary protocol (CRC16-CCITT, packed wire format, big-endian)
- ✅ HELLO/HELLO_ACK exchange w binary protocol
- ⏳ State machine z ACK retry (next)
- ⏳ Smart heartbeat (next)
- ⏳ Watchdog architecture (later)
- ⏳ systemd unit dla Go service (later)
- ⏳ Yocto build (production target)