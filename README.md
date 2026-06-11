# Bramka Setup Scripts

Idempotent setup automation for AM62 bramka deployments.
Configures fresh Arago/TI Linux image with persistent network identity,
M4F development tools, and firmware management infrastructure.

## Quick start

### On a new bramka

1. Flash fresh TI SDK Linux image to SD card (Etcher + .wic extracted manually).
2. Boot bramka, find its IP (router admin panel or `ssh root@am62xx-evm.local`).
3. Connect via SSH from laptop and verify access:
   ```bash
   ssh root@<bramka-ip>
   ```
   (default password is empty - press Enter)

### On a laptop (this repo)

4. Edit `config.sh` for this specific bramka:
   ```bash
   BRAMKA_MAC="22:F4:99:37:A5:12"    # unique per bramka!
   BRAMKA_HOSTNAME="bramka-01"
   ```

5. SCP the whole folder to bramka:
   ```bash
   scp -r bramka-setup root@<bramka-ip>:/root/
   ```

6. SSH and run:
   ```bash
   ssh root@<bramka-ip>
   cd /root/bramka-setup
   chmod +x setup.sh modules/*.sh
   ./setup.sh
   ```

7. Reboot bramka:
   ```bash
   reboot
   ```

8. Configure DHCP reservation in router for the new MAC.

9. Done - SSH back to verify:
   ```bash
   ssh root@<reserved-ip>
   m4f-watch
   ```

## What it does

| Module | Purpose |
|--------|---------|
| `01-network.sh` | Persistent MAC via systemd-networkd link unit. Solves AM62's no-EEPROM random-MAC-each-boot problem. |
| `02-tools.sh` | Installs `m4f-watch` (live trace0 monitor) and `m4f-reload` (firmware hot-swap). |
| `03-m4f-firmware.sh` | Backs up default TI M4F firmware. Optional dev-mode auto-stop. |
| `99-cleanup.sh` | Verification and next-steps info. |

## Installed tools

### `m4f-watch [polling_seconds]`

Live tail of M4F trace buffer. Default polling 0.5s.

Why not `tail -f`? trace0 is a sysfs/debugfs file - no inotify support, plus
it's a circular buffer. This script uses smart polling with deduplication.

```bash
m4f-watch          # default 500ms polling
m4f-watch 1        # 1s polling (less CPU)
m4f-watch 0.1      # 100ms polling (snappier)
```

### `m4f-reload [path_to_firmware.out]`

Hot-swap M4F firmware via Linux remoteproc. Default source: `/tmp/my_fw.out`.

```bash
# From laptop:
scp ./Debug/my_fw.out root@bramka:/tmp/my_fw.out

# On bramka:
m4f-reload         # uses /tmp/my_fw.out
m4f-reload /path/to/other.out
```

## Configuration reference

Edit `config.sh` per bramka.

| Variable | Default | Purpose |
|----------|---------|---------|
| `BRAMKA_MAC` | `22:F4:99:37:A5:12` | Persistent MAC. Use locally administered prefix. |
| `BRAMKA_HOSTNAME` | `bramka-01` | Linux hostname. |
| `BRAMKA_IP_STATIC` | `""` (DHCP) | Optional static IP (recommended: leave empty, use router DHCP reservation). |
| `TOOLS_DIR` | `/root` | Where to install m4f-watch and m4f-reload. Also symlinked to /usr/bin. |
| `ENABLE_M4F_AUTOSTOP` | `no` | If `yes`, M4F is stopped at boot (active dev mode). |

## Locally administered MAC prefixes

For BRAMKA_MAC, first byte must have bit 1 set:

- `02:xx:xx:xx:xx:xx`
- `06:xx:xx:xx:xx:xx`
- `0a:xx:xx:xx:xx:xx`
- `0e:xx:xx:xx:xx:xx`
- `22:xx:xx:xx:xx:xx`
- `26:xx:xx:xx:xx:xx`
- `2a:xx:xx:xx:xx:xx`
- ... etc.

Don't use real OUI prefixes (e.g. starting with `00:`, `04:`, etc.) - those
collide with real manufacturer addresses.

## Idempotency

All modules are idempotent - safe to run multiple times. They:
- Check if changes already applied before applying
- Use `cp -f` semantics (overwrite if exists)
- Don't duplicate entries in `/etc/hosts` etc.

You can re-run `./setup.sh` after editing config (e.g. to change hostname).

## Architecture notes

### Why systemd-networkd `.link` units for MAC

AM62 SK board has no EEPROM at I2C address 0x50/0x51, so kernel can't read
the "factory" MAC. Without EEPROM, the kernel generates a random MAC on each
boot (see boot log: `EEPROM not available at 0x50`).

The `.link` unit in `/etc/systemd/network/00-eth1-mac.link` is processed by
udev/networkd very early in boot, before any IP configuration. It pins the
MAC to a value we control, based on hardware path match (not interface name,
which can be unstable).

### Why backup default M4F firmware

The Linux remoteproc loads `/lib/firmware/ti-ipc/am62xx/ipc_echo_test_mcu2_0_release_strip.xer5f`
at boot. When we develop our own M4F firmware, we replace this file. The
backup `.original` lets us always restore the default TI demo firmware.

### Why m4f-watch over `tail -f`

The kernel exposes M4F's debug log at `/sys/kernel/debug/remoteproc/remoteproc0/trace0`.
This is a debugfs entry backed by a circular buffer in shared memory.

- No inotify support (debugfs limitation)
- Circular buffer wraps - file content can shrink or shift
- `tail` optimizes for regular files (seek to end, read back) - returns garbage on debugfs

m4f-watch uses smart polling: reads full buffer each cycle, sorts entries by
their embedded microsecond timestamps (as integers, not floats - avoids
precision issues), and outputs only entries newer than last shown.
