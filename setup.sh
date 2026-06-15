#!/bin/bash
# setup.sh - Main orchestrator for bramka setup
# Usage: sudo ./setup.sh
#
# Runs all modules in /modules/ in alphabetical order.
# Each module is idempotent - safe to run multiple times.

set -e  # exit on any error
set -u  # exit on undefined variable

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Load config:
if [ ! -f "$SCRIPT_DIR/config.sh" ]; then
    echo "ERROR: config.sh not found in $SCRIPT_DIR"
    echo "Copy config.sh.example to config.sh and edit it"
    exit 1
fi
source "$SCRIPT_DIR/config.sh"

# Ensure all module scripts are executable (git may have lost the bit):
log_info "Setting executable bit on modules..."
chmod +x "$SCRIPT_DIR"/modules/*.sh

# Oraz lepiej zmień check w pętli z "[ -x ]" na "[ -f ]":
for module in "$SCRIPT_DIR"/modules/*.sh; do
    if [ -f "$module" ]; then     ← było [ -x "$module" ]
        log_step "Running $(basename "$module")"
        bash "$module" || {        ← wywołuj przez bash, executable bit nieważny
            log_error "Module $(basename "$module") FAILED"
            exit 1
        }
    fi
done


# Colors for output:
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }
log_step()  { echo -e "\n${GREEN}===${NC} $* ${GREEN}===${NC}"; }

# Check that we're root:
if [ "$EUID" -ne 0 ]; then
    log_error "This script must be run as root"
    exit 1
fi

# Banner:
log_step "Bramka Setup - $(hostname) at $(date)"
log_info "Target MAC:      $BRAMKA_MAC"
log_info "Target hostname: $BRAMKA_HOSTNAME"
log_info "Tools dir:       $TOOLS_DIR"
log_info "Firmware dir:    $FW_DIR"

# Export variables and log functions so modules can use them:
export BRAMKA_MAC BRAMKA_HOSTNAME BRAMKA_IP_STATIC
export ETH_HW_PATH ETH_INTERFACE_NAME
export TOOLS_DIR FW_DIR DEFAULT_FW_NAME
export ENABLE_M4F_AUTOSTOP
export SCRIPT_DIR

# Run modules in order:
for module in "$SCRIPT_DIR"/modules/*.sh; do
    if [ -x "$module" ]; then
        log_step "Running $(basename "$module")"
        "$module" || {
            log_error "Module $(basename "$module") FAILED"
            exit 1
        }
    fi
done

log_step "Setup complete!"
log_info "Reboot recommended to verify everything persists."
log_info ""
log_info "After reboot, verify with:"
log_info "  ip link show $ETH_INTERFACE_NAME    # MAC should be $BRAMKA_MAC"
log_info "  hostname                              # should show $BRAMKA_HOSTNAME"
log_info "  m4f-watch                             # M4F live trace"
log_info ""
log_info "Don't forget to configure DHCP reservation in router:"
log_info "  MAC: $BRAMKA_MAC"
