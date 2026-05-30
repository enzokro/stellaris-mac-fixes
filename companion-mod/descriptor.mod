# Companion mod for the stellaris-mac-fixes dylib.
# Provides script-level CTD prevention to complement the binary-level
# crash recovery in libstellaris_fix.dylib.
#
# NOT achievement-compatible (modifies events/ + common/, which Stellaris
# includes in its checksum). Disable this mod for ironman runs — the dylib
# still loads independently and provides binary-level recovery.
#
# Conflicts with: ~~Stellaris [v4.3] General Fixes (overlapping event-ID
# overrides on crisis.233/1015/1284/1611, marauder.609, etc.) — do not
# enable both at the same time.
name="Stellaris Mac CTD Fixes (companion)"
version="0.1.0"
tags={
	"Fixes"
}
supported_version="v4.3.*"
picture="thumbnail.png"
