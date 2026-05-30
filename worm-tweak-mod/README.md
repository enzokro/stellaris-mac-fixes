# Worm Tweak

**Tune the spawn chance of the Horizon Signal (Worm-in-Waiting) event chain in Stellaris.**

In vanilla Stellaris, the Horizon Signal chain is gated behind a single 20% roll at game start, and even when the roll succeeds only **one** randomly chosen black hole in the galaxy can actually trigger the chain. Most games never see the Worm. This mod replaces that logic with a one-time configuration popup on game day 1 and, by default, flags **every** qualifying black hole so the chain is guaranteed to be reachable.

## What it does

At game start, a popup offers four choices:

| Option | Behavior |
| --- | --- |
| **Guarantee** *(default)* | Every qualifying black hole in the galaxy is flagged. Guaranteed spawn. |
| **High — 75%** | 75% chance the galaxy gets the Worm; if so, every qualifying black hole is flagged. |
| **Vanilla — 20%-equivalent** | One random black hole is flagged for certain (no 20% gating roll). Closest approximation to untouched vanilla. |
| **Disable** | No black holes are flagged. The chain will not appear. |

"Qualifying" uses the vanilla filter: black-hole star class, not a Fallen Empire cluster, no starbase, no `guardian` flag, not inside an `empire_cluster` start system.

Choice applies galaxy-wide and cannot be changed after day 1.

## Install (local)

```
./install-mod.sh
```

This writes a `.mod` pointer file to `~/Documents/Paradox Interactive/Stellaris/mod/worm-tweak-mod.mod` whose `path=` points back to this directory. The Paradox launcher will pick up **Worm Tweak** under Mods → Browse. Enable it and start a new game.

To remove:

```
./uninstall-mod.sh
```

## How it works

- `descriptor.mod` uses `replace_path="events/horizonsignal_events.txt"` so our copy of the vanilla file fully replaces the original.
- In the replaced file, only the `akx.8000` event's `immediate` block is changed — to a no-op. Everything else (`akx.9000`, the ship/planet events, the Worm-in-Waiting storyline) is identical to vanilla.
- `common/on_actions/zz_worm_tweak.txt` additively extends `on_game_start_country` with `worm_tweak.0` — a trampoline that schedules the popup for day 1.
- `events/worm_tweak_events.txt` defines the trampoline, the popup (`worm_tweak.1`), and the applier (`worm_tweak.2`) that sets the star flags based on the player's choice.

## Compatibility

- Requires Stellaris 4.3.x.
- Incompatible with any other mod that also uses `replace_path="events/horizonsignal_events.txt"` (e.g., some "Guaranteed Horizon Signal" mods). If you load both, whichever is last in the mod order wins.
- Safe to enable/disable mid-campaign in the sense that the game won't crash, but disabling it does **not** remove existing `horizonsignal_spawn` star flags from a save, and the configuration popup only fires at game start.

## Workshop upload

Before uploading to Steam Workshop:

1. Replace `thumbnail.png` with a real 512×512 PNG (the placeholder is a 1×1 black square).
2. The Paradox launcher handles the upload; it fills in `remote_file_id=` on first publish.
