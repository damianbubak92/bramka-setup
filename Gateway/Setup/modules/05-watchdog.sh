#!/bin/bash
# 05-watchdog.sh - enable systemd runtime HW watchdog (Warstwa D)
#
# Problem solved: AM62 ma sprzętowy K3 RTI watchdog (/dev/watchdog0), ale nikt
# go nie klepie dopóki systemd nie dostanie RuntimeWatchdogSec. Świeży obraz
# Arago + setup zostawiał Warstwę D WYŁĄCZONĄ, więc total freeze systemu (np.
# M4F silent-hang wieszający kernel) NIE miał ostatniej linii obrony i wymagał
# ręcznego power cycle.
#
# Solution: ustaw RuntimeWatchdogSec=30 w /etc/systemd/system.conf, żeby PID 1
# otworzył i klepał /dev/watchdog0. Przy zawiśnięciu HW resetuje cały SoC
# (~60s HW timeout). To warstwa BACKUP - primary recovery dla śmierci M4F to
# clean reboot z serwisu Go.
#
# Idempotent: re-run safe. Aktywacja wymaga reboota.
set -e

SYSTEM_CONF="/etc/systemd/system.conf"
WATCHDOG_SEC=30

echo "[*] Configuring systemd runtime watchdog (Warstwa D)"

if [ ! -f "$SYSTEM_CONF" ]; then
    echo "ERROR: $SYSTEM_CONF not found - is this a systemd system?"
    exit 1
fi

if grep -qE '^RuntimeWatchdogSec=' "$SYSTEM_CONF"; then
    CURRENT=$(grep -E '^RuntimeWatchdogSec=' "$SYSTEM_CONF" | head -1 | cut -d= -f2)
    if [ "$CURRENT" = "$WATCHDOG_SEC" ]; then
        echo "[*] Already set: RuntimeWatchdogSec=$CURRENT, skipping"
    else
        echo "[*] Updating RuntimeWatchdogSec=$CURRENT -> $WATCHDOG_SEC"
        sed -i "s/^RuntimeWatchdogSec=.*/RuntimeWatchdogSec=${WATCHDOG_SEC}/" "$SYSTEM_CONF"
    fi
elif grep -qE '^#RuntimeWatchdogSec=' "$SYSTEM_CONF"; then
    echo "[*] Enabling RuntimeWatchdogSec=${WATCHDOG_SEC} (było zakomentowane)"
    sed -i "s/^#RuntimeWatchdogSec=.*/RuntimeWatchdogSec=${WATCHDOG_SEC}/" "$SYSTEM_CONF"
else
    echo "[*] Appending RuntimeWatchdogSec=${WATCHDOG_SEC} pod [Manager]"
    if grep -qE '^\[Manager\]' "$SYSTEM_CONF"; then
        sed -i "/^\[Manager\]/a RuntimeWatchdogSec=${WATCHDOG_SEC}" "$SYSTEM_CONF"
    else
        printf '\n[Manager]\nRuntimeWatchdogSec=%s\n' "$WATCHDOG_SEC" >> "$SYSTEM_CONF"
    fi
fi

# Verify
echo "[*] Current setting:"
grep -E '^RuntimeWatchdogSec=' "$SYSTEM_CONF" || echo "    (none - ERROR)"

echo "[*] Watchdog module complete"
echo "    NOTE: aktywacja wymaga REBOOTA (PID 1 otwiera /dev/watchdog0 przy boot)"
echo "    Po reboocie weryfikuj: lsof /dev/watchdog0    # powinno pokazać systemd"
echo "                           wdctl /dev/watchdog0   # timeout/timeleft"
