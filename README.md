# stellaris-mac-fixes

**Fixing crash-to-desktop bugs in Stellaris on Mac.**

Large maps in Stellaris on macOS often crash around mid-game. This package fixes three crash groups without changing the game binary. When the game launches, a small support library is injected.

Build environment:
- macOS 26.4
- Stellaris v4.3.7 (Cetus) via Rosetta 2 (works on adjacent versions; see Compatibility in [DOCS.md](DOCS.md))

More details, including how to verify the install, in the [longer docs](DOCS.md). 

---

## Quick Install

1. Download the latest release zip
2. Extract it anywhere (e.g., your Downloads folder)
3. Double-click the **Install.command**
4. Launch Stellaris via Steam
5. In the Paradox launcher, choose **"Increased Stack Size"** from the launch options
6. Play

To uninstall, double-click **Uninstall.command**.

> If macOS warns that the `.command` file is from an unidentified developer, right-click (or Ctrl-click) the file, choose **Open**, then click **Open** in the dialog. You should only have to do this once.


## What it fixes

Four layers, installed by the same `install.sh`:

- **Layer 1 — the dylib** (`libstellaris_fix.dylib`). Binary-level crash recovery covering thread stack overflow, file descriptor exhaustion, bad-RIP use-after-free (vtable corruption — recovery via direct return or stack scan), and stale/NULL-pointer data-derefs across three instruction shapes: MOV-loads (zero destination register and skip), CMP-immediate against memory (emulate RFLAGS for the comparison and skip), and indirect CALL through memory (no-op the virtual method and skip). Faults are delivered through the Mach exception port handler (primary path, kernel-level before any signal is generated) with a sigaction handler as defense-in-depth fallback — both share the same `sfix_attempt_recovery` helper. Loads via `DYLD_INSERT_LIBRARIES` through the Paradox launcher's "Increased Stack Size" entry. `__TEXT` segment bounds and all loaded-image text ranges are resolved at constructor time by walking dyld, so the recovery surface auto-tracks across game updates without per-version rebuilds. Emits one parseable `SUMMARY` line per fault with a stable FNV-1a crash signature hash.
- **Layer 2 — the companion mod** (`companion-mod/`). Script-level CTD prevention: 13 focused defensive fixes mined from `~~Stellaris [v4.3] General Fixes` (workshop ID 3701747681). Stops the script-level bug conditions producing many of the crashes the dylib would otherwise have to recover from. **Disables Steam achievements** like every other Stellaris script fix mod — disable it in the launcher for ironman runs; the dylib still loads independently. See [companion-mod/README.md](companion-mod/README.md) for details.
- **Layer 3 — the doctor** (`bin/stellaris-fix-doctor`). Read-only health check that reads the dylib log + crash bundles + live game binary and prints what's installed, what's drifted, what crashed, and what's been recovered. Run it after a play session to see how things went. `--json` for tooling.
- **Layer 4 — the drift detector** (launchd agent `com.stellaris-fix.drift`). Watches the Stellaris game binary; when Steam updates the game and the binary's text segment shifts, posts a macOS notification telling you to rebuild and reinstall the dylib. Silent the rest of the time.
- **Layer 5 — the save-doctor** (`bin/stellaris-fix-save-doctor`). Offline inspection AND repair of a Stellaris .sav file. Detects zero-ethic empires (the `rebellionfix.34` case) and dangling country event_targets, classified by Stellaris's slot/version encoding so benign destroyed-scope markers are separated from genuinely-orphaned refs. With `--repair`, writes `<input>.repaired.sav` with zero-ethic empires fixed (a default ethic injected into the empty `ethos={}` block); original `.sav` is never modified. Use it before loading a problem save: `./bin/stellaris-fix-save-doctor [--repair] "$HOME/Documents/Paradox Interactive/Stellaris/save games/.../save.sav"`.

The short version of the binary side: Stellaris is a Windows game ported to macOS, and it trips over several macOS platform defaults that are more conservative than Windows. It also has some use-after-free bugs that likely only manifest on Mac because of timing differences.

### Fix 1: Thread stack overflow

**Symptom:** Random crashes during mid-to-late game, often with corrupted stack traces in `CPdxTaskScheduler::RunTasks` or other worker thread functions.

**Cause:** macOS gives each new pthread a tiny **512 KiB stack** by default. Windows gives 1 MiB. Stellaris' task scheduler threads run deep recursive AI/event code that overflows 512 KiB but fits comfortably in 1 MiB+.

**Fix:** This package intercepts `pthread_create` / `pthread_attr_init` / `pthread_attr_setstacksize` and transparently gives every new thread an **8 MiB stack**. Only virtual address space is reserved — physical memory is still allocated lazily, so this costs almost nothing in practice.

### Fix 2: File descriptor limit

**Symptom:** Subtle failures loading many assets or mods at once.

**Cause:** macOS defaults to **256 open files** per process. Windows allows thousands.

**Fix:** At startup, the library raises the file descriptor soft limit to **8192**.

### Fix 3: Use-after-free crashes (corrupted-vtable calls)

**Symptom:** Crashes with `???` in the stack trace, sometimes followed directly by a Stellaris frame:
```
???                         0x0000000000000000 0x0 + 0
stellaris                   CBypassGalacticMapIconBox::Update() + ...
```

…and sometimes routed through Apple's GL/Metal driver before the Stellaris frame:
```
???                         0x0000000000000000 0x0 + 0
AppleMetalOpenGLRenderer    GLDContextRec::setRenderVertexBuffers + ...
GLEngine                    glDrawArrays_ACC_Exec + ...
stellaris                   CBordersGraphics::Render + ...
```

The address after `???` is whatever garbage filled the freed slot — `0x0` and `0x1` are most common, but larger values like `0x300000000` happen too.

**Cause:** Game objects are freed on one thread while another thread (or Apple's driver) still holds a pointer. When the game later makes a virtual call through that pointer, the vtable slot contains whatever the freed memory got reused for — so the CPU jumps to a non-executable address and segfaults. These are bugs in Stellaris (or in Apple's driver tracking stale handles), but they disproportionately affect macOS due to timing differences under Rosetta 2.

**Fix:** The library installs a SIGSEGV recovery handler that catches instruction-fetch faults (where the kernel reports the faulting address equals the current `RIP`). It recovers via one of two paths:

- **Direct return** when the corrupted call came from Stellaris itself: simulate a `ret` with a zero return value. The game's existing error handling takes over.
- **Stack scan** when the corrupted call ended up inside an Apple driver call (e.g. during border rendering): walk the stack to find the Stellaris frame that initiated the chain and unwind to that frame, cancelling the foreign call entirely. At most one render frame is affected; the next retries.

Rate-limited (max 32 recoveries per 5 seconds) to prevent infinite loops if recovery causes immediate re-failure. Crashes that don't match this signature fall through to the game's normal crash reporter.

### Fix 4: Stale-pointer data-deref crashes

**Symptom:** Crashes deep in Stellaris frames (or framework frames called from Stellaris) where the OS reports a faulting address that looks like a real heap pointer (e.g. `si_addr=0x7fb7078c9410`) rather than NULL. Usually preceded by a flood of `Script Error: Invalid context switch [assembling_species]` / `[owner]` / `[contact_country]` entries in `error.log` with `opener_id=4294967295` — the upstream signal of orphaned game-object references.

**Cause:** The script side leaks a reference to a freed game object (planet, country, species); the C++ code later dereferences the stale pointer. Three instruction shapes dominate in real crashes: loading a field through the stale pointer (`mov rax, [rdi]`), checking a flag on the stale object (`cmp byte [rdi+0x30], 0`), and dispatching a virtual method through the stale vtable (`call [rax+0x68]`). When the load form recovers by zeroing a register, the next instruction can cascade-fault on the now-NULL pointer — the dylib handles the cascade in-line.

**Fix:** The library decodes the faulting instruction and applies a shape-specific recovery:
- **MOV-load** (`mov reg, [base+disp]`): zero the destination register, advance RIP past the load. The downstream code sees a NULL value where it expected a real object — a defensive null-check takes the safe branch; a missing null-check cascades into one of the other shapes and recovers there.
- **CMP against memory** (`cmp [base+disp], imm`): emulate the RFLAGS state of `(0 - imm)` per the standard SUB rules (CF/PF/AF/ZF/SF/OF), preserve the other RFLAGS bits, advance RIP. The downstream conditional branch takes the "field was zero" path.
- **Indirect CALL through memory** (`call [base+disp]`): skip the call, set RAX=RDX=0 to simulate the virtual method returning `(0, 0)` per the SysV AMD64 ABI. The dispatching loop continues past the dead element.

Same rate-limit (32 recoveries / 5 s) as Fix 3. Kill switch: set `STELLARIS_FIX_NO_STALE_PTR=1` in Steam launch options to disable the high-`si_addr` recovery branch and fall back to NULL-only recovery (v1.8.1 behavior) if a regression appears. Each recovery emits a distinct `SUMMARY` tag (`data-recovered`, `data-recovered-cmp`, `data-recovered-call`) so the doctor can group recoveries by class.

The companion mod (Layer 2) addresses the *upstream* condition by guarding the script sites that produce the orphan references in the first place — the dylib catches the binary-level fault either way, but with the companion mod enabled the fault stops happening on save-load-correlated patterns.

