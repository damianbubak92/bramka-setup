#!/bin/bash
# 06-kernel-panic.sh - promote kernel oops to full panic (fail-fast)
#
# Problem solved: domyślnie kernel.panic_on_oops=0 - po oopsie (NULL deref,
# zły wskaźnik, BUG()) kernel ubija winny wątek i jedzie dalej, ale często w
# niespójnym stanie (zdechły wątek z lockiem, na wpół martwy podsystem). System
# "kuleje", ale formalnie NIE panikuje, więc:
#   - systemd żyje -> dalej klepie /dev/watchdog0 -> Warstwa D NIE zadziała
#   - Warstwa A łapie tylko Go service, nie kernel
# To luka: oops kaleczący system, ale bez pełnego panic, nie jest łapany przez
# żadną warstwę recovery. Bramka wygląda na żywą, a nie działa - najgorsze dla
# 24/7 bez obsługi.
#
# Solution: kernel.panic_on_oops=1 -> każdy oops staje się pełnym kernel panic,
# a panic JUŻ łapiemy (Warstwa D HW watchdog, zweryfikowane testem `echo c`).
# Czyli oops -> panic -> reset SoC -> clean recovery. Fail-fast spójny z resztą
# designu: lepiej szybko paść i wstać niż cicho gnić.
#
# NIE ruszamy kernel.panic (delay auto-reboota po panic) - recovery jest
# zweryfikowane na ścieżce panic->HW watchdog (panic=0). Gdyby chcieć szybszy
# reboot po panic: dopisać `kernel.panic = 10` do tego samego drop-ina (HW
# watchdog zostaje backupem gdyby reboot się zawiesił).
#
# Idempotent: re-run safe (drop-in nadpisywany, apply przez sysctl). Apply od
# razu - nie wymaga reboota (ale i tak persistent na przyszłe boot-y).
set -e

DROPIN="/etc/sysctl.d/60-bramka-panic.conf"

echo "[*] Configuring kernel panic_on_oops (fail-fast)"

# Write managed drop-in (overwrite OK - this file is ours).
cat > "$DROPIN" << 'CONF'
# Managed by bramka-setup modules/06-kernel-panic.sh
# Promote kernel oops to full panic so the HW watchdog (Warstwa D) can recover.
kernel.panic_on_oops = 1
CONF
echo "[*] Wrote $DROPIN"

# Apply immediately (so it's live this boot too, not only after reboot).
if sysctl -w kernel.panic_on_oops=1 >/dev/null; then
    echo "[*] Applied at runtime: kernel.panic_on_oops=1"
else
    echo "[*] WARN: runtime apply failed (will take effect on next boot)"
fi

# Verify
echo "[*] Current setting:"
sysctl kernel.panic_on_oops 2>/dev/null || echo "    (cannot read - ERROR)"

echo "[*] Kernel-panic module complete"
echo "    Test (UWAGA: panikuje kernel, bramka się zresetuje):"
echo "      echo 1 > /proc/sys/kernel/panic_on_oops   # już ustawione"
echo "      # prawdziwy oops trudno wywołać na żądanie; pełny panic test: echo c > /proc/sysrq-trigger"
