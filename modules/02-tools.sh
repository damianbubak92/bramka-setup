#!/bin/bash
# 02-tools.sh - install M4F debug/dev utility scripts
#
# Installs:
#  - m4f-watch  : live tail of M4F trace0 (smart polling, deduplication)
#  - m4f-reload : hot-swap M4F firmware via remoteproc (stop/copy/start)

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

# Awk extracts (timestamp_microseconds, line) tuples for sortable comparison.
# Integer microseconds avoid floating point precision issues in shell comparisons.
extract_sortable() {
    awk '
        /^\[m4f/ {
            match($0, /[0-9]+\.[0-9]+/)
            ts_str = substr($0, RSTART, RLENGTH)
            ts_us = int(ts_str * 1000000)
            printf "%d %s\n", ts_us, $0
        }'
}

# On start: show only the single newest entry as anchor.
INITIAL=$(cat "$TRACE_FILE" | extract_sortable | sort -n | tail -1)
if [ -z "$INITIAL" ]; then
    echo "(trace buffer is empty - waiting for first M4F log...)"
    LAST_KEY="0"
else
    LAST_KEY=$(echo "$INITIAL" | cut -d' ' -f1)
    echo "$INITIAL" | cut -d' ' -f2-
fi

# Polling loop: show only entries with timestamp > LAST_KEY.
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
# m4f-reload - hot-swap M4F firmware
# ============================================================================

# Use printf to inject config variables, then heredoc for the rest:
cat > "$TOOLS_DIR/m4f-reload" << RELOAD_EOF
#!/bin/bash
# Reload M4F firmware via Linux remoteproc framework.
#
# Workflow: stop M4F -> copy new .out to /lib/firmware/ -> start M4F
# Linux remoteproc reads the new ELF, parses resource table, sets up VRINGs,
# starts M4F with the new code.
#
# Usage: m4f-reload [path_to_firmware.out]   (default: /tmp/my_fw.out)

FW_SRC="\${1:-/tmp/my_fw.out}"
FW_DST="$FW_DIR/$DEFAULT_FW_NAME"
STATE_FILE="/sys/class/remoteproc/remoteproc0/state"

if [ ! -f "\$FW_SRC" ]; then
    echo "Error: \$FW_SRC not found"
    echo "Usage: m4f-reload [path_to_firmware.out]"
    exit 1
fi

echo "[1/4] Stopping M4F..."
echo stop > "\$STATE_FILE" 2>/dev/null || true
sleep 0.5

CURRENT_STATE=\$(cat "\$STATE_FILE")
if [ "\$CURRENT_STATE" != "offline" ]; then
    echo "  WARN: M4F state is '\$CURRENT_STATE', expected 'offline'"
    echo "  Something is holding M4F. Try:"
    echo "    pkill -9 rpmsg_char_simple"
    echo "    fuser -k /dev/rpmsg0"
    echo "  Then re-run m4f-reload"
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
    echo "Tip: run 'm4f-watch' in another terminal for live trace"
else
    echo ""
    echo "=== ERROR: M4F not running ==="
    echo "Check kernel messages:"
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
