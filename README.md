# stellaris-mac-fixes

**Fixing crash-to-desktop bugs in Stellaris on Mac.**

Large maps in Stellaris on macOS often crash around mid-game. This package fixes three crash groups without changing the game binary. When the game launches, a small support library is injected.

Build environment:
- macOS 26.4
- Stellaris v4.3.3 (Cetus) via Rosetta 2

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

The short version: Stellaris is a Windows game ported to macOS, and it trips over several macOS platform defaults that are more conservative than Windows. It also has some use-after-free bugs that likely only manifest on Mac because of timing differences.

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

or:
```
???                         0x0000000000000001 0x0 + 1
stellaris                   CPersistentName::SVariable::WriteMembers() + ...
```

**Cause:** Game objects are freed on one thread while another thread still holds a pointer. When the game later makes a virtual call through that pointer, the vtable entry has been partially zeroed — so the CPU jumps to address 0x0, 0x1, or similar and segfaults. These are bugs in Stellaris itself, but they disproportionately affect macOS due to timing differences under Rosetta 2.

**Fix:** The library installs a SIGSEGV recovery handler. When the CPU attempts to execute an address inside the NULL page (0x0 – 0xFFF) and the return address points back into the Stellaris binary, we simulate a `ret` instruction with a zero return value. The game's existing error handling then takes over and the game keeps running.

The handler is rate-limited (max 32 recoveries per 5 seconds) to prevent infinite loops if a recovered call immediately fails again. Other crashes fall through to the game's normal crash reporter.

