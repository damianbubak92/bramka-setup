# Bramka Setup - AM62 IoT Gateway Development

Production-quality automation and reference code for AM62-based IoT bramka
(gateway) development. Includes idempotent setup scripts, debug tools, and
working firmware templates derived from real-world development experience.

## Quick start

### Prerequisites

- SK-AM62B-P1 dev board (or compatible AM62 SoM)
- TI SDK Linux image flashed to SD card
- Network access between bramka and your laptop
- SSH key or password for `root@bramka`

### On a new bramka

1. Flash fresh TI SDK Linux image to SD card (unpack `.xz` manually before
   flashing - Etcher has bugs with streaming decompression).
2. Boot bramka, find its IP (router admin panel or `am62xx-evm.local` via mDNS).
3. Test SSH:
   ```bash
   ssh root@<bramka-ip>     # default password empty
   ```

### From your laptop

4. Clone this repo:
   ```bash
   git clone <your-repo-url> bramka-setup
   cd bramka-setup
   ```

5. Edit `config.sh` for this specific bramka:
   ```bash
   BRAMKA_MAC="22:F4:99:37:A5:12"    # unique per bramka!
   BRAMKA_HOSTNAME="bramka-01"
   ```

6. SCP to bramka:
   ```bash
   scp -r . root@<bramka-ip>:/root/bramka-setup
   ```

7. Run setup on bramka:
   ```bash
   ssh root@<bramka-ip>
   cd /root/bramka-setup
   chmod +x setup.sh modules/*.sh
   ./setup.sh
   ```

8. Reboot bramka. Configure DHCP reservation in router for the MAC.

9. SSH back via the reserved IP - everything works.

## What's in this repo

```
bramka-setup/
├── README.md              <-- you are here
├── setup.sh               <-- main orchestrator
├── config.sh              <-- per-bramka config (MAC, hostname)
├── .gitignore
├── modules/               <-- numbered setup modules
│   ├── 01-network.sh      <-- persistent MAC + hostname
│   ├── 02-tools.sh        <-- m4f-watch, m4f-reload installation
│   ├── 03-m4f-firmware.sh <-- backup default M4F firmware
│   └── 99-cleanup.sh      <-- verification + next steps
├── docs/
│   ├── M4F_SHUTDOWN.md    <-- CRITICAL: shutdown handler requirement
│   └── TROUBLESHOOTING.md <-- common issues + diagnosis
└── examples/
    └── m4f-baseline/
        ├── ipc_rpmsg_echo.c <-- working firmware template
        └── README.md      <-- how to use template
```

## Setup modules

| Module | Purpose |
|--------|---------|
| `01-network.sh` | Persistent MAC via systemd-networkd `.link` unit. Solves AM62's no-EEPROM problem (kernel generates random MAC each boot otherwise). |
| `02-tools.sh` | Installs `m4f-watch` (smart trace0 monitor) and `m4f-reload` (firmware hot-swap). |
| `03-m4f-firmware.sh` | Backs up default TI M4F firmware. Optional dev-mode auto-stop. |
| `99-cleanup.sh` | Verification and next-steps info. |

## Installed tools

### `m4f-watch [polling_seconds]`

Live tail of M4F trace buffer with smart deduplication. Default polling 0.5s.

```bash
m4f-watch          # default 500ms polling
m4f-watch 1        # 1s polling (less CPU)
m4f-watch 0.1      # 100ms polling (snappier)
```

On start, shows only the newest entry as anchor. Then appends only new
entries (deduplicated by integer microsecond timestamp). Handles circular
buffer wrap correctly.

### `m4f-reload [path_to_firmware.out]`

Hot-swap M4F firmware via Linux remoteproc. Default source: `/tmp/my_fw.out`.

```bash
# From laptop:
scp ./Debug/my_fw.out root@bramka:/tmp/my_fw.out

# On bramka:
m4f-reload         # uses /tmp/my_fw.out
```

**Requires M4F firmware to have graceful shutdown handler** (see
`docs/M4F_SHUTDOWN.md`). Without it, every deploy needs full reboot.

## PowerShell helpers for Windows laptop

Add to `$PROFILE` (run `notepad $PROFILE` in PowerShell):

```powershell
# === Bramka deploy helpers ===
$BRAMKA_HOST = "root@192.168.2.170"
$WORKSPACE = "C:\Users\<you>\workspace_ccstheia"
$PROJECT_NAME = "ipc_rpmsg_echo_am62x-sk_m4fss0-0_nortos_ti-arm-clang"

function Deploy-M4F {
    param([switch]$NoLogs, [int]$LogSeconds = 5)

    $fwPath = "$WORKSPACE\$PROJECT_NAME\Debug\$PROJECT_NAME.out"
    if (-not (Test-Path $fwPath)) {
        Write-Host "ERROR: firmware not found: $fwPath" -ForegroundColor Red
        return
    }

    Write-Host "[1/3] Uploading..." -ForegroundColor Cyan
    scp $fwPath "${BRAMKA_HOST}:/tmp/my_fw.out"
    if ($LASTEXITCODE -ne 0) { return }

    Write-Host "[2/3] Reloading..." -ForegroundColor Cyan
    ssh $BRAMKA_HOST "m4f-reload"
    if ($LASTEXITCODE -ne 0) { return }

    if (-not $NoLogs) {
        Write-Host "[3/3] Watching logs..." -ForegroundColor Cyan
        ssh $BRAMKA_HOST "timeout $LogSeconds m4f-watch"
    }
}

function Watch-M4F   { ssh $BRAMKA_HOST "m4f-watch" }
function Test-Rpmsg  { param([int]$N=3); ssh $BRAMKA_HOST "timeout 10 rpmsg_char_simple -r 9 -n $N" }
function Connect-Bramka { ssh $BRAMKA_HOST }
function Get-M4FState { ssh $BRAMKA_HOST "echo 'state:'; cat /sys/class/remoteproc/remoteproc0/state; ls /dev/rpmsg*" }
```

Reload: `. $PROFILE`

Then deploy with one command: `Deploy-M4F`

## Working firmware template

See `examples/m4f-baseline/ipc_rpmsg_echo.c` for a complete reference
implementation including:

- Async RPMsg callback pattern (no FreeRTOS dependency)
- Autonomous 1 Hz tick demonstrating M4F independence
- Command parsing (ping, status, generic echo)
- **Graceful shutdown handler** in correct order (this is the part most
  tutorials get wrong)

Copy this file into your CCS Theia project, configure SysConfig per
`examples/m4f-baseline/README.md`, build, deploy.

## Documentation

- **`docs/M4F_SHUTDOWN.md`** - Why graceful shutdown matters, how to
  implement correctly, common pitfalls. **Read this before writing any
  M4F code.**
- **`docs/TROUBLESHOOTING.md`** - Real issues encountered with diagnosis
  and fixes. Network, M4F, SD card, build issues.

## Configuration reference (`config.sh`)

| Variable | Default | Purpose |
|----------|---------|---------|
| `BRAMKA_MAC` | `22:F4:99:37:A5:12` | Persistent MAC. Use locally administered prefix. |
| `BRAMKA_HOSTNAME` | `bramka-01` | Linux hostname. |
| `BRAMKA_IP_STATIC` | `""` (DHCP) | Optional static IP (recommended: leave empty, use router DHCP reservation). |
| `TOOLS_DIR` | `/root` | Where to install m4f-watch and m4f-reload. Also symlinked to /usr/bin. |
| `ENABLE_M4F_AUTOSTOP` | `no` | If `yes`, M4F is stopped at boot (active dev mode). |

## Locally administered MAC prefixes

For `BRAMKA_MAC`, first byte must have bit 1 set:

- `02:xx:...`, `06:xx:...`, `0a:xx:...`, `0e:xx:...`
- `22:xx:...`, `26:xx:...`, `2a:xx:...`, `2e:xx:...`
- ... etc.

Don't use real OUI prefixes (e.g. `00:1b:21:...` is Intel) - those collide
with manufacturer addresses.

## Idempotency

All modules safe to re-run. They check state before applying changes.
You can re-run `./setup.sh` after editing `config.sh` to apply new settings.

## Architecture notes

### Why systemd-networkd `.link` units for MAC

AM62 SK has no EEPROM at I2C 0x50/0x51, so kernel generates random MAC
each boot (boot log: `EEPROM not available at 0x50`).

The `.link` unit in `/etc/systemd/network/00-eth1-mac.link` is processed
by udev/networkd very early - before any IP configuration. It pins MAC to
a value we control, matched by hardware path (stable) not interface name
(can be unstable).

### Why backup default M4F firmware

Linux remoteproc auto-loads `/lib/firmware/ti-ipc/am62xx/ipc_echo_test_mcu2_0_release_strip.xer5f`
at boot. When developing, we replace this with our firmware. The `.original`
backup lets us restore TI's demo when needed (e.g. for diagnostics).

### Why `m4f-watch` over `tail -f`

`trace0` is a debugfs file backed by circular shared memory buffer:
- No inotify support
- Buffer wraps (96 entries ~ 4 KB)
- `tail` optimizes for regular files (seek-from-end), returns garbage on debugfs

`m4f-watch` extracts microsecond timestamps as integers, sorts numerically,
shows only entries newer than last shown. Handles wrap correctly.

### Why M4F needs shutdown handler

Linux uses mailbox handshake to stop M4F. Without ACK from M4F, Linux times
out (-EBUSY) and refuses to stop. This means every firmware deploy without
shutdown handler requires reboot. See `docs/M4F_SHUTDOWN.md` for the fix.

### Key lesson learned: order of operations matters

In the M4F shutdown sequence:
- **Wrong**: `Drivers_close()` → `IpcNotify_sendMsg(ACK)` → `System_deinit()` → WFI
- **Right**: `Drivers_close()` → `System_deinit()` → `IpcNotify_sendMsg(ACK)` → WFI

`System_deinit()` must come BEFORE ACK send. Empirically verified - sending
ACK earlier causes Linux to time out. Not well documented but visible in
SDK example `ipc_rpmsg_echo_linux.c`.

## Future modules

Planned additions to setup workflow:

- `04-ssh-keys.sh` - SSH key authentication (no password)
- `05-go-runtime.sh` - Go installation for bramka services
- `06-bramka-service.sh` - Production Go service as systemd unit
- `07-docker-services.sh` - Mosquitto, InfluxDB via docker-compose
- `08-firewall.sh` - Lock down unused ports
- `09-watchdog.sh` - Auto-restart on failure

Each new module adds capability without changing existing ones.
