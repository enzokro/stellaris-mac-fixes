#!/bin/bash
#
# stellaris-mac-fixes installer
#
# Installs libstellaris_fix.dylib and configures the Paradox launcher to
# load it via an "Increased Stack Size" launch option.
#
# Usage:  ./install.sh
# Custom: STELLARIS_DIR="/path/to/Stellaris" ./install.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STELLARIS_DIR="${STELLARIS_DIR:-$HOME/Library/Application Support/Steam/steamapps/common/Stellaris}"
DYLIB="libstellaris_fix.dylib"
WRAPPER="stellaris_wrapper.sh"
LAUNCHER="launcher-settings.json"

# Expected MD5 of the bundled dylib. Updated by `make release`.
# Set SKIP_CHECKSUM_VERIFY=1 to bypass (for devs who rebuild locally).
EXPECTED_DYLIB_MD5="5fb5604a1b4a3df071931bea7d872e67"
SKIP_CHECKSUM_VERIFY="${SKIP_CHECKSUM_VERIFY:-0}"

# ── Pretty output ─────────────────────────────────────────────────────────

BOLD=$'\033[1m'
GREEN=$'\033[32m'
YELLOW=$'\033[33m'
RED=$'\033[31m'
DIM=$'\033[2m'
RESET=$'\033[0m'

say()   { printf "%s\n" "$*"; }
info()  { printf "  ${DIM}%s${RESET}\n" "$*"; }
ok()    { printf "  ${GREEN}✓${RESET} %s\n" "$*"; }
warn()  { printf "  ${YELLOW}!${RESET} %s\n" "$*"; }
fail()  { printf "  ${RED}✗${RESET} %s\n" "$*" >&2; }
step()  { printf "\n${BOLD}%s${RESET}\n" "$*"; }

# ── Banner ────────────────────────────────────────────────────────────────

say ""
say "${BOLD}stellaris-mac-fixes installer${RESET}"
say "${DIM}────────────────────────────────${RESET}"

# ── Validate Stellaris directory ──────────────────────────────────────────

step "Checking Stellaris installation"

if [ ! -d "$STELLARIS_DIR" ]; then
    fail "Stellaris directory not found at:"
    fail "  $STELLARIS_DIR"
    say ""
    say "If Stellaris is installed somewhere else, rerun with:"
    say "  ${BOLD}STELLARIS_DIR=\"/path/to/Stellaris\" ./install.sh${RESET}"
    say ""
    exit 1
fi
ok "Found: $STELLARIS_DIR"

MACOS_DIR="$STELLARIS_DIR/stellaris.app/Contents/MacOS"
if [ ! -f "$MACOS_DIR/stellaris" ]; then
    fail "Stellaris executable not found at expected path:"
    fail "  $MACOS_DIR/stellaris"
    exit 1
fi
ok "Found game binary"

# ── Locate or build the dylib ─────────────────────────────────────────────

step "Preparing the fix library"

DYLIB_PATH="$SCRIPT_DIR/$DYLIB"

if [ ! -f "$DYLIB_PATH" ]; then
    info "Pre-built dylib not found, building from source..."
    if ! command -v cc >/dev/null 2>&1; then
        fail "No C compiler found. Install Xcode Command Line Tools:"
        fail "  xcode-select --install"
        exit 1
    fi
    if ! make -C "$SCRIPT_DIR" all >/dev/null 2>&1; then
        fail "Build failed. Try running 'make' manually to see the error."
        exit 1
    fi
    ok "Built $DYLIB"
else
    ok "Using pre-built $DYLIB"
fi

# Verify it's a valid Mach-O binary
if ! file "$DYLIB_PATH" | grep -q "Mach-O"; then
    fail "$DYLIB is not a valid Mach-O binary"
    exit 1
fi

# ── Verify dylib checksum ─────────────────────────────────────────────────

verify_md5() {
    local file="$1" expected="$2" actual
    if ! command -v md5 >/dev/null 2>&1; then
        warn "md5 command not available — skipping checksum verification"
        return 0
    fi
    actual=$(md5 -q "$file" 2>/dev/null)
    if [ -z "$actual" ]; then
        warn "Could not compute MD5 of $file — skipping verification"
        return 0
    fi
    if [ "$actual" != "$expected" ]; then
        fail "MD5 verification FAILED for $(basename "$file")"
        fail "  expected: $expected"
        fail "  actual:   $actual"
        say ""
        say "${RED}This means the file is corrupted, has been tampered with,"
        say "or you have an older/newer dylib than this installer was built for.${RESET}"
        say ""
        say "  • If you downloaded a release: re-download from the original source"
        say "    and verify the zip's hash against the release page."
        say "  • If you built the dylib yourself: re-run with"
        say "    ${BOLD}SKIP_CHECKSUM_VERIFY=1 ./install.sh${RESET}"
        say ""
        return 1
    fi
    return 0
}

if [ "$SKIP_CHECKSUM_VERIFY" = "1" ]; then
    warn "Skipping checksum verification (SKIP_CHECKSUM_VERIFY=1)"
elif [ "$EXPECTED_DYLIB_MD5" = "__DYLIB_MD5_PLACEHOLDER__" ]; then
    warn "No checksum embedded — this is a development build"
elif ! verify_md5 "$DYLIB_PATH" "$EXPECTED_DYLIB_MD5"; then
    exit 1
else
    ok "Checksum verified (MD5: $EXPECTED_DYLIB_MD5)"
fi

# ── Install the dylib ─────────────────────────────────────────────────────

step "Installing files"

cp -f "$DYLIB_PATH" "$STELLARIS_DIR/$DYLIB"
ok "Installed $DYLIB"

# ── Create the wrapper script ─────────────────────────────────────────────

cat > "$MACOS_DIR/$WRAPPER" << 'WRAPPER_EOF'
#!/bin/bash
# stellaris-mac-fixes wrapper — sets DYLD_INSERT_LIBRARIES and execs the game
DIR="$(cd "$(dirname "$0")" && pwd)"
export DYLD_INSERT_LIBRARIES="${DIR}/../../../libstellaris_fix.dylib"
exec "${DIR}/stellaris" "$@"
WRAPPER_EOF
chmod +x "$MACOS_DIR/$WRAPPER"
ok "Installed wrapper script"

# ── Patch launcher-settings.json ──────────────────────────────────────────

LAUNCHER_PATH="$STELLARIS_DIR/$LAUNCHER"

if [ ! -f "$LAUNCHER_PATH" ]; then
    warn "$LAUNCHER not found — skipping launcher integration"
    say ""
    say "You can still launch manually with:"
    say "  ${BOLD}DYLD_INSERT_LIBRARIES=\"$STELLARIS_DIR/$DYLIB\" \\${RESET}"
    say "  ${BOLD}  \"$MACOS_DIR/stellaris\" -gdpr-compliant${RESET}"
    exit 0
fi

# Back up the original (only on first install)
if [ ! -f "$LAUNCHER_PATH.bak" ]; then
    cp "$LAUNCHER_PATH" "$LAUNCHER_PATH.bak"
    ok "Backed up original $LAUNCHER → $LAUNCHER.bak"
else
    info "Backup already exists: $LAUNCHER.bak"
fi

# Re-patch from the backup so we don't double-apply on repeated installs
cp "$LAUNCHER_PATH.bak" "$LAUNCHER_PATH"

if ! command -v python3 >/dev/null 2>&1; then
    warn "python3 not found — cannot patch $LAUNCHER automatically"
    say ""
    say "The dylib is installed but the launcher entry is missing."
    say "Either install Python 3, or manually edit $LAUNCHER to add a"
    say "wrapper-based alternativeExecutables entry."
    exit 0
fi

python3 - "$LAUNCHER_PATH" << 'PYTHON_EOF'
import json, sys
path = sys.argv[1]
with open(path, 'r') as f:
    cfg = json.load(f)

fix_entry = {
    'exePath': './stellaris.app/Contents/MacOS/stellaris_wrapper.sh',
    'exeArgs': cfg.get('exeArgs', ['-gdpr-compliant']),
    'label': {
        'en': 'Increased Stack Size',
        'de': 'Erhöhte Stapelgröße',
        'fr': 'Taille de pile augmentée',
        'es': 'Tamaño de pila aumentado',
        'ja': 'スタックサイズ増加',
        'ko': '스택 크기 증가',
        'pt': 'Tamanho de pilha aumentado',
        'ru': 'Увеличенный размер стека',
        'zh-hans': '增加堆栈大小',
        'zh-hant': '增加堆疊大小',
        'pl': 'Zwiększony rozmiar stosu',
        'tr': 'Artırılmış Yığın Boyutu',
    },
    'visibleIn': ['GAME_SETTINGS', 'HOME_PAGE']
}

alts = cfg.get('alternativeExecutables', [])
alts.insert(0, fix_entry)
cfg['alternativeExecutables'] = alts

with open(path, 'w') as f:
    json.dump(cfg, f, indent='\t', ensure_ascii=False)
    f.write('\n')
PYTHON_EOF
ok "Patched $LAUNCHER"

# ── Done ──────────────────────────────────────────────────────────────────

step "Installation complete"
say ""
say "  ${GREEN}${BOLD}→${RESET} ${BOLD}Launch Stellaris via Steam${RESET}"
say "  ${GREEN}${BOLD}→${RESET} ${BOLD}In the Paradox launcher, select ${GREEN}\"Increased Stack Size\"${RESET}"
say "       from the launch options, then click Play."
say ""
say "${DIM}To uninstall: run ${RESET}${BOLD}./uninstall.sh${RESET}${DIM} or double-click Uninstall.command${RESET}"
say "${DIM}For verbose diagnostic logging, set Steam launch options to:${RESET}"
say "${DIM}   STELLARIS_FIX_DEBUG=1 %command%${RESET}"
say ""
