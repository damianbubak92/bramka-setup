#!/bin/bash
# config.sh - Configuration for THIS specific bramka
# Edit before running setup.sh on a new device.

# ============================================================================
# Network identity
# ============================================================================

# MAC address for eth1 - MUST be unique per bramka.
# Use locally administered prefix: 02, 06, 0a, 0e, 22, 26, 2a, 2e...
# (second bit of first octet = 1)
BRAMKA_MAC="22:F4:99:37:A5:12"

# Hostname for this device.
BRAMKA_HOSTNAME="bramka-01"

# Timezone (IANA name). Critical: the automation engine runs on the wall-clock
# the Go service injects from Linux local time, so COND_TIME rules fire at the
# WRONG hour if this is wrong (e.g. UTC). Set to the deployment location.
TIMEZONE="Europe/Warsaw"

# Static IP - leave empty to use DHCP with router-side reservation (recommended).
# Or set "192.168.2.170/24" to configure static IP directly on bramka.
BRAMKA_IP_STATIC=""

# ============================================================================
# Network HW match (rarely needs changing - AM62 specific)
# ============================================================================

# Hardware path of the ethernet controller (from `udevadm info /sys/class/net/eth1`)
ETH_HW_PATH="platform-8000000.ethernet"

# Final interface name after rename
ETH_INTERFACE_NAME="eth1"

# ============================================================================
# Paths and tools
# ============================================================================

# Where to install dev/debug tools (m4f-watch, m4f-reload).
# These are also symlinked to /usr/bin/ for convenience.
TOOLS_DIR="/root"

# M4F firmware location (TI Arago Linux convention)
FW_DIR="/lib/firmware/ti-ipc/am62xx"
DEFAULT_FW_NAME="ipc_echo_test_mcu2_0_release_strip.xer5f"

# ============================================================================
# M4F development behavior
# ============================================================================

# Auto-stop M4F on every boot? "yes" for active dev (skips default firmware load),
# "no" for normal operation (default firmware loads at boot).
ENABLE_M4F_AUTOSTOP="no"
