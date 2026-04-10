# Changelog

## v0.1 — Initial release

First release of `stellaris-mac-fixes`. Fixes three crash groups in Stellaris on macOS.

### Fixes

- **Thread stack overflow**: macOS pthreads have a default 512 KiB stack (vs 1 MiB on Windows). Stellaris' task scheduler threads often overflow during mid to end game, corrupting return addresses. This fix intercepts `pthread_create` / `pthread_attr_init` / `pthread_attr_setstacksize` to give every new thread an 8 MiB stack. Note: only virtual addresses are reserved, physical memory is still allocated lazily.
- **File descriptor size**: macOS defaults to 256 open files per process. This fix raises the soft limit to 8192 at startup.
- **Use-after-free crashes (NULL-page vtable calls)**: Stellaris has bugs where freed game objects are still referenced from another thread. The corrupted vtable entries point into the NULL page so the CPU jumps there and faults. This fix installs a SIGSEGV recovery handler that detects this specific signature, simulates a `ret` with a zero return value, and lets the game continue. Rate-limited to prevent infinite loops; non-matching crashes fall through to the game's normal crash reporter.

### Installation and running

1. Download `stellaris-mac-fixes.zip`
2. **Verify** the download (see below)
3. Extract anywhere
4. Double-click `Install.command`
5. Launch Stellaris via Steam
6. Go to Settings, scroll down and choose **"Increased Stack Size"** in the Paradox launcher

To uninstall, double-click `Uninstall.command`.

### Verify the download

Check the download with:
```
md5 -q stellaris-mac-fixes.zip
shasum -a 256 stellaris-mac-fixes.zip
```

Expected values:
```
MD5:    8c954a2b056efe67e9edefa3b54e14a2
SHA256: 1a5ed0ef488d79e4039ff12a8337bb332e7df2b2993bd5389729181652fc8092
Size:   28309 bytes
```

After extracting, `CHECKSUMS.txt` lists hashes for every bundled file. The installer also auto-verifies `libstellaris_fix.dylib` against an embedded MD5 (`5fb5604a1b4a3df071931bea7d872e67`) before copying it into your Stellaris directory.

### Compatibility

- macOS 11 (Big Sur) or later (tested on 26.4)
- Stellaris v4.3.3 (Cetus) (should work on adjacent versions)
- Apple Silicon and Intel (dylib for arm64 + x86_64)
- Compatible with SIP: the game binary is unsigned, so `DYLD_INSERT_LIBRARIES` injection doesn't disable system protections
- Does not touch mods or save files (uninstall fully reverts)

### Credits

Builds on the great work of [planetbeing/increase-default-macos-thread-stack-size](https://github.com/planetbeing/increase-default-macos-thread-stack-size), which introduced the `pthread_create` interposition approach. This release rounds out the thread attribute handling, raises the file descriptor limit, and fixes a group of systemic use-after-free crashes that were only revealed when the stack-size fix landed.
