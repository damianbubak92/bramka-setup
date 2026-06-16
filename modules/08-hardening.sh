#!/bin/bash
# 08-hardening.sh - run rpmsg-service as non-root (least privilege)
#
# Cel: zdjąć roota z rpmsg-service. To proces gadający z M4F (RPMsg) i docelowo
# z siecią/chmurą (remote access) - czyli główny cel ataku. Jako root jego
# kompromitacja = pełne przejęcie bramki (podmiana firmware M4F, persystencja,
# pivot na flotę). Non-root z minimalnymi uprawnieniami tnie promień rażenia i
# jest wymogiem security-by-design (CRA) przy sprzedaży setek/tysięcy sztuk w UE.
#
# Trzy uprzywilejowane operacje serwisu, każda przyznana wąsko bez roota:
#   1. /dev/rpmsg0      -> udev rule: grupa `bramka`, 0660
#   2. reboot na PEER DEAD -> wzorzec "path-unit": serwis tylko DOTYKA pliku
#      /run/bramka/reboot-request, a systemd (PID1) robi czysty `systemctl reboot`.
#      Serwis nie ma mocy reboota - może go tylko POPROSIĆ. Bez polkit/sudo/setuid.
#   3. zapis stanu      -> /var/lib/bramka i /run/bramka na własność `bramka`
#
# Binarka serwisu MUSI być poza /root (user `bramka` nie wejdzie do 0700 /root) ->
# deploy do /opt/bramka (Deploy-Go: cel scp zmieniony na /opt/bramka).
#
# Idempotent: re-run safe.
set -e

SVC_USER="bramka"
OPT_DIR="/opt/bramka"
STATE_DIR="/var/lib/bramka"
RUN_DIR="/run/bramka"
UDEV_RULE="/etc/udev/rules.d/70-bramka-rpmsg.rules"
TMPFILES="/etc/tmpfiles.d/bramka.conf"
REBOOT_PATH_UNIT="/etc/systemd/system/bramka-reboot.path"
REBOOT_SVC_UNIT="/etc/systemd/system/bramka-reboot.service"

echo "[*] Hardening: run rpmsg-service as non-root user '$SVC_USER'"

# ---------------------------------------------------------------------------
# System user + group (no login). Robust across shadow (useradd) and busybox.
# ---------------------------------------------------------------------------
if ! getent group "$SVC_USER" >/dev/null 2>&1; then
    groupadd --system "$SVC_USER" 2>/dev/null || addgroup -S "$SVC_USER"
    echo "[*] Created group $SVC_USER"
else
    echo "[*] Group $SVC_USER exists"
fi
if ! getent passwd "$SVC_USER" >/dev/null 2>&1; then
    useradd --system --gid "$SVC_USER" --no-create-home \
        --shell /usr/sbin/nologin "$SVC_USER" 2>/dev/null \
        || adduser -S -D -H -G "$SVC_USER" -s /sbin/nologin "$SVC_USER"
    echo "[*] Created user $SVC_USER"
else
    echo "[*] User $SVC_USER exists"
fi

# ---------------------------------------------------------------------------
# Directories owned by the service user.
# ---------------------------------------------------------------------------
mkdir -p "$OPT_DIR" "$STATE_DIR"
chown -R "$SVC_USER:$SVC_USER" "$OPT_DIR" "$STATE_DIR"
echo "[*] $OPT_DIR + $STATE_DIR owned by $SVC_USER"

# /run is tmpfs (recreated each boot) -> tmpfiles.d.
cat > "$TMPFILES" << TEOF
# Managed by bramka-setup modules/08-hardening.sh
# Reboot-request trigger dir, writable by the non-root rpmsg-service.
d $RUN_DIR 0775 $SVC_USER $SVC_USER -
TEOF
systemd-tmpfiles --create "$TMPFILES" >/dev/null 2>&1 || mkdir -p "$RUN_DIR"
chown "$SVC_USER:$SVC_USER" "$RUN_DIR" 2>/dev/null || true
echo "[*] $RUN_DIR (tmpfiles) writable by $SVC_USER"

# ---------------------------------------------------------------------------
# /dev/rpmsg* accessible to the service group (no root needed).
# ---------------------------------------------------------------------------
cat > "$UDEV_RULE" << UEOF
# Managed by bramka-setup modules/08-hardening.sh
# Let the non-root rpmsg-service open the M4F endpoint. Match the numbered char
# device nodes (/dev/rpmsg0..) by name; no SUBSYSTEM constraint so the rule fires
# regardless of how the rpmsg_chrdev node is classified.
KERNEL=="rpmsg[0-9]*", GROUP="$SVC_USER", MODE="0660"
UEOF
udevadm control --reload 2>/dev/null || true
udevadm trigger --subsystem-match=rpmsg 2>/dev/null || true
echo "[*] Wrote $UDEV_RULE (/dev/rpmsg* -> group $SVC_USER)"

# ---------------------------------------------------------------------------
# Clean-reboot path-unit: the non-root service requests a reboot by creating
# /run/bramka/reboot-request; systemd (root) performs the clean reboot.
# ---------------------------------------------------------------------------
cat > "$REBOOT_PATH_UNIT" << 'PEOF'
[Unit]
Description=Watch for a bramka reboot request (from non-root rpmsg-service)

[Path]
PathExists=/run/bramka/reboot-request
Unit=bramka-reboot.service

[Install]
WantedBy=multi-user.target
PEOF

cat > "$REBOOT_SVC_UNIT" << 'REOF'
[Unit]
Description=Bramka clean reboot (privileged action requested via bramka-reboot.path)
# The "why" breadcrumb (/var/lib/bramka/reboot_reason) was already written by the
# requester and is consumed by boot accounting on the next boot.

[Service]
Type=oneshot
# Consume the trigger, then do a clean systemd reboot. sh -c so systemctl is
# resolved via PATH (location varies across images).
ExecStart=/bin/sh -c 'rm -f /run/bramka/reboot-request; exec systemctl reboot'
REOF
echo "[*] Wrote bramka-reboot.path + bramka-reboot.service"

systemctl daemon-reload
systemctl enable bramka-reboot.path >/dev/null 2>&1 || true
systemctl start bramka-reboot.path 2>/dev/null || true

echo "[*] Hardening module complete"
echo "    WAŻNE: deploy binarki do $OPT_DIR (zmień cel w Deploy-Go z /root/bramka-services)."
echo "    Po Deploy-Go + Install-GoService (nowy unit) + restart serwis leci jako '$SVC_USER'."
echo "    Weryfikacja: systemctl show -p User,MainPID rpmsg-service ; ps -o user= -p \$(systemctl show -p MainPID --value rpmsg-service)"
