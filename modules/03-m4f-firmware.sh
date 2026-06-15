#!/bin/bash
# 03-m4f-firmware.sh - prepare M4F firmware directory state
#
# - Backs up default TI firmware (so user can always restore it)
# - Optionally installs systemd service to auto-stop M4F at boot (dev mode)

set -e

FW_FILE="$FW_DIR/$DEFAULT_FW_NAME"
BACKUP_FILE="$FW_FILE.original"

# Check if default firmware exists (it should on Arago/TI Linux):
if [ ! -f "$FW_FILE" ]; then
    echo "[!] WARN: $FW_FILE not found"
    echo "    Is this an Arago/TI Linux image? Skipping firmware backup."
    exit 0
fi

# Backup default firmware (only once):
if [ ! -f "$BACKUP_FILE" ]; then
    cp "$FW_FILE" "$BACKUP_FILE"
    echo "[*] Backed up default firmware: $BACKUP_FILE"
    echo "    To restore default later: cp $BACKUP_FILE $FW_FILE"
else
    echo "[*] Default firmware backup already exists: $BACKUP_FILE"
fi

# ============================================================================
# Optional: M4F auto-stop service for dev mode
# ============================================================================

SERVICE_FILE="/etc/systemd/system/m4f-autostop.service"

if [ "$ENABLE_M4F_AUTOSTOP" = "yes" ]; then
    cat > "$SERVICE_FILE" << 'EOF'
[Unit]
Description=Stop M4F remoteproc on boot (for active dev workflow)
After=multi-user.target
ConditionPathExists=/sys/class/remoteproc/remoteproc0/state

[Service]
Type=oneshot
ExecStart=/bin/sh -c "echo stop > /sys/class/remoteproc/remoteproc0/state"
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
    systemctl enable m4f-autostop.service
    echo "[*] Enabled m4f-autostop.service (dev mode - M4F stopped at boot)"
else
    # Remove service if it was enabled in a previous run:
    if [ -f "$SERVICE_FILE" ]; then
        systemctl disable m4f-autostop.service 2>/dev/null || true
        rm -f "$SERVICE_FILE"
        systemctl daemon-reload
        echo "[*] Disabled m4f-autostop.service (default firmware will load at boot)"
    fi
fi

echo "[*] M4F firmware module complete"
