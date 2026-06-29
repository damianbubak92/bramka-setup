#!/bin/bash
# 02-tools.sh - install M4F debug/dev utility scripts
#
# Installs:
#  - m4f-watch  : live tail of M4F trace0 (smart polling, deduplication)
#  - m4f-reload : hot-swap M4F firmware via remoteproc (stop/copy/start)
#
# NOTE: m4f-reload assumes M4F firmware implements graceful shutdown handler.
# See docs/M4F_SHUTDOWN.md for required implementation in firmware code.

set -e

mkdir -p "$TOOLS_DIR"

# ============================================================================
# m4f-watch - live M4F trace0 monitor
# ============================================================================

cat > "$TOOLS_DIR/m4f-watch" << 'WATCH_EOF'
#!/bin/bash
# Live tail M4F trace buffer with deduplication.
# 
# Why not 'tail -f'? trace0 is a sysfs/debugfs file - no inotify support,
# circular buffer that wraps. Plain tail/follow doesn't work correctly.
#
# Algorithm:
#  - On start, find newest log entry by timestamp, show only that one
#  - Then poll: read full buffer, filter entries newer than last shown
#  - Use integer microsecond timestamps to avoid float precision issues
#
# Usage: m4f-watch [polling_interval_seconds]   (default 0.5)

TRACE_FILE="/sys/kernel/debug/remoteproc/remoteproc0/trace0"
INTERVAL="${1:-0.5}"

if [ ! -r "$TRACE_FILE" ]; then
    echo "Error: cannot read $TRACE_FILE"
    echo "Is M4F remoteproc running?"
    echo "  cat /sys/class/remoteproc/remoteproc0/state"
    exit 1
fi

echo "=== M4F live trace (Ctrl+C to stop, polling every ${INTERVAL}s) ==="

extract_sortable() {
    awk '
        /^\[m4f/ {
            match($0, /[0-9]+\.[0-9]+/)
            ts_str = substr($0, RSTART, RLENGTH)
            ts_us = int(ts_str * 1000000)
            printf "%d %s\n", ts_us, $0
        }'
}

INITIAL=$(cat "$TRACE_FILE" | extract_sortable | sort -n | tail -1)
if [ -z "$INITIAL" ]; then
    echo "(trace buffer is empty - waiting for first M4F log...)"
    LAST_KEY="0"
else
    LAST_KEY=$(echo "$INITIAL" | cut -d' ' -f1)
    echo "$INITIAL" | cut -d' ' -f2-
fi

while true; do
    OUTPUT=$(cat "$TRACE_FILE" | extract_sortable | awk -v last="$LAST_KEY" '$1 > last' | sort -n)
    if [ -n "$OUTPUT" ]; then
        echo "$OUTPUT" | cut -d' ' -f2-
        LAST_KEY=$(echo "$OUTPUT" | tail -1 | cut -d' ' -f1)
    fi
    sleep "$INTERVAL"
done
WATCH_EOF

chmod +x "$TOOLS_DIR/m4f-watch"
echo "[*] Installed $TOOLS_DIR/m4f-watch"

# ============================================================================
# m4f-reload - hot-swap M4F firmware via remoteproc stop/start
# ============================================================================
#
# This assumes M4F firmware implements graceful shutdown handler.
# Without it, 'echo stop' will timeout and this script will fail.
# See docs/M4F_SHUTDOWN.md for shutdown handler implementation.
#
# NEVER use unbind/bind workaround - it permanently renumbers /sys/class/remoteproc/
# (M4F moves from remoteproc0 to remoteproc4 etc), breaking all hardcoded paths.

cat > "$TOOLS_DIR/m4f-reload" << RELOAD_EOF
#!/bin/bash
# Reload M4F firmware via Linux remoteproc framework.
#
# Workflow: stop M4F -> copy new .out to /lib/firmware/ -> start M4F
# Requires M4F firmware to have graceful shutdown handler.
#
# Usage: m4f-reload [path_to_firmware.out]   (default: /tmp/my_fw.out)

FW_SRC="\${1:-/tmp/my_fw.out}"
FW_DST="$FW_DIR/$DEFAULT_FW_NAME"
STATE_FILE="/sys/class/remoteproc/remoteproc0/state"

if [ ! -f "\$FW_SRC" ]; then
    echo "Error: \$FW_SRC not found"
    exit 1
fi

if [ ! -e "\$STATE_FILE" ]; then
    echo "Error: \$STATE_FILE not found"
    echo "Is M4F at remoteproc0? Check: ls /sys/class/remoteproc/"
    echo ""
    echo "If M4F moved elsewhere (e.g. after unbind/bind), REBOOT to restore."
    echo "Don't use unbind/bind - it permanently renumbers remoteproc devices."
    exit 1
fi

# Verify remoteproc0 actually is M4F:
NAME=\$(cat /sys/class/remoteproc/remoteproc0/name 2>/dev/null)
if [ "\$NAME" != "5000000.m4fss" ]; then
    echo "Error: remoteproc0 is '\$NAME', expected '5000000.m4fss'"
    echo "Reboot to restore deterministic numbering"
    exit 1
fi

# Stop rpmsg-service before touching M4F. The service watches /dev/rpmsg and
# treats a vanished device (the 'echo stop' below) as PEER DEAD -> immediate
# clean reboot of the whole gateway (P2 fast-fail). With the service stopped the
# device is also released cleanly, so the M4F stop doesn't need the kill fallback.
# Pre-flight checks above run first, so a trivial error never bounces the service.
SERVICE="rpmsg-service"
SVC_WAS_ACTIVE=0
if systemctl is-active --quiet "\$SERVICE"; then
    SVC_WAS_ACTIVE=1
    echo "[*] Stopping \$SERVICE (would reboot the gateway on device-gone)"
    systemctl stop "\$SERVICE"
fi

# Always bring the service back if it was running, even on early error/exit.
restore_service() {
    if [ "\$SVC_WAS_ACTIVE" = "1" ] && ! systemctl is-active --quiet "\$SERVICE"; then
        echo "[*] Restarting \$SERVICE"
        systemctl start "\$SERVICE"
    fi
}
trap restore_service EXIT

echo "[1/4] Stopping M4F..."
echo stop > "\$STATE_FILE" 2>/dev/null || true
sleep 0.5

# If still not offline, try cleanup of holding processes:
if [ "\$(cat \$STATE_FILE)" != "offline" ]; then
    pkill -9 -f rpmsg_char_simple 2>/dev/null || true
    pkill -9 -f '/dev/rpmsg' 2>/dev/null || true
    fuser -k /dev/rpmsg0 /dev/rpmsg1 2>/dev/null || true
    sleep 0.5
    echo stop > "\$STATE_FILE" 2>/dev/null || true
    sleep 0.5
fi

CURRENT_STATE=\$(cat "\$STATE_FILE")
if [ "\$CURRENT_STATE" != "offline" ]; then
    echo ""
    echo "ERROR: M4F won't stop (state: \$CURRENT_STATE)"
    echo ""
    echo "This usually means the current M4F firmware lacks graceful shutdown handler."
    echo "See /root/bramka-setup/docs/M4F_SHUTDOWN.md for required implementation."
    echo ""
    echo "WORKAROUND for current session:"
    echo "  1. Move firmware to disable auto-load on next boot:"
    echo "     mv \$FW_DST /tmp/old_fw_\$(date +%s).out"
    echo "  2. Reboot: sync && reboot"
    echo "  3. After reboot, M4F will be 'offline' - then m4f-reload works"
    echo ""
    echo "PROPER FIX: add shutdown handler to firmware code (one-time)"
    exit 2
fi

echo "[2/4] Copying \$FW_SRC -> \$FW_DST"
cp "\$FW_SRC" "\$FW_DST"

echo "[3/4] Starting M4F..."
echo start > "\$STATE_FILE"
sleep 1

NEW_STATE=\$(cat "\$STATE_FILE")
echo "[4/4] M4F state: \$NEW_STATE"

if [ "\$NEW_STATE" = "running" ]; then
    echo ""
    echo "=== Last 10 logs from new firmware ==="
    cat /sys/kernel/debug/remoteproc/remoteproc0/trace0 | tail -10
    echo ""
    echo "Tip: 'm4f-watch' in another terminal for live trace"
else
    echo ""
    echo "=== ERROR: M4F not running ==="
    dmesg | tail -15
    exit 3
fi
RELOAD_EOF

chmod +x "$TOOLS_DIR/m4f-reload"
echo "[*] Installed $TOOLS_DIR/m4f-reload"

# ============================================================================
# Symlinks to /usr/bin so tools are in PATH from anywhere
# ============================================================================

if [ "$TOOLS_DIR" != "/usr/local/bin" ] && [ "$TOOLS_DIR" != "/usr/bin" ]; then
    for tool in m4f-watch m4f-reload; do
        TARGET="/usr/bin/$tool"
        if [ ! -L "$TARGET" ] || [ "$(readlink "$TARGET")" != "$TOOLS_DIR/$tool" ]; then
            ln -sf "$TOOLS_DIR/$tool" "$TARGET"
            echo "[*] Symlinked $TARGET -> $TOOLS_DIR/$tool"
        fi
    done
fi

echo "[*] Tools module complete"
