#!/bin/bash
# configure-watchdog.sh
# Włącza systemd runtime watchdog dla AM62 (HW watchdog layer D).
#
# Bez tego skryptu /dev/watchdog jest aktywny ale BEZ kicker'a -
# bramka resetuje się co 60 sekund.

set -e

echo "=== Configuring Linux watchdog (Layer D) ==="

SYSTEM_CONF="/etc/systemd/system.conf"

# Check current state
if grep -qE '^RuntimeWatchdogSec=' "$SYSTEM_CONF"; then
    CURRENT=$(grep -E '^RuntimeWatchdogSec=' "$SYSTEM_CONF" | head -1)
    echo "Already configured: $CURRENT"
else
    echo "Setting RuntimeWatchdogSec=30 in $SYSTEM_CONF"
    sed -i 's/#RuntimeWatchdogSec=off/RuntimeWatchdogSec=30/' "$SYSTEM_CONF"
fi

# Verify
echo ""
echo "Current setting:"
grep -E '^(#?)RuntimeWatchdogSec' "$SYSTEM_CONF"

# Check if active (requires reboot to take effect)
echo ""
echo "Note: systemd watchdog activation requires REBOOT"
echo "After reboot, verify with:"
echo "  lsof /dev/watchdog0    # should show systemd"
echo "  wdctl /dev/watchdog    # device busy = systemd has it"