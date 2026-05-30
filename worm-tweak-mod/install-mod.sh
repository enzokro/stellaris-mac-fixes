#!/bin/bash
#
# worm-tweak-mod installer
#
# Registers the local mod with the Paradox launcher by writing a .mod pointer
# file at ~/Documents/Paradox Interactive/Stellaris/mod/worm-tweak-mod.mod
# that points `path=` at this repo's worm-tweak-mod/ directory.
#
# Usage: ./install-mod.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MOD_NAME="worm-tweak-mod"
PDX_MOD_DIR="$HOME/Documents/Paradox Interactive/Stellaris/mod"
POINTER="$PDX_MOD_DIR/${MOD_NAME}.mod"

BOLD=$'\033[1m'
GREEN=$'\033[32m'
YELLOW=$'\033[33m'
RED=$'\033[31m'
DIM=$'\033[2m'
RESET=$'\033[0m'

say()  { printf "%s\n" "$*"; }
ok()   { printf "  ${GREEN}✓${RESET} %s\n" "$*"; }
warn() { printf "  ${YELLOW}!${RESET} %s\n" "$*"; }
fail() { printf "  ${RED}✗${RESET} %s\n" "$*" >&2; }
step() { printf "\n${BOLD}%s${RESET}\n" "$*"; }

say ""
say "${BOLD}worm-tweak-mod installer${RESET}"
say "${DIM}────────────────────────────${RESET}"

step "Verifying mod contents"

for f in descriptor.mod events/horizonsignal_events.txt events/worm_tweak_events.txt \
         common/on_actions/zz_worm_tweak.txt localisation/english/worm_tweak_l_english.yml; do
    if [ ! -f "$SCRIPT_DIR/$f" ]; then
        fail "Missing: $f"
        exit 1
    fi
done
ok "All expected files present"

step "Registering with Paradox launcher"

mkdir -p "$PDX_MOD_DIR"

cat > "$POINTER" <<MOD_EOF
name="Worm Tweak"
version="1.0"
tags={
	"Events"
	"Gameplay"
}
supported_version="v4.3.*"
picture="thumbnail.png"
replace_path="events/horizonsignal_events.txt"
path="$SCRIPT_DIR"
MOD_EOF
ok "Wrote pointer: $POINTER"

step "Installation complete"
say ""
say "  ${GREEN}${BOLD}→${RESET} ${BOLD}Open the Paradox launcher${RESET}"
say "  ${GREEN}${BOLD}→${RESET} Under ${BOLD}Mods → Browse${RESET}, find ${GREEN}\"Worm Tweak\"${RESET} and enable it"
say "  ${GREEN}${BOLD}→${RESET} Start a new game — a configuration popup appears on day 1"
say ""
say "${DIM}To uninstall: ./uninstall-mod.sh${RESET}"
say ""
