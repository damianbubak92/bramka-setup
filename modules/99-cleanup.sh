#!/bin/bash
# 99-cleanup.sh - final verification and next-steps info

set -e

echo ""
echo "=== Verification ==="

# Check network link file:
LINK_FILE="/etc/systemd/network/00-eth1-mac.link"
if [ -f "$LINK_FILE" ]; then
    echo "  [OK] MAC link file: $LINK_FILE"
    grep "MACAddress=" "$LINK_FILE" | sed 's/^/       /'
else
    echo "  [FAIL] Missing: $LINK_FILE"
fi

# Check hostname:
CURRENT_HOSTNAME=$(hostname)
if [ "$CURRENT_HOSTNAME" = "$BRAMKA_HOSTNAME" ]; then
    echo "  [OK] Hostname: $CURRENT_HOSTNAME"
else
    echo "  [WARN] Hostname is '$CURRENT_HOSTNAME', expected '$BRAMKA_HOSTNAME'"
    echo "         Will take full effect after reboot"
fi

# Check tools:
for tool in m4f-watch m4f-reload; do
    if command -v "$tool" &> /dev/null; then
        echo "  [OK] Tool: $tool ($(command -v $tool))"
    else
        echo "  [FAIL] Tool missing: $tool"
    fi
done

# Check firmware backup:
BACKUP_FILE="$FW_DIR/$DEFAULT_FW_NAME.original"
if [ -f "$BACKUP_FILE" ]; then
    echo "  [OK] Default M4F firmware backup: $BACKUP_FILE"
fi

# Check M4F autostop service:
if [ "$ENABLE_M4F_AUTOSTOP" = "yes" ]; then
    if systemctl is-enabled m4f-autostop.service &>/dev/null; then
        echo "  [OK] m4f-autostop.service enabled (dev mode)"
    else
        echo "  [FAIL] m4f-autostop.service should be enabled but isn't"
    fi
fi

# Current M4F state:
if [ -r /sys/class/remoteproc/remoteproc0/state ]; then
    M4F_STATE=$(cat /sys/class/remoteproc/remoteproc0/state)
    echo "  [INFO] M4F current state: $M4F_STATE"
fi

# Current MAC (will change after reboot):
CURRENT_MAC=$(cat /sys/class/net/$ETH_INTERFACE_NAME/address 2>/dev/null || echo "unknown")
if [ "$CURRENT_MAC" = "$BRAMKA_MAC" ] || [ "$CURRENT_MAC" = "${BRAMKA_MAC,,}" ]; then
    echo "  [OK] Current MAC: $CURRENT_MAC (matches config)"
else
    echo "  [INFO] Current MAC: $CURRENT_MAC"
    echo "         Will change to $BRAMKA_MAC after reboot"
fi

# Show next steps:
echo ""
echo "=== Next steps ==="
echo "  1. Reboot to apply MAC change:    reboot"
echo "  2. Configure DHCP reservation in router:"
echo "       MAC: $BRAMKA_MAC"
echo "       IP:  (your choice, e.g. 192.168.2.170)"
echo "  3. After reboot SSH back and verify:"
echo "       ip link show $ETH_INTERFACE_NAME"
echo "       m4f-watch"
echo ""
