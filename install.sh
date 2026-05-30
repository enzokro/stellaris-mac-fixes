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

# Companion mod (script-level CTD prevention). Optional — installer skips
# silently if companion-mod/ directory is absent (lets users opt out by
# deleting the directory before running install).
COMPANION_MOD_DIR="$SCRIPT_DIR/companion-mod"
COMPANION_MOD_NAME="stellaris-mac-fixes-companion"
PDX_MOD_DIR="$HOME/Documents/Paradox Interactive/Stellaris/mod"
COMPANION_POINTER="$PDX_MOD_DIR/${COMPANION_MOD_NAME}.mod"

# Layer 4/5 — observability + lifecycle.
SIDECAR_DIR="$HOME/.config/stellaris-mac-fixes"
SIDECAR_EXPECTED="$SIDECAR_DIR/expected_text_end"
LAUNCHD_PLIST="$HOME/Library/LaunchAgents/com.stellaris-fix.drift.plist"
LAUNCHD_LABEL="com.stellaris-fix.drift"
PLIST_TEMPLATE="$SCRIPT_DIR/com.stellaris-fix.drift.plist.template"
DRIFT_CHECK="$SCRIPT_DIR/bin/stellaris-fix-drift-check"
DOCTOR="$SCRIPT_DIR/bin/stellaris-fix-doctor"

# Expected MD5 of the bundled dylib. Updated by `make release`.
# Set SKIP_CHECKSUM_VERIFY=1 to bypass (for devs who rebuild locally).
EXPECTED_DYLIB_MD5="e458d0bc2120ec1d4bcdebcb7cd73799"
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

# ── Capture Paradox launcher app version (observational) ─────────────────
#
# The Paradox bootstrapper keeps multiple launcher-v2.* dirs around and runs
# the newest one with a valid .cpatch marker. We record whichever is active
# so we can spot when it shifts between installs — the launcher app updates
# independently of the game binary and isn't tracked elsewhere.

step "Checking Paradox launcher version"

PDX_DIR="$HOME/Library/Application Support/Paradox Interactive"
STATE_DIR="$HOME/.local/state/stellaris-mac-fixes"
STATE_LOG="$STATE_DIR/install.log"

LAUNCHER_VERSION=""
if [ -d "$PDX_DIR" ]; then
    while IFS= read -r dir; do
        [ -f "$dir/.cpatch/launcher-v2_1/version" ] || continue
        LAUNCHER_VERSION="${dir##*/launcher-v2.}"
    done < <(find "$PDX_DIR" -maxdepth 1 -type d -name 'launcher-v2.*' 2>/dev/null | sort -V)
fi

if [ -z "$LAUNCHER_VERSION" ]; then
    warn "Paradox launcher not detected — skipping version log."
else
    ok "Active Paradox launcher: $LAUNCHER_VERSION"
    if [ -f "$STATE_LOG" ]; then
        prev=$(grep 'launcher-app-version:' "$STATE_LOG" 2>/dev/null | tail -1 | awk '{print $NF}')
        if [ -n "$prev" ] && [ "$prev" != "$LAUNCHER_VERSION" ]; then
            info "(was $prev at last install)"
        elif [ -n "$prev" ]; then
            info "(unchanged since last install)"
        fi
    fi
    mkdir -p "$STATE_DIR"
    printf '%s\tlauncher-app-version: %s\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$LAUNCHER_VERSION" >> "$STATE_LOG"
fi

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

# Snapshot a pristine .bak whenever the current file is in an unpatched state
# (first install, or after a Steam game-update refreshed launcher-settings.json
# and wiped our wrapper entry). Don't blindly restore from .bak — that would
# overwrite a freshly-deployed launcher config with stale content. Idempotency
# is enforced inside the python patcher instead (it strips existing wrapper
# entries before inserting).
if grep -q "$WRAPPER" "$LAUNCHER_PATH"; then
    info "$LAUNCHER already contains wrapper entry — patching in place (idempotent)"
else
    cp "$LAUNCHER_PATH" "$LAUNCHER_PATH.bak"
    ok "Snapshotted pristine $LAUNCHER → $LAUNCHER.bak"
fi

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
# Idempotency: drop any pre-existing wrapper entries before inserting fresh.
alts = [e for e in alts if e.get('exePath') != fix_entry['exePath']]
alts.insert(0, fix_entry)
cfg['alternativeExecutables'] = alts

with open(path, 'w') as f:
    json.dump(cfg, f, indent='\t', ensure_ascii=False)
    f.write('\n')
PYTHON_EOF
ok "Patched $LAUNCHER"

# ── Companion mod (optional) ──────────────────────────────────────────────

COMPANION_INSTALLED=0
if [ -d "$COMPANION_MOD_DIR" ]; then
    step "Registering companion mod (script-level CTD prevention)"

    missing=0
    for f in descriptor.mod thumbnail.png \
             common/on_actions/zz_smf_companion.txt \
             events/smf_planet_transfer.txt \
             events/smf_dead_war.txt \
             events/smf_army_counter.txt \
             events/smf_save_repair.txt \
             events/smf_dlc_cyber.txt \
             events/smf_dlc_anomaly.txt \
             events/!smf_overrides.txt; do
        if [ ! -f "$COMPANION_MOD_DIR/$f" ]; then
            warn "Companion mod missing: $f"
            missing=1
        fi
    done

    if [ "$missing" = "1" ]; then
        warn "Companion mod files incomplete — skipping registration."
        warn "The dylib is still installed and active."
    else
        mkdir -p "$PDX_MOD_DIR"
        cat > "$COMPANION_POINTER" << POINTER_EOF
name="Stellaris Mac CTD Fixes (companion)"
version="0.1.0"
tags={
	"Fixes"
}
supported_version="v4.3.*"
picture="thumbnail.png"
path="$COMPANION_MOD_DIR"
POINTER_EOF
        ok "Wrote launcher pointer: $COMPANION_POINTER"
        COMPANION_INSTALLED=1
    fi
fi

# ── Sidecar: expected __DATA vmaddr ────────────────────────────────────────
#
# Extract STELLARIS_TEXT_END from stellaris_fix.c so the doctor and the drift
# detector can compare it against the live game binary without re-parsing the
# .c source on every run. Idempotent — re-install overwrites with the current
# value.

step "Writing sidecar (expected game __DATA vmaddr)"

EXPECTED_TEXT_END=$(awk '/^#define[[:space:]]+STELLARIS_TEXT_END_DEFAULT[[:space:]]+/{
    val=$3; gsub(/[ULul]+$/, "", val); print val; exit
}' "$SCRIPT_DIR/stellaris_fix.c")

if [ -z "$EXPECTED_TEXT_END" ]; then
    warn "Could not extract STELLARIS_TEXT_END from stellaris_fix.c — skipping sidecar."
else
    mkdir -p "$SIDECAR_DIR"
    echo "$EXPECTED_TEXT_END" > "$SIDECAR_EXPECTED"
    ok "Wrote $SIDECAR_EXPECTED ($EXPECTED_TEXT_END)"
fi

# ── Drift detector (launchd agent) ─────────────────────────────────────────

DRIFT_INSTALLED=0
if [ -f "$PLIST_TEMPLATE" ] && [ -x "$DRIFT_CHECK" ]; then
    step "Installing drift detector (launchd agent)"

    mkdir -p "$(dirname "$LAUNCHD_PLIST")"

    # Render template — substitute @REPO_ROOT@ + @GAME_BINARY@. Use sed with
    # | as delimiter since paths contain /. Escape & in paths defensively.
    sed_repo=$(printf '%s' "$SCRIPT_DIR" | sed -e 's/[\&|]/\\&/g')
    sed_bin=$(printf '%s' "$MACOS_DIR/stellaris" | sed -e 's/[\&|]/\\&/g')
    sed -e "s|@REPO_ROOT@|$sed_repo|g" \
        -e "s|@GAME_BINARY@|$sed_bin|g" \
        "$PLIST_TEMPLATE" > "$LAUNCHD_PLIST"
    ok "Wrote $LAUNCHD_PLIST"

    # Idempotent reload: bootout if already loaded, then bootstrap.
    # Fall back to legacy load/unload on older macOS where bootout doesn't
    # know our domain yet.
    if launchctl print "gui/$(id -u)/$LAUNCHD_LABEL" >/dev/null 2>&1; then
        launchctl bootout "gui/$(id -u)/$LAUNCHD_LABEL" 2>/dev/null \
            || launchctl unload "$LAUNCHD_PLIST" 2>/dev/null || true
    fi
    if launchctl bootstrap "gui/$(id -u)" "$LAUNCHD_PLIST" 2>/dev/null \
        || launchctl load "$LAUNCHD_PLIST" 2>/dev/null; then
        ok "launchd agent loaded ($LAUNCHD_LABEL)"
        DRIFT_INSTALLED=1
    else
        warn "launchctl load failed — drift detector won't run automatically."
        warn "You can still run ./bin/stellaris-fix-doctor manually after game updates."
    fi
fi

# ── Done ──────────────────────────────────────────────────────────────────

step "Installation complete"
say ""
say "  ${GREEN}${BOLD}→${RESET} ${BOLD}Launch Stellaris via Steam${RESET}"
say "  ${GREEN}${BOLD}→${RESET} ${BOLD}In the Paradox launcher, select ${GREEN}\"Increased Stack Size\"${RESET}"
say "       from the launch options, then click Play."
if [ "$COMPANION_INSTALLED" = "1" ]; then
    say "  ${GREEN}${BOLD}→${RESET} Under ${BOLD}Mods → Browse${RESET}, find ${GREEN}\"Stellaris Mac CTD Fixes (companion)\"${RESET}"
    say "       and enable it. ${YELLOW}This breaks Steam achievements${RESET} like all other"
    say "       Stellaris script-level fix mods — disable for ironman runs."
fi
say ""
say ""
say "${DIM}Health check anytime:${RESET}  ${BOLD}./bin/stellaris-fix-doctor${RESET}"
if [ "$DRIFT_INSTALLED" = "1" ]; then
    say "${DIM}Drift detector is active — you'll get a Notification Center alert when${RESET}"
    say "${DIM}Stellaris updates and the dylib needs rebuilding.${RESET}"
fi
say ""
say "${DIM}To uninstall: run ${RESET}${BOLD}./uninstall.sh${RESET}${DIM} or double-click Uninstall.command${RESET}"
say "${DIM}For verbose diagnostic logging, set Steam launch options to:${RESET}"
say "${DIM}   STELLARIS_FIX_DEBUG=1 %command%${RESET}"
say ""
