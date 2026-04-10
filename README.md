# stellaris-mac-fixes

**Fixes crash-to-desktop bugs in Stellaris on macOS.**

Stellaris on macOS (Apple Silicon and Intel) crashes repeatedly during mid-to-late-game sessions. This package fixes three distinct crash families without modifying the game binary — it injects a small support library when the game launches.

Tested on macOS 26.4 with Stellaris v4.3.3 (Cetus) under Rosetta 2 on Apple Silicon, but should work on any recent macOS and Stellaris version.

---

## Quick Install

1. Download the latest release zip
2. **[Verify the download](#verifying-your-download)** (recommended — see below)
3. Extract it anywhere (e.g., your Downloads folder)
4. Double-click **Install.command**
5. Launch Stellaris via Steam
6. In the Paradox launcher, choose **"Increased Stack Size"** from the launch options
7. Play

To uninstall, double-click **Uninstall.command**.

> If macOS warns that the `.command` file is from an unidentified developer, right-click (or Ctrl-click) the file, choose **Open**, then click **Open** in the dialog. You only need to do this once.

---

## Verifying your download

This package contains a **dynamic library that gets injected into Stellaris** and **shell scripts that run on your machine**. You should verify the download before running anything from it. Don't take our word for it — check the hashes yourself.

There are three layers of verification, in order of importance:

### 1. Verify the release zip (most important)

Before extracting, compare the zip's hash to the values published on the release page (`RELEASE_CHECKSUMS.txt` or the release notes). Open Terminal in the folder where you downloaded the zip and run:

```bash
md5 -q stellaris-mac-fixes.zip
shasum -a 256 stellaris-mac-fixes.zip
```

The output should match exactly what's published. If it doesn't match, **stop and re-download** — the file is corrupted or has been tampered with somewhere along the way.

> The release page is the only authoritative source for the zip hash. Do not trust hashes copied into a forum post, a mirror, or anywhere else not under the release author's control.

### 2. Verify individual files (after extracting)

The extracted folder contains a `CHECKSUMS.txt` file listing the MD5 and SHA-256 of every bundled file. Compare each file's hash by running, for example:

```bash
cd stellaris-mac-fixes
md5 -q libstellaris_fix.dylib
md5 -q install.sh
shasum -a 256 libstellaris_fix.dylib
```

You can also `cat CHECKSUMS.txt` to see the full manifest. If any file's hash doesn't match, do not run the installer.

### 3. Automatic check at install time

The installer runs an MD5 check on `libstellaris_fix.dylib` before copying it into your Stellaris directory. If the dylib has been modified or is corrupted, the installer aborts with a clear error message.

This is a convenience layer — it catches accidental corruption and prevents installing the wrong dylib alongside an old installer. It is **not** a substitute for verifying the zip hash yourself, since a tampered installer could have a tampered expected-hash baked in.

### Inspecting the scripts before running

Unlike the dylib (which is binary), all the shell scripts in this package are plain text. You're encouraged to read them before running:

```bash
less Install.command   # double-click wrapper
less install.sh        # actual installer
less uninstall.sh      # uninstaller
```

The whole installer is around 200 lines and documented inline.

---

## What it fixes

The short version: Stellaris is a Windows game ported to macOS, and it trips over several macOS platform defaults that are more conservative than Windows. It also has some use-after-free bugs that only manifest on macOS because of timing differences.

### Fix 1: Thread stack overflow

**Symptom:** Random crashes during mid-to-late game, often with corrupted stack traces in `CPdxTaskScheduler::RunTasks` or other worker thread functions.

**Cause:** macOS gives each new pthread a tiny **512 KiB stack** by default. Windows gives 1 MiB. Stellaris' task scheduler threads run deep recursive AI/event code that overflows 512 KiB but fits comfortably in 1 MiB+.

**Fix:** This package intercepts `pthread_create` / `pthread_attr_init` / `pthread_attr_setstacksize` and transparently gives every new thread an **8 MiB stack**. Only virtual address space is reserved — physical memory is still allocated lazily, so this costs almost nothing in practice.

### Fix 2: File descriptor limit

**Symptom:** Subtle failures loading many assets or mods at once.

**Cause:** macOS defaults to **256 open files** per process. Windows allows thousands.

**Fix:** At startup, the library raises the file descriptor soft limit to **8192**.

### Fix 3: Use-after-free crashes (NULL-page vtable calls)

**Symptom:** Crashes that look like this in the stack trace:

```
???                         0x0000000000000000 0x0 + 0
stellaris                   CBypassGalacticMapIconBox::Update() + ...
```

or

```
???                         0x0000000000000001 0x0 + 1
stellaris                   CPersistentName::SVariable::WriteMembers() + ...
```

**Cause:** Game objects are freed on one thread while another thread still holds a pointer. When the game later makes a virtual call through that pointer, the vtable entry has been partially zeroed — so the CPU jumps to address 0x0, 0x1, or similar and segfaults. These are bugs in Stellaris itself, but they disproportionately affect macOS due to timing differences under Rosetta 2.

**Fix:** The library installs a SIGSEGV recovery handler. When the CPU attempts to execute an address inside the NULL page (0x0 – 0xFFF) and the return address points back into the Stellaris binary, we simulate a `ret` instruction with a zero return value. The game's existing error handling then takes over and the game keeps running.

The handler is rate-limited (max 32 recoveries per 5 seconds) to prevent infinite loops if a recovered call immediately fails again. Other crashes fall through to the game's normal crash reporter.

---

## Installation details

The installer does three things:

1. Copies `libstellaris_fix.dylib` into your Stellaris directory, alongside the other game libraries
2. Creates a small wrapper script at `stellaris.app/Contents/MacOS/stellaris_wrapper.sh` that sets `DYLD_INSERT_LIBRARIES` to load the library and then execs the real game binary
3. Adds an **"Increased Stack Size"** alternative launch option to the Paradox launcher by patching `launcher-settings.json` (your original is backed up as `launcher-settings.json.bak`)

### Where things get installed

Assuming the default Steam location:

```
~/Library/Application Support/Steam/steamapps/common/Stellaris/
├── libstellaris_fix.dylib                            ← installed
├── launcher-settings.json                            ← patched
├── launcher-settings.json.bak                        ← backup of original
└── stellaris.app/Contents/MacOS/
    ├── stellaris                                     ← unchanged
    └── stellaris_wrapper.sh                          ← installed
```

### Custom Stellaris directory

If Stellaris is not at the default location, set `STELLARIS_DIR` before running the installer:

```bash
STELLARIS_DIR="/path/to/Stellaris" ./install.sh
```

### Steam updates

Steam occasionally updates Stellaris, which overwrites `launcher-settings.json`. Just double-click **Install.command** again — the installer is idempotent and will reapply the patch. The dylib and wrapper script survive Steam updates.

---

## Verifying the fix is active

With the fix installed and Stellaris launched via the "Increased Stack Size" option, open Terminal and run:

```bash
ps -ef | grep stellaris
```

You should see the game running with `stellaris_wrapper.sh` somewhere in its ancestry.

For verbose diagnostic output, set the environment variable `STELLARIS_FIX_DEBUG=1` before launching. You can do this in Steam → Stellaris → Properties → Launch Options:

```
STELLARIS_FIX_DEBUG=1 %command%
```

Then check the terminal output (or run Stellaris from Terminal) — you'll see messages like:

```
[stellaris-fix] v1.2.0 loaded
[stellaris-fix]   target stack: 8 MiB, floor: 1 MiB
[stellaris-fix]   fd soft limit raised to 8192
[stellaris-fix]   SIGSEGV recovery handler installed
[stellaris-fix] attr_init: default stack → 8 MiB
[stellaris-fix] pthread_create: NULL attrs → 8 MiB stack
...
```

If the recovery handler catches a crash, you'll see:

```
[stellaris-fix] recovered: NULL-page call (execution at invalid address)
```

---

## Uninstalling

Double-click **Uninstall.command**, or run:

```bash
./uninstall.sh
```

This restores `launcher-settings.json` from backup and removes the dylib and wrapper script. Your game returns to its exact original state.

---

## Compatibility

- **macOS**: 11 (Big Sur) or later, tested on 26.4
- **Stellaris**: v4.3.3 (Cetus), likely works on earlier and later versions — the fixes target stable POSIX APIs
- **Architecture**: Universal binary (arm64 + x86_64). Works on Intel Macs natively and Apple Silicon under Rosetta 2.
- **System Integrity Protection**: Fully compatible — the game binary is unsigned so `DYLD_INSERT_LIBRARIES` works without needing SIP disabled.
- **Steam**: Yes. Other stores (Epic, GOG, Paradox Store) should work if the layout matches; use `STELLARIS_DIR`.
- **Mods**: No interaction with mods — the fix operates at the OS level.

---

## Troubleshooting

### "Permission denied" when double-clicking Install.command

macOS requires the file to be marked executable. Run this in Terminal from the extracted folder:

```bash
chmod +x Install.command Uninstall.command install.sh uninstall.sh
```

Then try again.

### "Unidentified developer" warning

macOS Gatekeeper blocks unsigned shell scripts from running. Right-click (or Ctrl-click) **Install.command**, choose **Open**, then click **Open** in the confirmation dialog. You only need to do this once per file.

### The "Increased Stack Size" option doesn't appear in the Paradox launcher

This means `launcher-settings.json` wasn't patched. Check that the installer ran successfully. If you modified the file manually afterward, re-run the installer. You can always revert to the original by restoring `launcher-settings.json.bak`.

### The game still crashes

- Make sure you selected **"Increased Stack Size"** in the Paradox launcher, not the default option.
- Run with `STELLARIS_FIX_DEBUG=1` (see above) and check whether the fix is loading.
- Some crashes may be from bugs this package doesn't cover yet. Open an issue with your crash log from `~/Documents/Paradox Interactive/Stellaris/crashes/` — especially `exception.txt`.

### I want to play without the fix for a run

Launch Stellaris via Steam as usual, and in the Paradox launcher, select the default launch option instead of "Increased Stack Size". The default is unmodified.

---

## Building from source

You need Xcode command-line tools (`xcode-select --install`).

```bash
make           # builds libstellaris_fix.dylib (universal: arm64 + x86_64)
make test      # runs the test suite with the dylib injected
make install   # calls install.sh
make uninstall # calls uninstall.sh
make clean     # removes build artifacts
```

The entire implementation is in a single C file: [`stellaris_fix.c`](stellaris_fix.c). The test harness is [`test_interpose.c`](test_interpose.c).

---

## How it works, briefly

The library uses two macOS-specific mechanisms:

1. **DYLD interposition** via the `__DATA,__interpose` Mach-O section to intercept `pthread_create`, `pthread_attr_init`, `pthread_attr_setstacksize`, and `sigaction`. The dynamic linker replaces references to these symbols in the game and all other loaded images with our versions. Calls within our own library still go to the real functions, so there's no recursion.

2. **Signal-handler recovery** for crashes that can't be intercepted via DYLD (because they happen through direct intra-binary calls). A SIGSEGV handler detects the "NULL-page execution" crash signature and unwinds by simulating a `ret` instruction in the signal context.

The library is loaded via `DYLD_INSERT_LIBRARIES` — this requires the target binary to be either unsigned or have the `com.apple.security.cs.disable-library-validation` entitlement. The Stellaris macOS build is unsigned, so injection works without needing SIP to be disabled.

For a deeper technical writeup, see the header comments in [`stellaris_fix.c`](stellaris_fix.c).

---

## Credits

This project builds on [planetbeing/increase-default-macos-thread-stack-size](https://github.com/planetbeing/increase-default-macos-thread-stack-size), which introduced the `pthread_create` interposition approach and provided the initial stack-size fix. The scope here is broader — covering thread attributes more completely, raising file descriptor limits, and adding signal-based recovery for unrelated use-after-free crashes that were previously masked by the stack overflow.

---

## License

MIT. See [LICENSE](LICENSE).

---

## Disclaimer

This is an unofficial community fix. It is not affiliated with or endorsed by Paradox Interactive. Use at your own risk. That said, the library is small, open source, and makes no modifications to the game binary or save files — uninstalling restores everything to exactly how it was.
