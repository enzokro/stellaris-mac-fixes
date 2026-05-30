#!/bin/bash
#
# worm-tweak-mod uninstaller
#
# Removes the Paradox launcher pointer file for this mod. The repo's
# worm-tweak-mod/ directory itself is untouched.
#
# Usage: ./uninstall-mod.sh
#

set -euo pipefail

MOD_NAME="worm-tweak-mod"
POINTER="$HOME/Documents/Paradox Interactive/Stellaris/mod/${MOD_NAME}.mod"

BOLD=$'\033[1m'
GREEN=$'\033[32m'
DIM=$'\033[2m'
RESET=$'\033[0m'

printf "\n${BOLD}worm-tweak-mod uninstaller${RESET}\n"
printf "${DIM}──────────────────────────────${RESET}\n\n"

if [ -f "$POINTER" ]; then
    rm -f "$POINTER"
    printf "  ${GREEN}✓${RESET} Removed $POINTER\n"
else
    printf "  ${DIM}Pointer file was not present; nothing to remove${RESET}\n"
fi

printf "\n  ${GREEN}${BOLD}→${RESET} ${BOLD}Done.${RESET} The mod will no longer appear in the Paradox launcher.\n"
printf "${DIM}     Save games that relied on it should still load; the vanilla Horizon${RESET}\n"
printf "${DIM}     Signal behavior simply returns (existing horizonsignal_spawn star${RESET}\n"
printf "${DIM}     flags persist in those saves).${RESET}\n\n"
