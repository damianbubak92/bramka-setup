#!/bin/bash
# 07-boot-accounting.sh - reboot counter, cause attribution, storm alarm
#
# Cel: wiedzieć ILE razy bramka się zrebootowała, KIEDY i DLACZEGO (kto wywołał),
# oraz dostać alarm gdy reboot-ów jest nienaturalnie dużo (reboot-storm). To
# domyka diagnostykę: przy spiętrzeniu reboot-ów chcemy od razu widzieć przyczynę.
#
# Mechanizm atrybucji:
#   - Inicjowane przez nas reboot-y (Go recoverByReboot) zostawiają "breadcrumb"
#     /var/lib/bramka/reboot_reason PRZED rebootem. Oneshot service przy boocie
#     czyta go, atrybuuje i kasuje.
#   - Brak breadcrumb = reset NIE zainicjowany przez nasz software. Doklasyfikowanie
#     z logu poprzedniego bootu (wymaga persistent journald, włączany niżej):
#       * "Kernel panic" w logu kernela -> kernel panic -> HW watchdog (Warstwa D)
#       * sygnatura clean shutdown      -> ręczny/orkiestrowany reboot bez breadcrumb
#       * nic z powyższych              -> hard reset (M4F SOC reset / HW wdt / power loss)
#
# Parametryzacja (runtime, /etc/bramka/boot-accounting.conf):
#   ALARM_ENABLED / ALARM_THRESHOLD / ALARM_WINDOW_HOURS
#   Plik tworzony z defaultami TYLKO gdy nie istnieje - re-run setup nie nadpisuje
#   ustawień zmienionych przez admina / health-monitor. ALARM_ENABLED=0 wyłącza alarm.
#
# Idempotent: re-run safe.
set -e

STATE_DIR="/var/lib/bramka"
CONF_DIR="/etc/bramka"
CONF="$CONF_DIR/boot-accounting.conf"
BIN_DIR="/usr/local/bin"
ACCT_BIN="$BIN_DIR/bramka-boot-accounting"
STATUS_BIN="$BIN_DIR/bramka-reboots"
UNIT="/etc/systemd/system/bramka-boot-accounting.service"
JOURNALD_DROPIN="/etc/systemd/journald.conf.d/bramka.conf"

echo "[*] Configuring boot accounting (reboot counter + cause + alarm)"

mkdir -p "$STATE_DIR" "$CONF_DIR" "$BIN_DIR"

# ---------------------------------------------------------------------------
# Persistent journald (BEST-EFFORT, for diagnostics only - classification does
# NOT depend on it; that's done via the clean_shutdown marker, see script below).
#
# GOTCHA (Arago/Yocto): /var/log to symlink do /var/volatile/log (tmpfs), więc
# /var/log/journal NIE przetrwa reboota tam. Backing trzymamy na trwałym /var/lib
# i bind-mountujemy na /var/volatile/log/journal. Robimy to OneShot serwisem
# (nie .mount - nazwa .mount musi pasować do skanonikalizowanej ścieżki, a przez
# symlink /var/log to ordering/naming hell). Ordered Before=systemd-journal-flush
# żeby journald przełączył się na persistent dopiero gdy bind już jest.
# ---------------------------------------------------------------------------
PERSIST_JOURNAL_DIR="/var/lib/journal"
JOURNAL_WHERE="/var/volatile/log/journal"
JOURNAL_BIND_UNIT="/etc/systemd/system/bramka-journal-bind.service"

# Cleanup wrong-named .mount units z wcześniejszych prób.
for old in var-log-journal.mount var-volatile-log-journal.mount; do
    systemctl disable "$old" >/dev/null 2>&1 || true
    rm -f "/etc/systemd/system/$old"
done

mkdir -p /etc/systemd/journald.conf.d "$PERSIST_JOURNAL_DIR"
cat > "$JOURNALD_DROPIN" << 'JEOF'
# Managed by bramka-setup modules/07-boot-accounting.sh
# Best-effort persistent journal for post-mortem diagnostics across reboots.
[Journal]
Storage=persistent
SystemMaxUse=50M
JEOF
echo "[*] Wrote $JOURNALD_DROPIN (persistent journal, cap 50M)"

cat > "$JOURNAL_BIND_UNIT" << 'BEOF'
[Unit]
Description=Bind persistent journal over volatile /var/log (best-effort)
DefaultDependencies=no
After=systemd-tmpfiles-setup.service
Before=systemd-journal-flush.service sysinit.target
ConditionPathIsDirectory=/var/lib/journal

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'mkdir -p /var/volatile/log/journal && { mountpoint -q /var/volatile/log/journal || mount --bind /var/lib/journal /var/volatile/log/journal; }'

[Install]
WantedBy=sysinit.target
BEOF
echo "[*] Wrote $JOURNAL_BIND_UNIT (oneshot bind, best-effort)"

systemctl daemon-reload
systemctl enable bramka-journal-bind.service >/dev/null 2>&1 || true

# Activate now too (not only after reboot).
mkdir -p "$JOURNAL_WHERE" 2>/dev/null || true
mountpoint -q "$JOURNAL_WHERE" || mount --bind "$PERSIST_JOURNAL_DIR" "$JOURNAL_WHERE" 2>/dev/null || true
systemctl restart systemd-journald 2>/dev/null || true
journalctl --flush 2>/dev/null || true
if mountpoint -q "$JOURNAL_WHERE"; then
    echo "[*] $JOURNAL_WHERE bind-mounted (persistent journal active now)"
else
    echo "[*] NOTE: persistent journal bind not active (diagnostics only; classification unaffected)"
fi

# ---------------------------------------------------------------------------
# Runtime config - defaults only if absent (don't clobber admin/health edits).
# ---------------------------------------------------------------------------
if [ ! -f "$CONF" ]; then
    cat > "$CONF" << 'CEOF'
# Bramka boot-accounting alarm policy (runtime-tunable).
# Edited by admin / future health-monitor; `setup.sh` will NOT overwrite this file.
# Counting and history are always recorded; these knobs only gate the ALARM.

ALARM_ENABLED=1        # 0 = never raise the alarm (still count + log history)
ALARM_THRESHOLD=3      # alarm when boots in the window EXCEED this number
ALARM_WINDOW_HOURS=24  # sliding window (hours) for the boot count
CEOF
    echo "[*] Wrote default $CONF"
else
    echo "[*] $CONF exists - leaving runtime config intact"
fi

# ---------------------------------------------------------------------------
# Accounting script (runs once per boot via the oneshot service).
# ---------------------------------------------------------------------------
cat > "$ACCT_BIN" << 'AEOF'
#!/bin/bash
# bramka-boot-accounting - record this boot, attribute its cause, raise storm alarm.
# Installed by modules/07-boot-accounting.sh. One run per boot (systemd oneshot).
set -u

STATE_DIR="/var/lib/bramka"
COUNT_FILE="$STATE_DIR/boot_count"
HISTORY="$STATE_DIR/boot_history.log"
REASON_FILE="$STATE_DIR/reboot_reason"
CLEAN_FILE="$STATE_DIR/clean_shutdown"
ALARM_FILE="$STATE_DIR/reboot_alarm"
CONF="/etc/bramka/boot-accounting.conf"
HISTORY_MAX_LINES=200
TAG="bramka-boot"

# Defaults; overridable via CONF (admin / health-monitor).
ALARM_ENABLED=1
ALARM_THRESHOLD=3
ALARM_WINDOW_HOURS=24
[ -f "$CONF" ] && . "$CONF"

mkdir -p "$STATE_DIR"

now_epoch=$(date +%s)
now_iso=$(date -Is 2>/dev/null || date -Iseconds 2>/dev/null || date)

# --- total boot counter (monotonic) ---
count=$(cat "$COUNT_FILE" 2>/dev/null)
case "$count" in ''|*[!0-9]*) count=0 ;; esac
count=$((count + 1))
echo "$count" > "$COUNT_FILE"

# --- attribute cause ---
# Classification does NOT depend on the journal (persistent journal is finicky on
# this volatile-/var/log image). It uses two persistent breadcrumbs in /var/lib:
#   - reboot_reason  : written by our software right before an initiated reboot
#                      (Go recoverByReboot). Most specific.
#   - clean_shutdown : written by this unit's ExecStop, so it exists iff the last
#                      shutdown ran systemd's stop sequence (clean reboot/poweroff).
# No breadcrumb + no clean marker => the box went down WITHOUT a clean shutdown =
# hard reset (kernel panic -> HW watchdog, M4F SOC reset, or power loss). If the
# persistent journal happens to be available we refine "hard reset" to "kernel
# panic" as a bonus, but never depend on it.
if [ -f "$REASON_FILE" ]; then
    cause=$(head -1 "$REASON_FILE" 2>/dev/null)
    [ -z "$cause" ] && cause="controlled (no reason text)"
    kind="CONTROLLED"
elif [ -f "$CLEAN_FILE" ]; then
    kind="CONTROLLED"; cause="clean shutdown (manual/orchestrated reboot, no breadcrumb)"
else
    kind="UNEXPECTED"; cause="hard reset (M4F SOC reset / HW watchdog / power loss)"
    if journalctl -k -b -1 --no-pager 2>/dev/null | grep -qi "kernel panic"; then
        cause="kernel panic -> HW watchdog (Warstwa D)"
    fi
fi
# Consume both breadcrumbs (whatever was there) so they don't leak into next boot.
rm -f "$REASON_FILE" "$CLEAN_FILE"

# --- append history: epoch | iso | boot# | kind | cause ---
printf '%s | %s | boot#%s | %s | %s\n' "$now_epoch" "$now_iso" "$count" "$kind" "$cause" >> "$HISTORY"
if [ "$(wc -l < "$HISTORY" 2>/dev/null || echo 0)" -gt "$HISTORY_MAX_LINES" ]; then
    tail -n "$HISTORY_MAX_LINES" "$HISTORY" > "$HISTORY.tmp" && mv "$HISTORY.tmp" "$HISTORY"
fi

logger -t "$TAG" "boot#$count kind=$kind cause=$cause"

# --- storm alarm: count boots within the sliding window (epoch is field 1) ---
window_start=$((now_epoch - ALARM_WINDOW_HOURS * 3600))
recent=$(awk -v ws="$window_start" '$1 ~ /^[0-9]+$/ && $1 >= ws { n++ } END { print n+0 }' "$HISTORY")

if [ "${ALARM_ENABLED:-1}" = "1" ] && [ "${recent:-0}" -gt "$ALARM_THRESHOLD" ]; then
    msg="ALARM: $recent boots in ${ALARM_WINDOW_HOURS}h (> $ALARM_THRESHOLD). Last cause: $cause"
    printf '%s | %s\n' "$now_iso" "$msg" > "$ALARM_FILE"
    logger -t "$TAG" -p daemon.warning "$msg"
else
    rm -f "$ALARM_FILE"
fi

exit 0
AEOF
chmod +x "$ACCT_BIN"
echo "[*] Installed $ACCT_BIN"

# ---------------------------------------------------------------------------
# Status helper (user-facing): bramka-reboots
# ---------------------------------------------------------------------------
cat > "$STATUS_BIN" << 'SEOF'
#!/bin/bash
# bramka-reboots - show reboot accounting status (counter, recent history, alarm).
STATE_DIR="/var/lib/bramka"
CONF="/etc/bramka/boot-accounting.conf"
COUNT_FILE="$STATE_DIR/boot_count"
HISTORY="$STATE_DIR/boot_history.log"
ALARM_FILE="$STATE_DIR/reboot_alarm"

ALARM_ENABLED=1; ALARM_THRESHOLD=3; ALARM_WINDOW_HOURS=24
[ -f "$CONF" ] && . "$CONF"

echo "=== Bramka reboot accounting ==="
echo "Total boots:  $(cat "$COUNT_FILE" 2>/dev/null || echo 0)"
echo "Alarm policy: enabled=$ALARM_ENABLED  threshold=>$ALARM_THRESHOLD / ${ALARM_WINDOW_HOURS}h"

now=$(date +%s); ws=$((now - ALARM_WINDOW_HOURS * 3600))
recent=$(awk -v ws="$ws" '$1 ~ /^[0-9]+$/ && $1 >= ws { n++ } END { print n+0 }' "$HISTORY" 2>/dev/null)
echo "In window:    ${recent:-0} boot(s) in last ${ALARM_WINDOW_HOURS}h"

if [ -f "$ALARM_FILE" ]; then
    echo "ALARM:        *** ACTIVE *** $(cat "$ALARM_FILE")"
else
    echo "Alarm:        clear"
fi

echo
echo "--- last 15 boots (epoch | time | boot# | kind | cause) ---"
if [ -s "$HISTORY" ]; then
    tail -n 15 "$HISTORY" | sed 's/^/  /'
else
    echo "  (no history yet - activates on next boot)"
fi
echo
echo "Tune: edit $CONF  (disable alarm: ALARM_ENABLED=0)"
SEOF
chmod +x "$STATUS_BIN"
# Symlink into /usr/bin for PATH (matches modules/02 convention).
ln -sf "$STATUS_BIN" /usr/bin/bramka-reboots
echo "[*] Installed $STATUS_BIN (+ /usr/bin/bramka-reboots)"

# ---------------------------------------------------------------------------
# Oneshot service - runs accounting once per boot.
# ---------------------------------------------------------------------------
cat > "$UNIT" << 'UEOF'
[Unit]
Description=Bramka boot accounting (reboot counter + cause attribution + storm alarm)
After=systemd-journald.service local-fs.target
Wants=local-fs.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/bin/bramka-boot-accounting
# Clean-shutdown marker: runs only when the unit is stopped, i.e. during a
# systemd shutdown/reboot sequence. Its presence at the next boot proves the
# previous shutdown was clean (vs a hard reset that never ran ExecStop).
ExecStop=/bin/sh -c 'touch /var/lib/bramka/clean_shutdown'

[Install]
WantedBy=multi-user.target
UEOF
echo "[*] Wrote $UNIT"

systemctl daemon-reload
systemctl enable bramka-boot-accounting.service >/dev/null 2>&1 || true

echo "[*] Boot-accounting module complete"
echo "    Aktywuje się przy NASTĘPNYM boocie (oneshot). Podgląd: bramka-reboots"
echo "    Test bez reboota (doda 1 wpis): systemctl restart bramka-boot-accounting && bramka-reboots
    (oneshot+RemainAfterExit: 'start' po pierwszym boocie to no-op, użyj 'restart')"
