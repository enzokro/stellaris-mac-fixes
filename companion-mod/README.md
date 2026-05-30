# Stellaris Mac CTD Fixes — Companion Mod

**Script-level crash prevention. Installed automatically alongside the [stellaris-mac-fixes](../README.md) dylib.**

## What this mod does

Twelve script-level fixes that prevent the Stellaris bug conditions producing many of the crashes the dylib otherwise has to recover from. The dylib catches the binary-level fault; this mod stops the script from creating the dangling pointer in the first place.

| # | Fix | Hook |
|---|---|---|
| 1 | AI hive→non-hive planet-transfer null guard (`smf.1`) | `on_planet_transfer` |
| 2 | Dead-war detector + cleanup (`smf.50/51/52/53`) | `on_yearly_pulse`, `on_planet_occupied`, save load |
| 3 | AI Shroud engagement loop (`smf.60`) | `on_yearly_pulse` |
| 4 | Save-load state repair — Shroud DLC flag, prethoryn / pet_queen target re-anchor, ascension trait sync (`smf.10`) | `on_single_player_save_game_load` |
| 5 | No-ethic vassal reconstruction from overlord (`smf.20/21`) | `on_single_player_save_game_load` |
| 6 | Cybernetic Creed leader trait sync (`smf.30/301`) | `on_single_player_save_game_load` |
| 7 | Cyberization situation deadlock recovery (`smf.31/311`) | `on_single_player_save_game_load` |
| 8 | Titanic army cap defense + counter resync (`smf.40-48`) | `on_army_recruited`, `on_army_killed_*`, `on_army_disbanded`, `on_ship_disbanded`, `on_planet_transfer`, `on_colonized`, `on_colony_destroyed`, `on_bi_yearly_pulse`, save load |
| 9 | Nanite swarm anomaly re-enable on starbase transfer (`anomaly.16001/16002` overrides) | `on_starbase_transfer` |
| 10 | Crisis sentinel reinforcement gate (`crisis.233` override) | (vanilla `on_five_year_pulse`) |
| 11 | Crisis portal-destroyed null fallback (`crisis.1015/1611` overrides) | (vanilla `on_fleet_destroyed_victim`) |
| 12 | Crisis anchor-destroyed `portal_holder_1` null fallback (`crisis.1284` override) | (vanilla `on_starbase_destroyed`) |
| 13 | Marauder khan-succession scope guard (`marauder.609` override) | (vanilla `on_country_destroyed`) |

Every state-mutating fix is guarded — they only act when the broken state is detected. Healthy saves are no-ops.

## Source

Each event file in this mod cites its source line range from `~~Stellaris [v4.3] General Fixes` (workshop ID 3701747681, by vladimir / FirePrince / Caligula Caesar / Ariphaos / Corsairmarks). This mod is a focused, defensive subset — it explicitly drops the source mod's silent vanilla overrides (50+ files under `common/buildings/`, `common/pop_jobs/`, etc. that smuggle balance changes alongside fixes).

## Achievements

**This mod disables Steam achievements**, like every other Stellaris script-level fix mod. Stellaris computes a checksum over `events/`, `common/`, and a few other folders at game start; any mod adding files under those paths breaks the checksum. There is no opt-in flag.

For ironman / achievement runs, **disable this mod** in the Paradox launcher's Mods list. The dylib still loads independently and provides binary-level crash recovery — you just lose the script-level prevention layer.

## Install

`cd ..; ./install.sh` from the parent stellaris-mac-fixes directory. The installer registers a launcher pointer file at `~/Documents/Paradox Interactive/Stellaris/mod/stellaris-mac-fixes-companion.mod` that points back at this directory. Open the Paradox launcher, go to **Mods → Browse**, find **Stellaris Mac CTD Fixes (companion)**, and enable it.

`./uninstall.sh` removes the pointer file (the mod content stays in this repo).

## Compatibility

- **Stellaris 4.3.x** (`supported_version="v4.3.*"` in descriptor.mod).
- **Hard conflict** with the upstream `~~Stellaris [v4.3] General Fixes` mod itself — both define `crisis.233`, `crisis.1015`, `crisis.1284`, `crisis.1611`, `marauder.609`, `anomaly.16001`, `anomaly.16002`. Whichever loads later wins; running both is unsupported. Pick one.
- **Soft conflict** with any mod that also overrides those same vanilla event IDs.
- Compatible with all the user's other installed mods (UI Overhaul Dynamic, Beautiful Universe, Gigastructural Engineering, Real Space, Planetary Diversity, More Events Mod, etc.) which don't overlap on these IDs.
- Save-game safe. Disabling the mod doesn't crash existing saves; the dylib continues to provide binary-level recovery.

## File layout

```
companion-mod/
├── descriptor.mod                    — launcher metadata
├── README.md                         — this file
├── thumbnail.png                     — 1×1 placeholder
├── common/
│   └── on_actions/
│       └── zz_smf_companion.txt      — additive on_action hooks (zz_ sorts late)
└── events/
    ├── smf_planet_transfer.txt       — fix #1
    ├── smf_dead_war.txt              — fixes #2, #3
    ├── smf_save_repair.txt           — fixes #4, #5
    ├── smf_dlc_cyber.txt             — fixes #6, #7
    ├── smf_army_counter.txt          — fix #8
    ├── smf_dlc_anomaly.txt           — fix #9
    └── !smf_overrides.txt            — fixes #10-13 (vanilla event-ID overrides; ! prefix forces late load)
```
