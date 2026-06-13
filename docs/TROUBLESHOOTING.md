# Troubleshooting

Real issues encountered during AM62 bramka development, with diagnosis and fixes.

## Network / SSH issues

### MAC address changes on every boot (DHCP reservation broken)

**Symptom**: Router gives different IP each boot despite DHCP reservation.

**Cause**: AM62 SK board has no EEPROM at I2C 0x50/0x51, so kernel generates
random MAC on each boot (see dmesg: `EEPROM not available at 0x50`).

**Fix**: Persistent MAC via systemd-networkd `.link` unit (done by `01-network.sh`).
Match by hardware path, not interface name (which can be unstable):

```ini
[Match]
Path=platform-8000000.ethernet
OriginalName=eth*

[Link]
MACAddressPolicy=none
MACAddress=22:F4:99:37:A5:12
NamePolicy=
Name=eth1
```

After applying, reboot and verify with `ip link show eth1`.

### SSH connection refused after fresh image

**Symptom**: `ssh root@<ip>` fails with "Connection refused".

**Cause**: SSH daemon not running yet, or empty root password disabled.

**Fix** via UART console:
```bash
systemctl status sshd
systemctl enable sshd
systemctl start sshd
# If still refused: passwd root, then set a password
```

## M4F development issues

### `echo stop > .../state` returns "Device or resource busy"

**Symptom**:
```
-sh: echo: write error: Device or resource busy
```

dmesg shows:
```
k3-m4-rproc 5000000.m4fss: notify_shutdown_rproc: timeout waiting for rproc completion event
remoteproc remoteproc0: can't stop rproc: -16
```

**Cause**: M4F firmware lacks graceful shutdown handler. Linux sends shutdown
notification, waits 10s for ACK, times out.

**Fix**: Add shutdown handler to firmware code. See `docs/M4F_SHUTDOWN.md`
for complete implementation.

**Temporary workaround** (forces reboot but doesn't break remoteproc numbering):
```bash
# Disable auto-load on next boot:
mv /lib/firmware/ti-ipc/am62xx/ipc_echo_test_mcu2_0_release_strip.xer5f /tmp/fw_bak.out
sync && reboot
# After reboot, M4F is 'offline' - then m4f-reload works for first deploy
```

### After unbind/bind, M4F is at remoteproc4 instead of remoteproc0

**Symptom**: All scripts hardcoding `remoteproc0` break. `ls /sys/class/remoteproc/`
shows `remoteproc1, 2, 3, 4` with M4F at `remoteproc4`.

**Cause**: Linux remoteproc never reuses numbers after unbind. Each bind
creates a new entry with the next available number.

**Fix**: Reboot. This restores deterministic device tree enumeration order
(M4F always at remoteproc0 after fresh boot).

**Prevention**: Don't use unbind/bind. Use `m4f-reload` which relies on
proper graceful shutdown.

### M4F shutdown handler sends ACK but Linux still times out

**Symptom**: trace0 shows shutdown was handled by M4F:
```
[M4F] Shutdown requested by core 0
[M4F] Shutdown ACK sent to Linux, going to WFI
```

But Linux dmesg shows:
```
notify_shutdown_rproc: timeout waiting for rproc completion event
```

**Cause**: Wrong order of operations in shutdown sequence. ACK was sent
BEFORE `System_deinit()`, but SDK requires it AFTER.

**Fix**: Correct order is `Drivers_close()` → `System_deinit()` → `IpcNotify_sendMsg(ACK)` → WFI.
See `docs/M4F_SHUTDOWN.md` for details.

## Linux watch / log issues

### `tail -f trace0` doesn't show new entries

**Symptom**: `tail -f /sys/kernel/debug/remoteproc/remoteproc0/trace0` displays
existing buffer but never shows new lines as M4F logs them.

**Cause**: `trace0` is a debugfs file, not a regular file:
1. No inotify support - `tail -f` watches for filesystem events that never fire
2. Circular buffer in shared memory - `tail` optimizes by seeking to end and
   reading back, which doesn't work on debugfs (returns garbage)

**Fix**: Use the `m4f-watch` script (installed by `02-tools.sh`) which uses
smart polling with deduplication based on microsecond timestamps extracted
from log lines.

### Trace buffer shows old entries mixed with new (circular wrap)

**Symptom**: After running for a long time, `cat trace0` shows entries with
timestamps that don't go in order - some old, some new, intermixed.

**Cause**: Trace buffer is a fixed 4KB circular buffer (~96 entries).
When full, new writes overwrite oldest entries. Physical position in file
doesn't match logical time order.

**Fix**: `m4f-watch` script extracts timestamps and sorts numerically,
showing only entries newer than last shown. Buffer wrap is handled correctly.

To get longer history, increase trace buffer in M4F SysConfig:
`TI Driver Porting Layer -> Debug Log -> Memory Log Size` (default 4096,
try 16384 for ~6 minutes history at 1 tick/sec).

## Build / firmware issues

### CCS Theia warns about SysConfig version mismatch

**Symptom**:
```
Product SysConfig v1.26.2 is not currently installed.
A compatible version 1.27.1 will be used.
```

**Cause**: SDK was tested with v1.26.2, but CCS Theia ships with v1.27.1.
The "compatible" path uses newer version which works in most cases.

**Fix** (optional): Install matching version via CCS Help -> Manage Packages -> SysConfig.

For now, this is a warning, not an error - builds should still work.

### Firmware loads but no logs appear in trace0

**Symptom**: `cat /sys/class/remoteproc/remoteproc0/state` shows `running`
but `cat /sys/kernel/debug/remoteproc/remoteproc0/trace0` is empty.

**Cause**: SysConfig log configuration missing "Info Zone" enable.

**Fix**: In SysConfig:
1. Open `example.syscfg`
2. Navigate to `TI Driver Porting Layer -> Debug Log`
3. Enable Sinks: Error Zone ✓, Warning Zone ✓, **Info Zone ✓** (required!),
   Remote Core Trace ✓, CCS Log ✓
4. Save, rebuild, redeploy

## SD card / image issues

### Etcher fails to flash image (corrupted)

**Symptom**: Etcher hangs at "Decompressing" or flashes but card doesn't boot.

**Cause**: Etcher has known bugs with streaming `.xz` decompression on
large images.

**Fix**: Manually decompress before flashing:
1. Extract `tisdk-default-image-am62xx-evm.wic.xz` with 7-Zip or WinRAR
2. Get `.wic` file
3. Flash that with Etcher

### Card boots but partitions are tiny

**Symptom**: After boot, `df -h /` shows rootfs ~150MB but card is 64GB.

**Cause**: First-boot resize hasn't run yet, or `growpart` is missing.

**Fix**: TI Arago Linux includes `cloud-init`-style first-boot resize.
After first reboot, partition should fill the card. If not, manually:
```bash
growpart /dev/mmcblk0 2
resize2fs /dev/mmcblk0p2
```
## RPMsg na AM62 - Niedeterministyczna numeracja virtio busów

### Problem

`/dev/rpmsg_ctrl0` i `/dev/rpmsg_ctrl1` mogą wskazywać na **różne**
remote core'y (M4F lub R5F) między bootami. Hardcoded ścieżki **nie działają**.

### Rozwiązanie

Autodetect przez sysfs:

```c
for (int i = 0; ; i++) {
    snprintf(path, "/sys/class/rpmsg/rpmsg_ctrl%d/device", i);
    if (readlink(path, real_path) < 0) break;
    if (strstr(real_path, "5000000.m4fss")) {
        // To jest M4F ctrl
    }
}
```

Po stronie Go: `findM4FChrdev()` w `protocol-test/main.go`.

### Identyfikatory hardware AM62

- M4F: `5000000.m4fss`
- R5F: `78000000.r5f`

## /dev/rpmsg0 - loopback gdy wskazuje na R5F

### Problem

Pisanie do `/dev/rpmsg0` powodowało **loopback** w kernelu (RX = TX bajtowo).

### Przyczyna

`/dev/rpmsg0` w niektórych boot'ach to **R5F chrdev** (default firmware z TI demo
robi local loopback). My chcemy M4F.

### Rozwiązanie

`findM4FChrdev()` szuka:
1. `/sys/class/rpmsg/rpmsg[0-9]*` symlink wskazuje na `5000000.m4fss` (M4F)
2. AND nazwa zawiera `rpmsg_chrdev` (nie `rpmsg-client-sample` ani inny custom endpoint)

## Go service zombie process po SSH disconnect

### Problem

`ssh root@bramka "./protocol-test"` w PowerShell. Po Ctrl+C w PowerShell,
proces wciąż żyje na bramce. Kolejny run → "resource busy" na `/dev/rpmsg0`.

### Przyczyna

Standardowe `ssh` nie alokuje pseudo-TTY → SIGINT nie propaguje do remote process.

### Rozwiązanie

`ssh -t root@bramka "./protocol-test"` — flag `-t` wymusza PTY allocation.
Plus w `Deploy-Go` dodajemy `pkill -f $ServiceName` przed startem (cleanup
po ewentualnych poprzednich zombie).

## Volatile vs const uint8_t* w protocol_decode

### Problem

`gRxBuffer.data` to `volatile char[]`. `protocol_decode` przyjmuje `const uint8_t*`.
Compiler warning: "discards qualifiers".

### Rozwiązanie tymczasowe

Explicit cast: `(const uint8_t *)gRxBuffer.data`.

### Rozwiązanie docelowe (later)

Lokalna kopia bufora w `handleLinuxMessage` - pozwala wcześniej zwolnić `pending`
flag i uniknąć volatile concerns.

## Binary protocol - lessons z implementacji

### CRC compatibility wymaga DOKŁADNIE tej samej implementacji

Init value 0xFFFF, poly 0x1021, no reflect, no xorout. Wszystkie cztery muszą się zgadzać.
Test vector `crc16("123456789") == 0x29B1` - standard XMODEM.

### Struct packing krytyczne

`__attribute__((packed))` na `msg_header_t`. Bez tego compiler doda padding
i wire format M4F ≠ Go.

### Big-endian wire format

Mimo że obie strony są little-endian (ARM Cortex-M4 i Cortex-A53),
wire format **BE** dla portability. Helper functions `protocol_get_u16_be`
i `protocol_put_u16_be` w header. **Nigdy nie polegaj na memcpy bezpośrednio
ze struct'a do wire.**

### Single source of truth z cgo

Jeden `protocol.h` w `shared/`. Kopiowany do:
- M4F project folder (CCS workspace)
- Go service folder (przez Deploy-Go)

Go używa cgo: `#include "protocol.h"` + `import "C"`.
Stałe dostępne jako `C.MSG_HELLO`, etc.
Inline functions z header **automatycznie** dostępne w Go bez extra linkowania.