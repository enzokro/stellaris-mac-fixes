# Changelog

## v1.10.3 — indirect-call recovery for vtable-dispatch cascades

CTD captured 2026-05-29 22:38 exposed a recovery cascade we hadn't yet handled:

```
0x102714e1b:  48 8b 07               mov rax, [rdi]            ← fault #1: rdi is stale, recovered (zeroed rax)
0x102714e1e:  ff 50 68               call qword ptr [rax+0x68] ← fault #2: rax=0, si_addr=0x68 — op=ff UNRECOVERED
0x102714e21:  48 ff c3               inc rbx
0x102714e24:  49 63 86 64 02 00 00   movsxd rax, [r14+0x264]
0x102714e2b:  48 39 c3               cmp rbx, rax
0x102714e2e:  7c e0                  jl 0x102714e10            ← loop
```

This is a textbook C++ virtual-method dispatch loop over an array of object pointers: `for each obj in r14->array: obj->vtable[0x68]()`. On iteration N, the array element was a stale pointer. The v1.10.0 load recovery correctly zeroed RAX (the vtable pointer), but the very next instruction tried to dispatch through vtable slot 0x68 on NULL. The source comment had anticipated cascades — "missing null-checks fall through and may crash again, which path-a/path-b/path-data-recovered handles" — but path-a/b are *bad-RIP* recovery (jumped to bad address), while here the CALL hadn't jumped yet: the operand-fetch faulted first, classified as data-segv, decoder rejected.

**Fix:** add `sfix_decode_call_indirect_mem` accepting opcode `0xff` with `/2` subop = `CALL r/m64` (near, absolute indirect). Same addressing-mode rules as load/cmp decoders: `mod ∈ {0,1,2}`, SIB rejected, RIP-relative rejected, register-direct rejected. REX prefix optional and ignored (operand size is already 64 for indirect calls in 64-bit mode); `0x66` rejected.

Recovery: skip past the instruction (1–7 bytes), set `RAX = RDX = 0` to simulate the virtual function returning `(0, 0)` per the SysV AMD64 ABI integer/pointer return slots. The dispatching loop continues past the dead element.

**Why not also `FF /4` (JMP near indirect):** indirect jumps are used for tail-calls and jump tables. Skipping a tail-call has the same shape as skipping a vtable dispatch (safe), but a wrong-target jump table miss could land in arbitrary code. Deferred until telemetry shows it's worth the risk.

**Why not `FF /0` (INC) / `FF /1` (DEC) / `FF /6` (PUSH):** INC/DEC are read-modify-write — silently dropping the store risks state divergence (same reason `80/83 /0–/6` are deferred). PUSH is read-only but rare in object access patterns.

**Diagnostic upgrade.** Successful call recoveries log `path=data-recovered-call` with `instr_len`. The doctor can now distinguish three recovery classes: `data-recovered` (load zero+skip), `data-recovered-cmp` (flag emulate), `data-recovered-call` (vtable no-op).

**Test coverage.** `test_decoder.c` gains 10 new cases for the indirect-call decoder: tonight's exact byte sequence `ff 50 68`, the no-disp `ff 10`, disp32 `ff 97 ..`, REX-prefixed `41 ff 50 68` for r8/r15 base regs, and 5 rejection cases (RMW /0, SIB, RIP-rel, reg-direct, 16-bit prefix). All 34 decoder tests pass.

## v1.10.2 — CMP-form data-SEGV recovery (4.3.7 stale-pointer CTD #2)

Tonight's CTD (2026-05-26 22:40): SIGSEGV at `rip=0x100a20308`, `si_addr=0x7fb7078c9410`. Resolver-fix from v1.10.1 worked correctly (`stellaris=[0x100000000,0x103018000) found_main=1` — correctly identified the main image). Stale-pointer gate passed. But recovery declined with `op=80`. Disassembling the faulting bytes (`80 7f 30 00 75 0b ...`) showed `cmp byte ptr [rdi+0x30], 0; jne +0xb` — a check-bool-on-object idiom against a freed/dangling object pointer. The decoder only handled `MOV r, [r/m]` loads (opcode `0x8B`); the `80 /7` CMP-byte-immediate form is structurally the same kind of fault (memory read on a stale pointer) but a different opcode family.

Binary scan: ~109K total CMP-on-memory instructions in `stellaris.app`'s `__text` (opcodes `80 /7` + `83 /7`), with ~13.5K matching the `cmp [reg+disp8], 0` "check-bool-on-deref'd-ptr" idiom specifically. The pattern is common enough that a narrow decoder extension covers a substantial recovery gap.

**Fix:** add `sfix_decode_cmp_mem_imm` accepting opcodes `0x80` (byte CMP) and `0x83` (32/64-bit CMP with sign-extended imm8), `/7` subop (CMP) only, with `mod ∈ {0,1,2}` and same SIB/RIP-rel rejections as the load decoder. Add `sfix_apply_cmp_zero_flags` computing RFLAGS for `(0 - imm)` at operand size — CF/PF/AF/ZF/SF/OF derived from the standard SUB rules, all other RFLAGS bits preserved. Wire into `sfix_handle_sigsegv` as a fallthrough after the load decoder rejects.

**Why not the RMW subops** (`/0`–`/6` = ADD/OR/ADC/SBB/AND/SUB/XOR): these are read-modify-write forms (writes back to memory if mod≠11). Silently skipping a memory store risks losing program state — divergence rather than just a missed branch. The CMP variant is read-only (only sets flags), so flag-emulation is a complete model of the instruction's observable side effects. RMW recovery is deferred until we have telemetry showing it's worth the risk.

**Diagnostic upgrade.** Successful CMP recoveries log `path=data-recovered-cmp` with `op_size` and `imm`, distinct from the load form's `path=data-recovered` with `dest_reg`. Lets the doctor distinguish the two recovery classes.

**Test coverage.** `test_decoder.c` rewritten with two test groups: 10 load-decoder cases (unchanged from v1.10) + 14 CMP cases covering the tonight's-crash byte sequence, the r15 twin, dword/qword variants, the `cmp byte, 0x80` OF edge case, RIP-relative rejection, SIB rejection, 16-bit-prefix rejection, wrong-subop rejection. All 24 pass.

## v1.10.1 — fix main-image detection in dyld walker

v1.10.0 assumed `_dyld_get_image_header(0)` returned the main executable. dyld doesn't guarantee that — image 0 can be dyld itself, an injected dylib, or whichever image landed first in load order. Evidence from `stellaris-fix.log`: across recent sessions the resolver logged `stellaris=[0x10aec6000,0x10aecd000)` (7 pages) and `stellaris=[0x1026bb000,0x102a5f000)` (3.7 MB) — neither could be the 50 MB Stellaris binary. Only one run happened to land on the right image.

Impact: `g_stellaris_text_start/end` got clobbered with a wrong-image window, silently breaking the stack-scan return-address validators at `stellaris_fix.c:523/553/692/907` — eligible bad-RIP recoveries were rejected because the validator thought no candidate addresses pointed into Stellaris. The multi-image `sfix_rip_in_any_text` gate (data-segv recovery) was unaffected; it walks the full table regardless.

**Fix:** identify the main image by `mach_header.filetype == MH_EXECUTE` instead of by image index. Exactly one image has this flag (the host program). If no `MH_EXECUTE` image is found, the compile-time defaults `[0x100000000, 0x103018000)` remain in place — which still match the 4.3.7 binary bit-for-bit (Steam's `(4f3c)` rebuild label did not ship a new executable).

## v1.10.0 — runtime text bounds + framework-RIP recovery (4.3.7 CTD)

A real CTD hit a 47-minute Stellaris 4.3.7 session on 2026-05-11: `mach-exc rip=0x11fbf47a8 si_addr=0x7f96b34676c0 path=data-segv trigger=skip op=?`. The dylib caught the SIGSEGV but declined recovery. Two failures composed:

1. **`op=?` in the summary** — RIP was outside the hardcoded `[STELLARIS_TEXT_START, STELLARIS_TEXT_END)` window, so the byte-at-RIP read was skipped. With v1.8.1's constants pinned to the static binary base (`0x100000000`–`0x103018000`), any RIP in a framework / system dylib / Steam-injected library was treated as "outside stellaris" regardless of which image it was in. That's an upper bound on diagnosis quality, not just recovery — we couldn't even classify the opcode.

2. **No data-SEGV recovery for high `si_addr`** — the gate was `si_addr < LOW_MEMORY_BOUND (1 MiB) && rip in stellaris.text`. NULL-class derefs (small `si_addr`) were eligible for zero+skip; stale-heap-pointer / use-after-free derefs (high `si_addr`) were not. The 4.3.7 crash had `si_addr=0x7f96b34676c0` — a real-looking pointer, the canonical UAF signature. The pre-crash 47 minutes accumulated 14,736 `Invalid context switch [assembling_species]` script errors from `on_all_capital_buildings.txt:203`, consistent with a stale species reference leaking into a C++ path that eventually dereferenced it.

**Fixes:**

- **Runtime text bounds.** `STELLARIS_TEXT_START` / `STELLARIS_TEXT_END` are now macros redirecting to `g_stellaris_text_start` / `g_stellaris_text_end`, resolved at constructor time from `_dyld_image_count()` + LC_SEGMENT_64 walk of image 0. Eliminates version drift (4.3.3 → 4.3.4 → 4.3.5 → 4.3.7 and beyond all auto-track) and ASLR-slide gotchas. The compile-time defaults are kept as fallback in case dyld lookup ever fails.
- **Multi-image text range table.** `g_text_ranges[]` (capped at 256, populated once) records every loaded image's `__TEXT` extent. The data-recovery gate's `rip >= STELLARIS_TEXT_START && rip < STELLARIS_TEXT_END` is replaced with `sfix_rip_in_any_text(rip)` — recovery is now eligible for faults whose RIP lives in a framework, driver, or injected dylib. This is what unblocks the 4.3.7 crash class.
- **Stale-pointer recovery (default on).** The `si_addr < LOW_MEMORY_BOUND` gate is replaced with `(si_addr < LOW_MEMORY_BOUND) || g_recover_stale_ptr`. Default on; set `STELLARIS_FIX_NO_STALE_PTR=1` to revert to v1.8.1 behavior if a regression appears. The decoder still validates opcode form before mutating thread state — we only zero+skip instructions we recognize, so a "real-pointer load" recovery still falls back to crash if the decoder rejects the bytes.

**Diagnostic upgrade.** Constructor logs a `text-bounds: stellaris=[start,end) found_main=N images=N` line, so future drift is visible at session start without re-grepping the source.

**Test coverage.** `make test_decoder` still 10/10. `make test` (interpose + recovery) green: NULL-vtable recovery still works, fd limits raise, stack-size interposes hold.

## v1.8.1 — Mach message alignment + decoder mod=2 (4.3.5 telemetry)

Two bugs surfaced from the persistent log under Stellaris 4.3.5. Both were caught from the same set of crash bundles.

**Bug 1: Mach exception message struct misalignment.** The Mach handler's request struct (`sfix_exc_request_t`) lacked the `#pragma pack(push, 4)` that Apple's MIG-generated equivalents in `<mach/exc.h>` use. Default 8-byte alignment of `int64_t code[2]` inserts a 4-byte hole after `codeCnt` that the wire format doesn't have, so `code[1]` (the faulting address) was read 4 bytes past where the kernel placed it. Empirical signature: misread `si_addr` was the high 16 bits of the true address (e.g. `0x7f9a` from `0x7f9a6a9ac5f8`) — the bytes 4..7 of the real `code[1]` followed by trailer-zero padding. Two visible failure modes:

1. Bad-RIP faults at NULL got classified as data-segv. RIP was correctly read from `thread_get_state` (rip=0), but si_addr was misread (still 0 in this case from the high bits) — wait, both 0, so trigger should match. Actually the failure here is *high-mem* bad-RIP cases: when the bad RIP is non-zero (e.g., a freed function pointer that isn't all-zero), the misread si_addr won't equal it, so `trigger=skip` and we never enter path-a/path-b.
2. Real high-mem data faults misclassified as low-mem. A real fault at e.g. `si_addr=0x7f73...` was misread as `si_addr=0x7f73`, which is below `LOW_MEMORY_BOUND`. The data-recovery path (v1.7) then "recovered" by zeroing a register and advancing RIP — actively corrupting the thread state. The downstream crash an instruction or two later is the visible CTD; the original cause is hidden one step back.

**Fix:** wrap the Mach request/reply structs with `#pragma pack(push, 4)` / `pop` to match Apple's MIG layout. Verified `code[1]` lands at offset 76 (the kernel's offset) instead of 80.

**Bug 2: Decoder rejects `mod=2` (disp32) MOV-loads.** The decoder accepted `[reg]` and `[reg+disp8]` but rejected `[reg+disp32]`. v1.7's comment said "not seen yet in the patterns we care about" — 4.3.5 telemetry showed otherwise. Bytes at one observed unrecovered fault (`rip=0x1026a5abe`): `48 8B 89 18 02 00 00` = `mov rcx, [rcx+0x218]`, mod=2, rcx=NULL → `si_addr=0x218`. This is the canonical vtable thunk shape for offsets ≥ 128; declining to recover it leaves a substantial recovery gap.

**Fix:** extend `sfix_decode_simple_load` to accept `mod=2` and skip the 4-byte displacement. Mirrored in `test_decoder.c` with a new test case for the exact byte sequence.

Both fixes shipped together as a point release; no API changes, no install-side changes. Existing recovery paths (path-a / path-b / data-recovered) are unchanged in behavior — they just see correct inputs now.

## v1.9.0 — save-doctor v0.2: zero-ethic empire repair

`bin/stellaris-fix-save-doctor` gains a `--repair` flag. v0.1 was inspection-only; v0.2 actually fixes the one error-severity issue class — zero-ethic empires (the offline equivalent of the in-game `rebellionfix.34` repair).

**What it does.** When `--repair` is passed AND the save contains a country with an empty `ethos={}` block:
1. Reads the gamestate text from the source `.sav` zip
2. Finds the offending ethos block by line number (located during validation)
3. Injects `ethic="ethic_egalitarian"` as the default — same as the rebellionfix logic but more conservative (we don't try to copy from overlord since that requires more state-tracking)
4. Writes a fresh zip to `<input>.repaired.sav` with the modified gamestate and the original (unmodified) meta. **Original `.sav` is never touched.**
5. Prints a per-action log: which countries were repaired, around which line numbers

**Verified end-to-end** by synthesizing a broken save (took a real healthy save, surgically emptied `country=0`'s ethos block to reproduce the zero-ethic state), running `--repair`, and re-inspecting the output: the repair eliminates the error and the gamestate text round-trips cleanly through the parser.

**What's deliberately not repaired in v0.2.** Dangling `saved_event_target` blocks: the engine null-guards most of them; removing them safely requires understanding the parent `event_chain` block's expectations; and v0.2 is the first repair landing — best to ship one repair that's verifiably correct than several risky ones. Defer to v0.3.

**Default-ethic choice.** `ethic_egalitarian` is a low-impact safe pick: doesn't unlock fanatic-only bonuses, doesn't conflict with most civics, and matches the "republic by default" convention several vanilla rebel paths fall back to. The user can change ethics in-game post-load. v0.3 may add `--copy-from-overlord` to mirror the source mod's rebellionfix.35 logic for vassal-derived rebels.

**Exit codes:** 0 = no error/warn issues OR repair completed cleanly, 1 = unrepaired error/warn issues, 2 = ironman / unsupported.

## v1.8.0 — Fix 5: Mach exception ports (primary path); sigaction kept as fallback

The dylib gains a Mach exception port handler that runs **before** any signal is generated. When a thread crashes, the kernel sends a Mach message to our handler thread first; if we recover, we reply `KERN_SUCCESS` and the thread resumes — no signal delivery, no PLCrashReporter involvement at all. When we can't recover, we reply `KERN_FAILURE` and the kernel falls through to signal delivery, which lands in our existing sigaction handler — defense in depth.

**What changed structurally.** The recovery decision tree (path-a / path-b / data-recovered) is now a shared helper `sfix_attempt_recovery(state, si_addr, source_label)` that operates on a raw `x86_thread_state64_t *`. Both handlers extract a thread state and call it: the sigaction handler from `ucontext_t->uc_mcontext->__ss`, the Mach handler from `thread_get_state()`. Identical recovery logic, two delivery mechanisms.

**Why both layers.** If Mach setup fails for any reason — entitlement issues on a future macOS, port exhaustion, anything — the sigaction handler still installs and catches everything. The Mach layer is the primary path; sigaction is the safety net. Set `STELLARIS_FIX_NO_MACH=1` to disable Mach init explicitly (sigaction-only mode for debugging).

**Message format.** `EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES` flavor (64-bit code values), `x86_THREAD_STATE64`. The `mach_exception_raise` message (msgh_id 2405) carries the faulting thread port + task port + exception type + 2× int64 codes; `code[1]` is the faulting address (corresponds to `siginfo_t.si_addr`). Reply is a 24-byte struct (header + NDR + return code).

**Verified end-to-end.** A standalone probe (`/tmp/probe_mach2.c`, captured during development) demonstrated the kernel delivers exceptions to our port, `thread_get_state` returns valid registers, modifying state via `thread_set_state` and replying `KERN_SUCCESS` correctly resumes the faulting thread. The dylib's integration was verified against a synthetic NULL-deref test program: the Mach handler received the exception, called `sfix_attempt_recovery`, correctly declined recovery (the test binary's RIP is outside Stellaris's text range — by-design safety check), replied `KERN_FAILURE`, sigaction fallback handled chaining.

**Architectural cleanup, not user-visible value win.** The previous sigaction + interpose approach reliably won over PLCrashReporter (we install in the constructor, AND interpose `sigaction` so PLCR's later registration chains through us). The Mach migration removes that brittleness — there is no chain ordering question when we receive faults at the kernel level. But it doesn't fix any user-visible problem the sigaction layer wasn't already handling. Worth doing for robustness; not the kind of change the user will notice in normal play.

## v1.7.0 — Fix 4: data-SEGV recovery for simple loads

The dylib gains a fourth recovery class. v1.6 added classification of data-SEGVs (visibility); v1.7 actually recovers them when the faulting instruction matches a recognized safe pattern.

**What's recovered.** Data faults where:
1. `si_addr` is in the bottom 1 MiB (NULL or near-NULL deref — the canonical use-after-free vtable / object-field load symptom).
2. The faulting RIP is in `stellaris.text`.
3. The faulting instruction is a `REX.W=1 8B /r` `mov r64, [r/m64]` with `mod ∈ {00, 01}`, no SIB, no RIP-relative addressing — i.e., the simplest object-field deref forms (`mov rax, [rdi]`, `mov rax, [rdi+0x10]`, etc.). REX.R is honored so the destination can be `r8..r15`.

When all three pass, the dylib decodes the destination register, zeros it via `ucontext`, advances RIP past the instruction, and resumes. Downstream null-checks see the NULL and take their defensive branch; missing null-checks fall through and may crash again — handled by the existing path-a/path-b/data-recovered cascade subject to the rate limit.

**What's deliberately not recovered.** Writes (`mov [r/m64], r64`, opcode `0x89`), SIB-form addressing, RIP-relative loads, 32/16/8-bit operand sizes, register-direct moves, and anything outside the recognized opcode set. Misdecoding a write would silently lose data; the decoder is conservative on purpose. Patterns can be added incrementally as real-world telemetry shows them.

**Decoder.** ~30 lines, in `sfix_decode_simple_load`. Validated by a standalone unit test (`test_decoder.c`, 9 cases covering accept/reject) wired into `make test`. Algorithm is duplicated between the dylib and the test on purpose — keeps the test independent of the signal-handler machinery.

**Per-recovery telemetry** gains a new SUMMARY label `path=data-recovered`, with extra fields for the data address that faulted, the destination register that was zeroed, and the resume RIP. The doctor's per-session line now distinguishes recovered (path-a + path-b + data-recovered) from bad-RIP-not-recovered (chain) from data-segv-unrecovered (data faults that didn't match the decoder).

**Deferred from this iteration.** Mach exception port migration (planned for v1.8) — architecturally cleaner than `sigaction` + interpose, but the current sigaction approach actually works for the user's scenario, so the migration doesn't move user-visible value. Allocation ring buffer (debug-mode crash provenance) — diagnostic, doesn't prevent crashes; defer until there's a real need from telemetry.

## v1.6.0 — Dylib hardening: data-SEGV classification + call-site scoring

Two improvements to the SIGSEGV recovery handler, both on the path of "make existing recovery more accurate / give the doctor better visibility" rather than expanding the recovery class itself.

**Data-SEGV classification.** When a SIGSEGV arrives with `si_addr != rip` (a normal data fault, not the bad-RIP class we recover), the dylib previously logged the handler-entry line and silently chained to PLCrashReporter. Now it emits a `path=data-segv` SUMMARY line including the faulting data address and the first instruction byte at RIP, then chains. We don't try to recover (would need x86_64 disassembly to safely zero the dest register and skip the load — out of scope for v1.6) but the doctor command now sees these crashes and groups them as a third class alongside path-a / path-b. Real Stellaris crash bundles often span multiple classes; previously the doctor could only see the bad-RIP ones.

**Stack-scan call-site scoring.** Path-b's stack scan now runs in two passes:
1. **Strict pass** — only accepts return-address candidates whose preceding bytes look like an x86_64 call instruction (`E8 imm32`, `FF 15 rel32`, `FF /2 indirect`). Filters out callee-saved register spills that happen to match `STELLARIS_TEXT_START..END` by coincidence.
2. **Loose fallback** — if strict finds no valid pair, retry with the pre-v1.6 behavior (accept any stellaris.text address). Catches unusual call encodings the heuristic doesn't recognize.

The `bytes-before` log line (which has been there since v1.3.1 for diagnostics) gains a `[callsite]` / `[no-callsite]` tag from the new scorer. No behavior change for clean recoveries; reduced false-positive risk in path-b under unusual stack layouts.

The doctor command was updated to parse `path=data-segv` and the new flexible SUMMARY tail (since `data-segv` lines have different fields than path-a/b/chain). Per-session reporting now shows recovered / bad-RIP-not-recovered / data-segv counts separately.

## v1.7.0 — Save-doctor (offline save inspection)

`bin/stellaris-fix-save-doctor` — opens a Stellaris .sav file (zip with meta + gamestate), parses the gamestate text, and reports:

- Save metadata: empire name, in-game date, game version, fleet/planet counts, country count
- Two validators:
  - **V1 (error-severity): zero-ethic empires** — country blocks whose `ethos={}` is empty. The same corruption class the rebellionfix.34 save-load handler in the companion mod targets, but detected offline before you load a save.
  - **V2 (info-severity): dangling country event_targets** — `saved_event_target` blocks pointing to country IDs not in the live country pool. Classified by Stellaris's slot/version-byte encoding so destroyed-version markers (which the engine handles gracefully) are reported separately from genuinely-orphaned references.

Inspection-only in v0.1. Repair (rewriting `.sav` → `.repaired.sav` with corrupted state cleaned up) is deferred to v0.2 once the inspection has been validated against more real saves — mutating a save file is genuinely risky and the safer ship is "diagnose first."

`--json` flag for tooling. Exit codes: 0 = healthy, 1 = error/warn issues, 2 = ironman / unsupported binary save. Pure offline tool, no install action — invoke directly from `./bin/`.

## v1.6.0 — Doctor command + drift detector

Two new tools, both installed by `./install.sh`:

- **`bin/stellaris-fix-doctor`** — one-shot health check. Reads the persistent dylib log, the Stellaris crash bundles, the live game binary, and the install state, and prints a categorized report: game-vs-dylib alignment (drift detection), install presence, recent sessions with per-session recovery counts, recent crashes classified by signature, and a recommendations section. `--json` flag for machine-readable output. Pure read-only, runs as the user.
- **launchd drift detector** (`com.stellaris-fix.drift`) — `WatchPaths` on the Stellaris game binary; when Steam updates Stellaris and the binary's `__DATA` vmaddr shifts, posts a Notification Center alert telling the user to rebuild the dylib. Backed by `bin/stellaris-fix-drift-check` (small bash) and a sidecar at `~/.config/stellaris-mac-fixes/expected_text_end` written by `install.sh`.

Why this matters: the doctor immediately surfaced that the user's last 4 Stellaris crashes happened with the dylib *not loaded* — the launcher wasn't using "Increased Stack Size". This was invisible before; now it's the first thing the report flags.

`uninstall.sh` cleanly reverses everything: launchctl unloads the agent, removes the plist, removes the sidecar, removes the launcher pointer. Round-trip verified.

## v1.5.0 — Companion mod for script-level CTD prevention

The package is now two layers, installed by the same `install.sh`:

- **Layer 1 — `libstellaris_fix.dylib`** (binary). Catches SIGSEGVs from corrupted-vtable calls at the moment of crash, as before.
- **Layer 2 — `companion-mod/`** (Stellaris script mod). Prevents the script-level conditions that produce many of those crashes: planet-transfer null guards, dead-war cleanup, titanic-army cap defense, save-load state repair, crisis-portal anchor null fallback, marauder khan-succession scope guards, plus DLC-conditional fixes for Cybernetic Creed and the Nanite Swarm anomaly.

The dylib gains structured per-recovery telemetry: a single `SUMMARY` line per recovery decision (path-a / path-b / chain-to-PLCR) with a stable FNV-1a crash signature hash over the faulting RIP plus up to 16 stellaris-text return-address candidates on the stack. Same fault produces the same signature across runs, so repeats are easy to count.

The companion mod is **not achievement-compatible** — like all Stellaris script-level fix mods, enabling it changes the checksum and Steam achievements stop firing for that save. Disable it in the launcher's Mods list for ironman runs; the dylib still loads and provides binary-level recovery.

Companion-mod content is mined from `~~Stellaris [v4.3] General Fixes` (workshop ID 3701747681, by vladimir / FirePrince / Ariphaos / Corsairmarks). Each event file cites its source. Conflicts with that source mod itself (overlapping event-ID overrides) — do not run both at the same time.

## v1.4.0 — Generalized the Fix 3 trigger

The SIGSEGV recovery handler now fires on any instruction-fetch fault (kernel reports the faulting address equals the current `RIP`), not just on RIPs inside the NULL page. The previous `RIP < 4096` gate was generalized too narrowly from the originally-observed `0x0` / `0x1` cases — the same use-after-free family also produces larger garbage values (e.g. `0x300000000`, observed in autosave through `CPersistentName::SVariable::WriteMembers`). Recovery shape is identical regardless of the garbage value; only the trigger needed widening.

## v1.3.2 — Stellaris 4.3.5 compatibility, and a real install.sh bug

Stellaris 4.3.5 grew the `__TEXT` segment by 16 KiB. Bumped `STELLARIS_TEXT_END` from `0x103008000` to `0x10300c000` to match. Without this, recovery would silently miss crashes whose return address landed in the new tail of the segment.

Fixed `install.sh` to be properly idempotent. Previously, on re-install after a Steam game update, the installer would copy the old `.bak` over the freshly-deployed `launcher-settings.json`, regressing the launcher's version metadata. Now the `.bak` is refreshed only when the current file is in an unpatched state (first install, or after a Steam refresh wiped our patch), and the JSON patcher strips any pre-existing wrapper entry before inserting — so re-running install on an already-patched file is a true no-op.

## v1.3.1 — Replaced FP walk with stack scan in Fix 3 path (b)

The Apple-driver-side recovery path was rewritten. The original frame-pointer walk failed because parts of Apple's GL runtime (`GLEngine`, parts of `AppleMetalOpenGLRenderer`) are compiled without frame pointers and use `RBP` as a scratch register — so `RBP` from the faulting context doesn't form a valid frame chain on the first Apple frame.

The replacement is a linear stack scan: from the faulting `RSP`, look upward for two qwords in `stellaris.text`. The first is the return-into-Stellaris slot (resume target); the second bounds the Stellaris frame's size and gives us where to put `RBP`. This depends only on the Stellaris binary maintaining frame pointers (it does), not on the Apple frames in between.

## v1.3.0 — Added Fix 3 path (b) for Apple-driver-side crashes

The SIGSEGV recovery handler previously only caught crashes whose immediate caller was Stellaris itself (a virtual call inside the game went through a corrupted vtable slot). Added a second path for crashes that ended up inside an Apple GL/Metal driver call — e.g., border rendering issuing `glDrawArrays`, with the corruption being inside the driver's stale handle to a Stellaris-managed texture or vertex buffer.

The handler unwinds to the Stellaris frame that initiated the foreign call chain, cancelling the entire driver call. At most one render frame is affected; the next frame retries normally. Without this path, those crashes were unrecoverable because returning with `RAX=0` mid-driver-call would leave the Metal command encoder in an inconsistent state.

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
