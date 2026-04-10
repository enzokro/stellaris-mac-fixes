#!/bin/bash
#
# stellaris-mac-fixes uninstaller
#
# Restores Stellaris to its original state: removes the dylib, removes
# the wrapper script, and restores launcher-settings.json from backup.
#

set -euo pipefail

STELLARIS_DIR="${STELLARIS_DIR:-$HOME/Library/Application Support/Steam/steamapps/common/Stellaris}"
DYLIB="libstellaris_fix.dylib"
WRAPPER="stellaris_wrapper.sh"
LAUNCHER="launcher-settings.json"

BOLD=$'\033[1m'
GREEN=$'\033[32m'
YELLOW=$'\033[33m'
DIM=$'\033[2m'
RESET=$'\033[0m'

say()  { printf "%s\n" "$*"; }
ok()   { printf "  ${GREEN}✓${RESET} %s\n" "$*"; }
warn() { printf "  ${YELLOW}!${RESET} %s\n" "$*"; }
step() { printf "\n${BOLD}%s${RESET}\n" "$*"; }

say ""
say "${BOLD}stellaris-mac-fixes uninstaller${RESET}"
say "${DIM}──────────────────────────────────${RESET}"

if [ ! -d "$STELLARIS_DIR" ]; then
    warn "Stellaris directory not found at $STELLARIS_DIR — nothing to do."
    exit 0
fi

step "Removing fix"

# Restore launcher-settings.json from backup
LAUNCHER_PATH="$STELLARIS_DIR/$LAUNCHER"
if [ -f "$LAUNCHER_PATH.bak" ]; then
    mv "$LAUNCHER_PATH.bak" "$LAUNCHER_PATH"
    ok "Restored $LAUNCHER from backup"
else
    warn "No backup of $LAUNCHER found — leaving current file untouched"
fi

# Remove wrapper script
WRAPPER_PATH="$STELLARIS_DIR/stellaris.app/Contents/MacOS/$WRAPPER"
if [ -f "$WRAPPER_PATH" ]; then
    rm "$WRAPPER_PATH"
    ok "Removed wrapper script"
fi

# Remove dylib
DYLIB_PATH="$STELLARIS_DIR/$DYLIB"
if [ -f "$DYLIB_PATH" ]; then
    rm "$DYLIB_PATH"
    ok "Removed $DYLIB"
fi

step "Uninstall complete"
say ""
say "  Stellaris has been restored to its original state."
say ""
