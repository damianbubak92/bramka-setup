#!/bin/bash
# 09-timezone.sh - set the system timezone + enable NTP time sync.
#
# Problem solved: a fresh Arago image defaults to UTC and ships MINIMAL tzdata
# (no Europe/Warsaw -> `timedatectl set-timezone` fails). The gen2 automation
# engine (M4F, no RTC) runs on the wall-clock the Go service injects, and the Go
# service now EMBEDS its own tzdata (import _ "time/tzdata") + loads the zone via
# -tz, so RULE TIMING IS CORRECT regardless of this module. This module fixes the
# *system* clock (date/logs/boot-accounting) for human readability.
#
# Since the image lacks the zoneinfo file, we extract it from Go's bundled tzdata
# (Go is installed by 04-go.sh). If that fails, we WARN and continue (UTC) - the
# engine is unaffected, so setup must not break over a cosmetic system-clock zone.
#
# Idempotent: re-run safe. TIMEZONE comes from config.sh (default Europe/Warsaw).
set -e

TIMEZONE="${TIMEZONE:-Europe/Warsaw}"
ZONE_FILE="/usr/share/zoneinfo/${TIMEZONE}"
GO_BIN="/usr/local/go/bin/go"
GO_TZZIP="/usr/local/go/lib/time/zoneinfo.zip"

echo "[*] Configuring timezone: ${TIMEZONE}"

# 1) If the zone file is missing, provide it from Go's tzdata zip. Fast paths
#    first (unzip / python3 - instant); the Go extractor is a last-resort fallback
#    (`go run` recompiles archive/zip + stdlib on the A53 -> ~2-3 min one-time).
if [ ! -f "$ZONE_FILE" ] && [ -f "$GO_TZZIP" ]; then
    echo "[*] zoneinfo '${TIMEZONE}' not on image - extracting from Go tzdata"
    mkdir -p "$(dirname "$ZONE_FILE")"

    # 1a) unzip (busybox or full) - stores entry at path "Europe/Warsaw".
    if [ ! -f "$ZONE_FILE" ] && command -v unzip >/dev/null 2>&1; then
        unzip -o -d /usr/share/zoneinfo "$GO_TZZIP" "$TIMEZONE" >/dev/null 2>&1 \
            && echo "[*] extracted via unzip -> ${ZONE_FILE}" || true
    fi

    # 1b) python3 zipfile.
    if [ ! -f "$ZONE_FILE" ] && command -v python3 >/dev/null 2>&1; then
        python3 - "$GO_TZZIP" "$TIMEZONE" "$ZONE_FILE" <<'PY' && echo "[*] extracted via python3 -> ${ZONE_FILE}" || true
import sys, zipfile, os
zf, name, dest = sys.argv[1], sys.argv[2], sys.argv[3]
data = zipfile.ZipFile(zf).read(name)
os.makedirs(os.path.dirname(dest), exist_ok=True)
open(dest, "wb").write(data)
PY
    fi

    # 1c) Go extractor (guaranteed present, but slow first run).
    if [ ! -f "$ZONE_FILE" ] && [ -x "$GO_BIN" ]; then
        echo "[*] (no unzip/python3 - using Go extractor, ~2-3 min first run)"
        TMPGO="$(mktemp -d)"
        cat > "$TMPGO/extract.go" <<'GOEOF'
package main

import (
	"archive/zip"
	"io"
	"os"
	"path/filepath"
)

func main() {
	zf, name, dest := os.Args[1], os.Args[2], os.Args[3]
	r, err := zip.OpenReader(zf)
	if err != nil {
		os.Exit(1)
	}
	defer r.Close()
	for _, f := range r.File {
		if f.Name == name {
			rc, err := f.Open()
			if err != nil {
				os.Exit(1)
			}
			defer rc.Close()
			_ = os.MkdirAll(filepath.Dir(dest), 0o755)
			out, err := os.Create(dest)
			if err != nil {
				os.Exit(1)
			}
			defer out.Close()
			_, _ = io.Copy(out, rc)
			return
		}
	}
	os.Exit(2) // zone not found in zip
}
GOEOF
        GOCACHE=/tmp/gocache GOPATH=/root/go "$GO_BIN" run "$TMPGO/extract.go" \
            "$GO_TZZIP" "$TIMEZONE" "$ZONE_FILE" \
            && echo "[*] extracted via go -> ${ZONE_FILE}" \
            || echo "[!] Go extractor failed"
        rm -rf "$TMPGO"
    fi
fi

# 2) Apply, or warn-and-continue if we still don't have the zone.
if [ ! -f "$ZONE_FILE" ]; then
    echo "WARNING: could not install zoneinfo for '${TIMEZONE}'; system clock stays UTC."
    echo "         The automation engine is UNAFFECTED (Go service embeds its own"
    echo "         tzdata via -tz). This only affects system date/log readability."
    exit 0
fi

CURRENT_TZ="$(readlink -f /etc/localtime 2>/dev/null | sed 's#.*/zoneinfo/##')"
if [ "$CURRENT_TZ" = "$TIMEZONE" ]; then
    echo "[*] Already set to ${TIMEZONE}"
else
    echo "[*] Setting timezone ${CURRENT_TZ:-unknown} -> ${TIMEZONE}"
    if command -v timedatectl >/dev/null 2>&1; then
        timedatectl set-timezone "$TIMEZONE" 2>/dev/null || ln -sf "$ZONE_FILE" /etc/localtime
    else
        ln -sf "$ZONE_FILE" /etc/localtime
        echo "$TIMEZONE" > /etc/timezone
    fi
fi

# 3) NTP keeps the RTC-less gateway accurate while online (best-effort).
if command -v timedatectl >/dev/null 2>&1; then
    timedatectl set-ntp true 2>/dev/null || true
fi

echo "[*] Current time/date:"
date
echo "[*] Timezone module complete"
