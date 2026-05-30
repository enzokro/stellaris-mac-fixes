/*
 * stellaris_fix.c — macOS crash fix for Stellaris
 *
 * Addresses three crash families when running Stellaris on macOS:
 *
 *   1. Thread stack overflow: macOS defaults to 512 KiB stacks vs 1 MiB on
 *      Windows. Task scheduler threads overflow during late-game computation.
 *      Fix: DYLD interposition of pthread_attr_init, setstacksize, create.
 *
 *   2. File descriptor exhaustion: macOS defaults to 256 open files.
 *      Fix: constructor raises RLIMIT_NOFILE to 8192.
 *
 *   3. Use-after-free vtable calls. A freed object's vtable slot gets
 *      refilled with arbitrary garbage; the CPU jumps to that non-
 *      executable address and SIGSEGVs. Observed in stellaris-internal
 *      virtual calls (autosave, map icons) and in stellaris→Apple-GL/Metal
 *      call chains where the corrupted reference is held by the driver.
 *      Cannot use DYLD interposition (intra-image / cross-binary calls).
 *      Fix: SIGSEGV recovery handler triggered on instruction-fetch faults
 *      (info->si_addr == rip). Recovers via two paths — direct ret when
 *      the corrupted call came from stellaris, stack scan when it came
 *      from inside an Apple driver call. See the FIX 3 block below.
 *
 * Build: clang -dynamiclib -arch x86_64 -arch arm64 stellaris_fix.c -o libstellaris_fix.dylib
 * Usage: DYLD_INSERT_LIBRARIES=./libstellaris_fix.dylib ./stellaris
 */

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <sys/resource.h>

#ifdef __x86_64__
#include <sys/ucontext.h>
#endif

#include <mach-o/dyld.h>
#include <mach-o/loader.h>

/* ── Configuration ────────────────────────────────────────────────────── */

#define STELLARIS_FIX_VERSION   "1.10.4"
#define TARGET_STACK_SIZE       (8UL * 1024 * 1024)   /* 8 MiB */
#define MIN_STACK_THRESHOLD     (1UL * 1024 * 1024)   /* Floor for explicit setstacksize */
#define TARGET_NOFILE           8192

/* ── DYLD Interpose Macro ─────────────────────────────────────────────── */

#define DYLD_INTERPOSE(_replacement, _replacee) \
    __attribute__((used)) static struct { \
        const void *replacement; \
        const void *replacee; \
    } _interpose_##_replacee \
    __attribute__((section("__DATA,__interpose"))) = { \
        (const void *)(unsigned long)&_replacement, \
        (const void *)(unsigned long)&_replacee \
    };

/* ── Debug Logging (opt-in via STELLARIS_FIX_DEBUG=1) ─────────────────── */

static int g_debug = 0;

#define SFIX_LOG(fmt, ...) do { \
    if (__builtin_expect(g_debug, 0)) \
        fprintf(stderr, "[stellaris-fix] " fmt "\n", ##__VA_ARGS__); \
} while (0)

/* Write is async-signal-safe; fprintf is not. Use this in signal handlers. */
#define SFIX_LOG_SAFE(msg) do { \
    if (__builtin_expect(g_debug, 0)) \
        write(STDERR_FILENO, msg, sizeof(msg) - 1); \
} while (0)

/* ── Diagnostic file log (always-on; signal-handler safe) ─────────────────
 *
 * Persistent log at ~/Documents/Paradox Interactive/Stellaris/stellaris-fix.log.
 * Captures every recovery decision (handler entry, scan hits, pair accept/
 * reject, rate-limit, chain-forward) regardless of whether STELLARIS_FIX_DEBUG
 * is set. write(2) on a pre-opened O_APPEND FD is async-signal-safe; small
 * (<PIPE_BUF) appends are atomic so concurrent writes from multiple threads
 * don't interleave.
 *
 * Formatting helpers below operate on a stack buffer; no libc calls. */

static int g_log_fd = -1;

/* All helpers marked __attribute__((unused)) because the arm64 build
 * doesn't compile the SIGSEGV handler, leaving lb_hex/lb_byte unused
 * there. -Werror would otherwise reject. */

__attribute__((unused))
static char *lb_str(char *p, const char *s) {
    while (*s) *p++ = *s++;
    return p;
}

__attribute__((unused))
static char *lb_hex(char *p, uint64_t v) {
    *p++ = '0'; *p++ = 'x';
    if (v == 0) { *p++ = '0'; return p; }
    char tmp[16];
    int n = 0;
    while (v) {
        int nib = v & 0xf;
        tmp[n++] = nib < 10 ? '0' + nib : 'a' + nib - 10;
        v >>= 4;
    }
    while (n--) *p++ = tmp[n];
    return p;
}

__attribute__((unused))
static char *lb_dec(char *p, uint64_t v) {
    if (v == 0) { *p++ = '0'; return p; }
    char tmp[24];
    int n = 0;
    while (v) { tmp[n++] = '0' + (v % 10); v /= 10; }
    while (n--) *p++ = tmp[n];
    return p;
}

__attribute__((unused))
static char *lb_byte(char *p, uint8_t b) {
    int hi = (b >> 4) & 0xf;
    int lo = b & 0xf;
    *p++ = hi < 10 ? '0' + hi : 'a' + hi - 10;
    *p++ = lo < 10 ? '0' + lo : 'a' + lo - 10;
    return p;
}

__attribute__((unused))
static void lb_flush(char *buf, char *p) {
    if (g_log_fd >= 0) {
        ssize_t r = write(g_log_fd, buf, (size_t)(p - buf));
        (void)r;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  FIX 1: Thread Stack Size (DYLD Interposition)
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Point A: pthread_attr_init ───────────────────────────────────────── */

int sfix_pthread_attr_init(pthread_attr_t *attr) {
    int ret = pthread_attr_init(attr);
    if (ret == 0) {
        pthread_attr_setstacksize(attr, TARGET_STACK_SIZE);
        SFIX_LOG("attr_init: default stack → %lu MiB", TARGET_STACK_SIZE / (1024 * 1024));
    }
    return ret;
}

DYLD_INTERPOSE(sfix_pthread_attr_init, pthread_attr_init)

/* ── Point B: pthread_attr_setstacksize ───────────────────────────────── */

int sfix_pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize) {
    if (stacksize < MIN_STACK_THRESHOLD) {
        SFIX_LOG("setstacksize: %zu KiB → %lu MiB (below threshold)",
                 stacksize / 1024, TARGET_STACK_SIZE / (1024 * 1024));
        stacksize = TARGET_STACK_SIZE;
    }
    return pthread_attr_setstacksize(attr, stacksize);
}

DYLD_INTERPOSE(sfix_pthread_attr_setstacksize, pthread_attr_setstacksize)

/* ── Point C: pthread_create ──────────────────────────────────────────── */

int sfix_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                        void *(*start_routine)(void *), void *arg) {
    if (attr == NULL) {
        pthread_attr_t fixed;
        int ret = pthread_attr_init(&fixed);
        if (ret != 0)
            return ret;

        pthread_attr_setstacksize(&fixed, TARGET_STACK_SIZE);

        SFIX_LOG("pthread_create: NULL attrs → %lu MiB stack",
                 TARGET_STACK_SIZE / (1024 * 1024));

        ret = pthread_create(thread, &fixed, start_routine, arg);
        pthread_attr_destroy(&fixed);
        return ret;
    }

    return pthread_create(thread, attr, start_routine, arg);
}

DYLD_INTERPOSE(sfix_pthread_create, pthread_create)

/* ══════════════════════════════════════════════════════════════════════════
 *  FIX 3: Bad-RIP Execution Recovery (Signal Handler)
 *
 *  Stellaris has use-after-free bugs where objects referencing freed memory
 *  make virtual calls through corrupted vtable entries. The freed memory
 *  contains arbitrary garbage, so the CPU jumps to a non-executable address
 *  → SIGSEGV with si_addr == rip (an instruction-fetch fault).
 *
 *  Observed instances (corrupted RIP values):
 *    - RIP=0x0:           CBypassGalacticMapIconBox::Update vtable (map icons)
 *    - RIP=0x1:           CPersistentName::operator== vtable (autosave)
 *    - RIP=0x300000000:   CPersistentName::SVariable::WriteMembers vtable
 *                         (autosave; high-word-only int leakage into ptr)
 *    - RIP=0x0 via Apple GL/Metal stack (borders render → glDrawArrays →
 *      stale vertex-buffer/texture vtable inside Apple driver)
 *
 *  Trigger condition: SIGSEGV where info->si_addr == rip. This is the
 *  canonical signal that the fault is from instruction fetch (CPU tried
 *  to execute at rip and that page isn't mapped/executable), as opposed
 *  to a data fault while running valid code (where si_addr is the data
 *  address, not rip). Catches all corrupted-fn-pointer crashes regardless
 *  of the specific garbage value.
 *
 *  Recovery has two paths:
 *
 *    (a) Direct ret — the bad call came from stellaris itself. The
 *        pushed return address is in the stellaris text segment. We
 *        simulate `ret`: pop the return addr into RIP, bump RSP, zero
 *        RAX as a safe false/NULL default.
 *
 *    (b) Stack scan — the bad call came from inside a system framework
 *        that stellaris called into (seen: Apple's GL→Metal translator
 *        during glDrawArrays, GLEngine during glBufferSubData). Returning
 *        with RAX=0 into the middle of an Apple driver call is not safe:
 *        subsequent instructions may rely on partial state that was being
 *        built when the fault hit. Instead we scan the stack upward for
 *        two return addresses pointing into stellaris.text, which bracket
 *        a single stellaris stack frame F (F called the Apple chain that
 *        ended at the bad call). We resume execution inside F, immediately
 *        after its call into Apple — cancelling the entire foreign chain.
 *
 *        Why scan rather than walk frame pointers: parts of Apple's GL
 *        runtime (GLEngine, parts of AppleMetalOpenGLRenderer) are
 *        compiled without frame pointers and use RBP as a scratch
 *        register. An RBP walk from the faulting context gets garbage
 *        at the first Apple frame. A linear scan is frame-pointer-free
 *        and only relies on F (stellaris) itself maintaining FPs, which
 *        the game binary reliably does.
 *
 *  Both paths set RAX=RDX=0 as a safe scalar return default (covers
 *  bool/int/pointer and the low half of a struct-returned-in-regs).
 *
 *  Rate limiting: if we recover more than MAX_RECOVERIES times within
 *  RECOVERY_WINDOW_NS nanoseconds, we stop recovering and let the crash
 *  propagate. This prevents infinite loops where recovery causes another
 *  immediate bad-RIP call. The limit is shared across both paths.
 *
 *  The binary's crash reporter (PLCrashReporter) installs its own SIGSEGV
 *  handler via sigaction(). We interpose sigaction to chain our recovery
 *  handler before PLCrashReporter's.
 * ══════════════════════════════════════════════════════════════════════════ */

#ifdef __x86_64__

#include <mach/mach.h>
#include <mach/mach_time.h>

#define MAX_RECOVERIES          32
#define RECOVERY_WINDOW_NS      (5ULL * 1000000000) /* 5 seconds */

/* Stack-scan bounds for the path-(b) fallback. 128 KiB is far beyond the
 * depth of any observed Apple-call-chain + stellaris-frame pair (typical
 * worst case is < 10 KiB). Plausible frame-size bounds below filter out
 * false positives from code-pointer-like values deep inside Apple frames. */
#define STACK_SCAN_MAX_BYTES    (128UL * 1024)
#define MIN_F_FRAME_BYTES       16UL                 /* saved-rbp + ret = smallest valid F frame */
#define MAX_F_FRAME_BYTES       (64UL * 1024)        /* 64 KiB cap on a single stellaris frame */

/* Stellaris __TEXT segment (non-PIE, fixed). Return address must be here.
 * END must equal the start of __DATA — going past would false-positive on
 * mutable-data pointers. Re-verify on every Stellaris update with:
 *   otool -l stellaris | awk '/segname __DATA/{f=1} f && /vmaddr/{print $2; exit}'
 *
 * History:
 *   4.3.3, 4.3.4: 0x103008000
 *   4.3.5:        0x10300c000  (text grew 16 KiB across the patch)
 *   4.3.7:        0x103018000  (text grew 48 KiB across the patch)
 */
/* Compile-time fallbacks for the main-executable __TEXT bounds. These match
 * 4.3.7's static binary; they are only used if runtime resolution fails.
 * The macros below redirect every call site to the runtime-resolved globals
 * so version drift (4.3.3 → 4.3.7 and beyond) and ASLR slide are tracked
 * automatically. Resolved in stellaris_fix_init() via sfix_resolve_text_bounds(). */
#define STELLARIS_TEXT_START_DEFAULT  0x100000000ULL
#define STELLARIS_TEXT_END_DEFAULT    0x103018000ULL

static uint64_t g_stellaris_text_start = STELLARIS_TEXT_START_DEFAULT;
static uint64_t g_stellaris_text_end   = STELLARIS_TEXT_END_DEFAULT;

#define STELLARIS_TEXT_START  (g_stellaris_text_start)
#define STELLARIS_TEXT_END    (g_stellaris_text_end)

/* Table of __TEXT ranges for ALL images loaded at constructor time. The
 * data-recovery gate consults this so it can attempt zero+skip recovery
 * for faults whose RIP lives in a framework or injected dylib, not just
 * stellaris itself — without that, framework-resident UAFs are always
 * unrecoverable. Populated once in sfix_resolve_text_bounds(); read
 * lock-free from signal-handler context (count is sig_atomic-style read). */
#define SFIX_MAX_TEXT_RANGES  256
struct sfix_text_range {
    uint64_t start;
    uint64_t end;
};
static struct sfix_text_range g_text_ranges[SFIX_MAX_TEXT_RANGES];
static volatile sig_atomic_t  g_text_range_count = 0;

/* Whether to extend data-SEGV recovery to non-NULL si_addr (stale heap
 * pointers, UAF). Default on; disable with STELLARIS_FIX_NO_STALE_PTR=1.
 * Resolved once at constructor time. */
static int g_recover_stale_ptr = 1;

/* Forward decls — implementations live near the constructor. */
static void sfix_resolve_text_bounds(void);
static int  sfix_rip_in_any_text(uint64_t rip);

/* Bound for "low-memory" data faults that we'll consider candidates for
 * data-SEGV recovery. NULL/near-NULL derefs land in the bottom MB of the
 * address space; legitimate-but-stale-pointer derefs land arbitrarily and
 * we don't try to recover those. */
#define LOW_MEMORY_BOUND         0x100000ULL  /* 1 MiB */

static volatile sig_atomic_t g_total_recoveries = 0;

/* Ring buffer of recent recovery timestamps for rate limiting */
static uint64_t g_recovery_times[MAX_RECOVERIES];
static int g_recovery_idx = 0;

/* The chained signal handler (PLCrashReporter's) */
static void (*g_chained_sigsegv)(int, siginfo_t *, void *) = NULL;
static struct sigaction g_chained_sa;
static volatile sig_atomic_t g_have_chained = 0;

/* mach_absolute_time conversion factor (computed once in constructor) */
static uint64_t g_timebase_numer = 1;
static uint64_t g_timebase_denom = 1;

/* Compute a 64-bit FNV-1a signature over (faulting RIP + up to 16 stellaris-
 * text return-address candidates on the stack). Stable across runs for the
 * same crash class — lets the future doctor command group repeats and
 * quantify what's hitting which path. Async-signal-safe (only reads). */
static uint64_t sfix_crash_signature(uint64_t rip, uint64_t rsp) {
    uint64_t h = 0xcbf29ce484222325ULL;
    h ^= rip;
    h *= 0x100000001b3ULL;

    if ((rsp & 0x7) != 0) return h;

    /* Smaller scan window than the path-b unwind — we only need enough
     * addresses to make the signature distinctive, not the full unwind. */
    uint64_t *p = (uint64_t *)rsp;
    uint64_t *end = p + 256;            /* 2 KiB window */
    int found = 0;
    for (; p < end && found < 16; p++) {
        uint64_t v = *(volatile uint64_t *)p;
        if (v >= STELLARIS_TEXT_START && v < STELLARIS_TEXT_END) {
            h ^= v;
            h *= 0x100000001b3ULL;
            found++;
        }
    }
    return h;
}

/* One parseable summary line per recovery decision. The field order is
 * stable so future tooling (doctor command) can split on whitespace.
 *
 * path values:
 *   a          — recovered via direct ret (caller in stellaris)
 *   b          — recovered via stack scan (caller in Apple driver)
 *   chain      — bad-RIP fault but could not recover (rate-limit / no pair)
 *   data-segv  — fault was a normal data SEGV (si_addr != rip), not our
 *                recovery class; we don't try to repair these but we record
 *                them so the doctor can show them alongside the bad-RIP class.
 */
static void sfix_log_summary(const char *path, uint64_t rip,
                             uint64_t resume_rip, uint64_t sig) {
    if (g_log_fd < 0) return;
    char buf[256], *p = buf;
    p = lb_str(p, "[stellaris-fix] SUMMARY path=");
    p = lb_str(p, path);
    p = lb_str(p, " rip=");
    p = lb_hex(p, rip);
    p = lb_str(p, " resume=");
    if (resume_rip == 0) p = lb_str(p, "none");
    else p = lb_hex(p, resume_rip);
    p = lb_str(p, " sig=");
    p = lb_hex(p, sig);
    p = lb_str(p, " slot=");
    p = lb_dec(p, (uint64_t)g_total_recoveries);
    p = lb_str(p, "/32\n");
    lb_flush(buf, p);
}

/* Variant for successful data-SEGV recoveries (v1.7). Like the data-segv
 * unrecovered variant but adds the destination register cleared and the
 * resume RIP. */
static void sfix_log_summary_data_recovered(uint64_t rip, uint64_t si_addr,
                                            uint64_t sig, int dest_reg_idx,
                                            uint64_t resume_rip) {
    if (g_log_fd < 0) return;
    char buf[256], *p = buf;
    p = lb_str(p, "[stellaris-fix] SUMMARY path=data-recovered rip=");
    p = lb_hex(p, rip);
    p = lb_str(p, " si_addr=");
    p = lb_hex(p, si_addr);
    p = lb_str(p, " resume=");
    p = lb_hex(p, resume_rip);
    p = lb_str(p, " sig=");
    p = lb_hex(p, sig);
    p = lb_str(p, " dest_reg=");
    p = lb_dec(p, (uint64_t)dest_reg_idx);
    p = lb_str(p, " slot=");
    p = lb_dec(p, (uint64_t)g_total_recoveries);
    p = lb_str(p, "/32\n");
    lb_flush(buf, p);
}

/* Variant for CMP-form data-SEGV recoveries (v1.10.2). Logs operand size
 * and imm so we can grep the log for distribution data — separate path
 * tag (`data-recovered-cmp`) so the doctor doesn't conflate it with the
 * register-zeroing load recovery. */
static void sfix_log_summary_data_recovered_cmp(uint64_t rip, uint64_t si_addr,
                                                uint64_t sig, int op_size,
                                                int64_t imm, uint64_t resume_rip) {
    if (g_log_fd < 0) return;
    char buf[256], *p = buf;
    p = lb_str(p, "[stellaris-fix] SUMMARY path=data-recovered-cmp rip=");
    p = lb_hex(p, rip);
    p = lb_str(p, " si_addr=");
    p = lb_hex(p, si_addr);
    p = lb_str(p, " resume=");
    p = lb_hex(p, resume_rip);
    p = lb_str(p, " sig=");
    p = lb_hex(p, sig);
    p = lb_str(p, " op_size=");
    p = lb_dec(p, (uint64_t)op_size);
    p = lb_str(p, " imm=");
    p = lb_hex(p, (uint64_t)(imm & 0xff));
    p = lb_str(p, " slot=");
    p = lb_dec(p, (uint64_t)g_total_recoveries);
    p = lb_str(p, "/32\n");
    lb_flush(buf, p);
}

/* Variant for indirect-call recoveries (v1.10.3). Logs instr_len so we can
 * grep for distribution data. Distinct `path=data-recovered-call` tag so
 * the doctor can separate vtable-dispatch no-ops from register-load and
 * flag-emulate recoveries. */
static void sfix_log_summary_data_recovered_call(uint64_t rip, uint64_t si_addr,
                                                 uint64_t sig, int instr_len,
                                                 uint64_t resume_rip) {
    if (g_log_fd < 0) return;
    char buf[256], *p = buf;
    p = lb_str(p, "[stellaris-fix] SUMMARY path=data-recovered-call rip=");
    p = lb_hex(p, rip);
    p = lb_str(p, " si_addr=");
    p = lb_hex(p, si_addr);
    p = lb_str(p, " resume=");
    p = lb_hex(p, resume_rip);
    p = lb_str(p, " sig=");
    p = lb_hex(p, sig);
    p = lb_str(p, " instr_len=");
    p = lb_dec(p, (uint64_t)instr_len);
    p = lb_str(p, " slot=");
    p = lb_dec(p, (uint64_t)g_total_recoveries);
    p = lb_str(p, "/32\n");
    lb_flush(buf, p);
}

/* Variant of the summary line for non-bad-RIP data SEGVs. Adds the data
 * address that faulted and the first byte of the instruction at RIP, so
 * the doctor can group these as a third crash class alongside path-a/b. */
static void sfix_log_summary_data_segv(uint64_t rip, uint64_t si_addr,
                                       uint64_t sig) {
    if (g_log_fd < 0) return;
    /* Read the instruction byte at RIP defensively. trigger=skip means RIP
     * is mapped/executable (the fault was at si_addr, not rip), so the
     * read should be safe. Gate on any loaded image's text — same surface
     * as the recovery path — otherwise framework-RIP crashes log op=?
     * and we lose the one byte that says which decoder rejected it. */
    uint8_t op = 0;
    int op_ok = 0;
    if (sfix_rip_in_any_text(rip)) {
        op = *(const volatile uint8_t *)rip;
        op_ok = 1;
    }
    char buf[256], *p = buf;
    p = lb_str(p, "[stellaris-fix] SUMMARY path=data-segv rip=");
    p = lb_hex(p, rip);
    p = lb_str(p, " si_addr=");
    p = lb_hex(p, si_addr);
    p = lb_str(p, " sig=");
    p = lb_hex(p, sig);
    if (op_ok) {
        p = lb_str(p, " op=");
        p = lb_byte(p, op);
    } else {
        p = lb_str(p, " op=?");
    }
    *p++ = '\n';
    lb_flush(buf, p);
}

/* Consume one recovery slot if the rate limit allows.
 * Returns 1 on success, 0 if over the limit. */
static int sfix_recovery_slot_take(void) {
    uint64_t now = mach_absolute_time();
    uint64_t now_ns = now * g_timebase_numer / g_timebase_denom;
    int oldest = g_recovery_idx;
    uint64_t oldest_ns = g_recovery_times[oldest];

    if (g_total_recoveries >= MAX_RECOVERIES &&
        oldest_ns != 0 &&
        (now_ns - oldest_ns) < RECOVERY_WINDOW_NS) {
        return 0;
    }

    g_recovery_times[g_recovery_idx] = now_ns;
    g_recovery_idx = (g_recovery_idx + 1) % MAX_RECOVERIES;
    g_total_recoveries++;
    return 1;
}

/* Scan the stack upward from the faulting RSP looking for two return
 * addresses pointing into stellaris.text, and unwind to the first.
 *
 *   Stellaris G → Stellaris F → Apple A → callq *NULL
 *
 * Stack at entry (addresses increase toward the top):
 *
 *   [RSP]                    ← ret_in_A        (pushed by callq *NULL)
 *   ... A's frame (locals, spilled regs; A may lack frame pointers) ...
 *   [hit1]                   ← ret_in_F        ← scan match #1 (resume target)
 *   ... F's frame (locals) ...
 *   [hit2 - 8]               ← G's saved RBP   (F's saved-rbp slot)
 *   [hit2]                   ← ret_in_G        ← scan match #2 (bounds F's frame)
 *   ...
 *
 * Unwind — simulate A returning into F right after its call-site:
 *   RIP ← *hit1      (resume inside F)
 *   RSP ← hit1 + 8   (F's RSP immediately before it called A)
 *   RBP ← hit2 - 8   (F's saved-rbp slot = F's operational RBP)
 *   RAX/RDX ← 0      (safe scalar/struct-low return default)
 *
 * Only F needs frame pointers for this to be correct — the stellaris
 * binary maintains them. Intermediate Apple frames can use RBP as a
 * scratch register without breaking the walk, because we never deref
 * RBP; we deref stack slots.
 *
 * False-positive risk: a non-return-addr qword in A's frame could happen
 * to land in stellaris.text. With a 48 MiB text segment and 16K qwords
 * scanned, expected false hits ≈ 0.003 per invocation. The min/max F
 * frame-size check discards implausible hit2 placements.
 *
 * Returns 1 on successful unwind, 0 otherwise. */
/* Check whether the 5 bytes preceding `val` look like a real call instruction
 * — used both for diagnostic logging and to filter false-positive scan hits.
 * Real return addresses follow a call. Common x86_64 call encodings:
 *   E8 imm32          (call rel32, 5 bytes total)        -> bytes[-5]=0xE8
 *   FF /2 modrm       (call r/m64, 2-7 bytes total)      -> 0xFF appears at -2..-7
 *   FF 15 imm32       (call [rip+disp], 6 bytes)         -> bytes[-6]=0xFF [-5]=0x15
 * Heuristic: 0xE8 at -5, OR 0xFF at -2/-3/-6 (the most common indirect-call
 * encodings). Returns 1 if it looks like a callsite return, 0 otherwise.
 *
 * Caller must ensure val is in stellaris.text and val-5 is too. */
static int sfix_looks_like_callsite_return(uint64_t val) {
    if (val < STELLARIS_TEXT_START + 7 || val >= STELLARIS_TEXT_END) return 0;
    const volatile uint8_t *bp = (const volatile uint8_t *)(val - 7);
    /* Indices 0..6 = bytes at val-7..val-1 */
    /* E8 rel32: byte at val-5 == 0xE8 */
    if (bp[2] == 0xE8) return 1;
    /* FF 15 rel32 (call [rip+disp32]): bytes at val-6, val-5 */
    if (bp[1] == 0xFF && bp[2] == 0x15) return 1;
    /* FF /2 with various modrm encodings — 0xFF anywhere in [-2..-6] is
     * common for indirect calls. We check the closer offsets first. */
    if (bp[5] == 0xFF) return 1;  /* val-2 */
    if (bp[4] == 0xFF) return 1;  /* val-3 */
    if (bp[3] == 0xFF) return 1;  /* val-4 */
    if (bp[1] == 0xFF) return 1;  /* val-6 */
    if (bp[0] == 0xFF) return 1;  /* val-7 */
    return 0;
}

/* Log a scan hit with the 5 bytes preceding the matched return-address
 * value, plus a [callsite] / [no-callsite] tag from the scoring helper. */
static void log_scan_hit(int idx, uint64_t addr, uint64_t val) {
    if (g_log_fd < 0) return;
    char buf[256], *p = buf;
    p = lb_str(p, "[stellaris-fix] path-b hit#");
    p = lb_dec(p, (uint64_t)idx);
    p = lb_str(p, " at=");
    p = lb_hex(p, addr);
    p = lb_str(p, " val=");
    p = lb_hex(p, val);
    /* Read 5 bytes at val-5..val-1. val is in stellaris.text (we just
     * checked); val-5 must also be in text to be safe to read. */
    if (val >= STELLARIS_TEXT_START + 5 && val < STELLARIS_TEXT_END) {
        const volatile uint8_t *bp = (const volatile uint8_t *)(val - 5);
        p = lb_str(p, " bytes-before=");
        for (int i = 0; i < 5; i++) {
            if (i > 0) *p++ = ' ';
            p = lb_byte(p, bp[i]);
        }
        p = lb_str(p, sfix_looks_like_callsite_return(val) ? " [callsite]" : " [no-callsite]");
    }
    *p++ = '\n';
    lb_flush(buf, p);
}

/* ── Data-SEGV recovery (v1.7) ────────────────────────────────────────────
 *
 * For SIGSEGVs that AREN'T the bad-RIP class (si_addr != rip), we attempt
 * recovery by:
 *   1. Confirming the fault is plausibly a NULL/near-NULL deref
 *      (si_addr in the bottom 1 MiB of the address space)
 *   2. Confirming the faulting instruction is in stellaris.text
 *   3. Decoding the instruction to recognize a small set of safe load
 *      patterns (REX-prefixed `mov r64, [r/m64]` with simple addressing)
 *   4. Zeroing the destination register and advancing RIP past the load
 *
 * If all four pass, the program continues with a NULL value where it
 * tried to deref a NULL/stale pointer. Any downstream null-check sees the
 * NULL and takes the defensive branch; missing null-checks fall through
 * and may crash again, which path-a/path-b/path-data-recovered handles
 * (subject to the rate limit).
 *
 * We deliberately handle ONLY the simplest patterns. Anything we don't
 * recognize falls through to data-segv classification + chain. Misdecoding
 * a write would be silently catastrophic — a missed write isn't observable
 * locally but corrupts state for everything downstream. The decoder
 * rejects writes (reads only), unusual addressing modes, and any
 * instruction it doesn't fully understand.
 *
 * Pointer to the appropriate ucontext register slot, indexed by the
 * x86_64 4-bit register number (with REX.R extension). */

#define UC_REG_PTR(uc, idx) (sfix_ucontext_reg_ptr(uc, idx))

static uint64_t *sfix_thread_state_reg_ptr(x86_thread_state64_t *s, int idx) {
    /* Encoding order: rax=0, rcx=1, rdx=2, rbx=3, rsp=4, rbp=5, rsi=6,
     * rdi=7, r8..r15 = 8..15. Map to x86_thread_state64_t fields. The
     * sigaction handler reaches the same fields via uc->uc_mcontext->__ss
     * which IS this struct. */
    switch (idx) {
        case 0:  return &s->__rax;
        case 1:  return &s->__rcx;
        case 2:  return &s->__rdx;
        case 3:  return &s->__rbx;
        case 4:  return &s->__rsp;
        case 5:  return &s->__rbp;
        case 6:  return &s->__rsi;
        case 7:  return &s->__rdi;
        case 8:  return &s->__r8;
        case 9:  return &s->__r9;
        case 10: return &s->__r10;
        case 11: return &s->__r11;
        case 12: return &s->__r12;
        case 13: return &s->__r13;
        case 14: return &s->__r14;
        case 15: return &s->__r15;
        default: return NULL;
    }
}

/* Decode a recognized load at `pc`. Returns instruction length on success
 * (with *dest_reg_idx set to 0..15), or 0 if not a recognized pattern.
 *
 * Recognized patterns (REX-prefix variants only — we want 64-bit ops):
 *   REX.W=1  8B /r              mov r64, r/m64    with mod=00, 01, or 10.
 * Addressing modes accepted:
 *   [reg]                       mod=00, rm != 4, rm != 5
 *   [reg+disp8]                 mod=01, rm != 4
 *   [reg+disp32]                mod=10, rm != 4
 *   [base]      via SIB         mod=00, rm=4, sib.index=4, sib.base != 5
 *   [base+disp8]  via SIB       mod=01, rm=4, sib.index=4
 *   [base+disp32] via SIB       mod=10, rm=4, sib.index=4
 * That covers the canonical vtable / object-field deref:
 *   mov rax, [rdi]              48 8B 07           (mod=00, rm=7)
 *   mov rax, [rdi+0x10]         48 8B 47 10        (mod=01, rm=7, disp8=0x10)
 *   mov rdx, [rax+0x8]          48 8B 50 08        (etc.)
 *   mov rax, [r12]              49 8B 04 24        (SIB-form; r12/rsp need SIB
 *                                                   because their bottom 3 bits
 *                                                   collide with the rm=4
 *                                                   sentinel)
 *   mov rcx, [rsp+0x40]         48 8B 4C 24 40
 * With REX.R the dest can be r8..r15 too.
 *
 * We REJECT writes (mov r/m64, r64 — opcode 0x89), SIB with a scaled index
 * register ([base + index*scale] — either pointer could be the stale one,
 * and we don't want to silently zero a register that wasn't to blame),
 * SIB with mod==0 base==5 (disp32-only, no base register), RIP-relative
 * addressing, and 32-bit / 16-bit / 8-bit variants. Each new pattern is a
 * chance to misdecode something; widen from real-world data. */
static int sfix_decode_simple_load(const uint8_t *pc, int *dest_reg_idx) {
    int off = 0;
    uint8_t rex = 0;
    /* REX prefix (0x40-0x4F) — required for 64-bit operand size on most
     * mov forms. We only handle REX.W=1 (operand size 64). */
    if ((pc[off] & 0xf0) == 0x40) {
        rex = pc[off];
        off++;
    }
    if (!(rex & 0x08)) return 0;  /* Need REX.W for 64-bit mov */

    /* Opcode: only 8B (mov r64, r/m64) — i.e. a load. */
    if (pc[off] != 0x8b) return 0;
    off++;

    uint8_t modrm = pc[off++];
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t reg = ((modrm >> 3) & 7) | ((rex & 0x04) ? 8 : 0);  /* +REX.R */
    uint8_t rm  = modrm & 7;

    /* Reject register-direct mod (no memory access — won't fault on deref). */
    if (mod == 3) return 0;
    /* Reject RIP-relative (mod==0, rm==5). Our recovery doesn't make sense
     * for those — they don't deref a register-held pointer. */
    if (mod == 0 && rm == 5) return 0;

    /* SIB byte (rm==4 in 64-bit mode means "SIB follows"). We accept the
     * no-index subset, which is the encoding the toolchain emits whenever
     * the base register's low 3 bits collide with the rm=4 sentinel
     * (rsp, r12). With sib.index=4 the addressing mode reduces to plain
     * [base] / [base+disp], same recovery semantics as the non-SIB form.
     * Reject the mod==0/base==5 form (no base register, disp32 absolute),
     * and reject scaled-index forms. */
    if (rm == 4) {
        uint8_t sib   = pc[off++];
        uint8_t index = (sib >> 3) & 7;
        uint8_t base  = sib & 7;
        if (index != 4) return 0;
        if (mod == 0 && base == 5) return 0;
    }

    if (mod == 1) off += 1;       /* disp8 */
    else if (mod == 2) off += 4;  /* disp32 */

    *dest_reg_idx = reg;
    return off;
}

/* ── CMP-form recovery (v1.10.2) ──────────────────────────────────────────
 *
 * Stellaris compiles `if (obj->some_bool)` and `if (obj->some_int != 0)`
 * checks into `cmp byte/dword [reg+disp], 0` instructions. When the object
 * pointer is stale (UAF), the memory read faults — same physical pattern
 * as a stale-pointer MOV-load, but a different opcode family.
 *
 * Recovery semantics: pretend the loaded value was 0. For CMP that means
 * computing the flag results of `(0 - imm)` at the operand size and writing
 * RFLAGS. Downstream `je`/`jne`/`jl`/etc. then take the same branch they
 * would have taken if the dead object's flag/int field happened to be 0.
 * For the most common idiom — `cmp [reg+disp], 0` — the result is ZF=1,
 * all other arithmetic flags clear, and the following `jne` falls through.
 *
 * We accept opcodes 0x80 (byte form) and 0x83 (sign-extended imm8 for
 * 32/64-bit), with /7 (CMP) only — `/0`–`/6` are read-modify-write forms
 * that would silently drop a memory store, which is unsafe. 0x66 prefix
 * (16-bit operand size) is rejected for simplicity; the binary uses it
 * rarely if at all.
 *
 * Returned operand_size is 1, 4, or 8 — matches the C type the compiler
 * was checking against. */
struct sfix_decoded_cmp {
    int     instr_len;     /* bytes to advance RIP past */
    int     operand_size;  /* 1, 4, or 8 */
    int64_t imm;           /* imm8, sign-extended to int64 */
};

static int sfix_decode_cmp_mem_imm(const uint8_t *pc, struct sfix_decoded_cmp *out) {
    int off = 0;
    uint8_t rex = 0;
    int op_size = 4;  /* default operand size for 0x83 in 64-bit mode */

    /* Reject 16-bit operand-size override; we don't see this pattern in
     * the binary and the flag math is fiddlier. */
    if (pc[off] == 0x66) return 0;

    /* REX prefix (0x40-0x4F). REX.W selects 64-bit operand size for 0x83. */
    if ((pc[off] & 0xf0) == 0x40) {
        rex = pc[off];
        if (rex & 0x08) op_size = 8;
        off++;
    }

    uint8_t opcode = pc[off++];
    if (opcode == 0x80) {
        op_size = 1;  /* byte form ignores REX.W */
    } else if (opcode == 0x83) {
        /* op_size already set above */
    } else {
        return 0;
    }

    uint8_t modrm = pc[off++];
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t subop = (modrm >> 3) & 7;  /* /N selector for group 1 */
    uint8_t rm  = modrm & 7;

    if (subop != 7) return 0;        /* /7 = CMP only */
    if (mod == 3) return 0;          /* register-direct doesn't deref */
    if (rm == 4) return 0;           /* SIB — not handling */
    if (mod == 0 && rm == 5) return 0;  /* RIP-relative — won't deref a stale ptr */

    if (mod == 1) off += 1;
    else if (mod == 2) off += 4;

    /* imm8, sign-extended. For op_size=1 the byte itself is the comparand;
     * for op_size=4/8 it's sign-extended to operand size. */
    int8_t imm8 = (int8_t)pc[off++];

    out->instr_len = off;
    out->operand_size = op_size;
    out->imm = (int64_t)imm8;
    (void)rex;  /* REX.B/X/R don't affect length or our memory-skip semantic */
    return 1;
}

/* Compute the RFLAGS bits set by `cmp 0, imm` at the given operand size,
 * and write them into the thread state. CMP is subtract-without-store, so
 * the rules are SUB's: CF, OF, SF, ZF, AF, PF. We preserve all other
 * flag bits (TF, IF, DF, etc.) — those are control state, not arithmetic.
 *
 * Notes on edge cases:
 *   - CF: 0 < imm_unsigned at operand size. For op_size>1 with sign-
 *     extended imm8: if imm8 < 0, the unsigned value at op_size becomes
 *     very large, so CF=1. Equivalent to (imm_u != 0).
 *   - OF: signed overflow only occurs when imm == INT_MIN at the operand
 *     size. For op_size=1 that's imm8==0x80. For op_size=4/8, imm8 sign-
 *     extended ranges over [-128,127], never reaches INT_MIN(32/64), so
 *     OF is always 0 for the larger sizes.
 *   - AF: borrow from bit 4. For a=0: AF = ((imm & 0xf) != 0).
 *   - PF: even parity of low byte of result. */
static void sfix_apply_cmp_zero_flags(x86_thread_state64_t *state,
                                      const struct sfix_decoded_cmp *cmp) {
    int op_size = cmp->operand_size;
    int64_t imm_s = cmp->imm;
    uint64_t imm_u;
    uint64_t result_u;

    if (op_size == 1) {
        imm_u = (uint8_t)imm_s;
        result_u = (uint64_t)((uint8_t)(0u - (uint8_t)imm_u));
    } else if (op_size == 4) {
        imm_u = (uint32_t)imm_s;  /* sign-extended at op_size */
        result_u = (uint64_t)((uint32_t)(0u - (uint32_t)imm_u));
    } else { /* 8 */
        imm_u = (uint64_t)imm_s;
        result_u = (uint64_t)(0ull - imm_u);
    }

    int zf = (imm_u == 0);
    int cf = (imm_u != 0);
    int sf, of;
    if (op_size == 1) {
        sf = (result_u >> 7) & 1;
        of = ((uint8_t)imm_s == 0x80) ? 1 : 0;
    } else if (op_size == 4) {
        sf = (result_u >> 31) & 1;
        of = 0;  /* imm8 sign-ext can't hit INT32_MIN */
    } else {
        sf = (result_u >> 63) & 1;
        of = 0;
    }
    int af = ((imm_s & 0xf) != 0);
    uint8_t low = (uint8_t)(result_u & 0xff);
    low ^= low >> 4;
    low ^= low >> 2;
    low ^= low >> 1;
    int pf = !(low & 1);  /* PF=1 when low byte has even popcount */

    /* RFLAGS bit positions: CF=0, PF=2, AF=4, ZF=6, SF=7, OF=11.
     * Preserve everything else. */
    uint64_t mask = (1ULL << 0) | (1ULL << 2) | (1ULL << 4)
                  | (1ULL << 6) | (1ULL << 7) | (1ULL << 11);
    uint64_t set  = ((uint64_t)cf <<  0)
                  | ((uint64_t)pf <<  2)
                  | ((uint64_t)af <<  4)
                  | ((uint64_t)zf <<  6)
                  | ((uint64_t)sf <<  7)
                  | ((uint64_t)of << 11);
    state->__rflags = (state->__rflags & ~mask) | set;
}

/* ── Indirect-call recovery (v1.10.3) ─────────────────────────────────────
 *
 * Recovery cascade observed 2026-05-29: load decoder zeroed RAX from a
 * stale-ptr `mov rax, [rdi]`; the very next instruction `call [rax+0x68]`
 * then faulted on the NULL vtable read (si_addr=0x68). The cascade pattern
 * is the C++ virtual-method-dispatch idiom — load vtable from object,
 * call vmethod through vtable slot. When the receiver object is dead,
 * neither operation can complete; the principled continuation is to
 * no-op the dispatched method as if it had returned `(0, 0)`.
 *
 * Decoded form: opcode 0xff with /2 subop = CALL r/m64 (near, absolute
 * indirect). REX prefix optional and ignored (operand size is already 64
 * for indirect calls in 64-bit mode). Memory addressing modes accepted:
 *   FF /2 [reg]                — mod=00 (no disp)
 *   FF /2 [reg+disp8]          — mod=01
 *   FF /2 [reg+disp32]         — mod=10
 * SIB (rm=4), RIP-relative (mod=00 rm=5), and register-direct (mod=11)
 * are rejected.
 *
 * Note we deliberately do NOT handle FF /4 (JMP near indirect) yet —
 * skipping an indirect jump is semantically riskier than skipping an
 * indirect call (the jump may be a tail-call to a function whose work
 * was meant to *replace* the caller's). Add if telemetry warrants.
 *
 * Returns instruction length (1..7 bytes) on accept, 0 on reject. */
static int sfix_decode_call_indirect_mem(const uint8_t *pc) {
    int off = 0;
    /* 0x66 operand-size override is meaningless for indirect call in 64-bit
     * mode; if present, this isn't the pattern we recover. */
    if (pc[off] == 0x66) return 0;
    /* Optional REX prefix. We don't read its bits — REX.B extends the base
     * register but we only need the instruction length, not the operand. */
    if ((pc[off] & 0xf0) == 0x40) off++;

    if (pc[off] != 0xff) return 0;
    off++;

    uint8_t modrm = pc[off++];
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t subop = (modrm >> 3) & 7;
    uint8_t rm = modrm & 7;

    if (subop != 2) return 0;          /* /2 = CALL r/m64 near indirect */
    if (mod == 3) return 0;             /* reg-direct call doesn't fault on operand fetch */
    if (rm == 4) return 0;              /* SIB — not handling */
    if (mod == 0 && rm == 5) return 0;  /* RIP-relative — not the stale-ptr shape */

    if (mod == 1) off += 1;
    else if (mod == 2) off += 4;

    return off;
}

/* Inner scan loop — finds two stellaris-text return addresses on the stack
 * that bracket a single F frame. `strict` controls whether non-callsite hits
 * are accepted (strict=1 rejects them, strict=0 is the legacy behavior).
 * Returns 1 on successful unwind+set, 0 otherwise. Logs to the persistent
 * file when g_log_fd >= 0. */
static int sfix_scan_for_pair(x86_thread_state64_t *state, int strict) {
    uint64_t rsp = state->__rsp;
    uint64_t *p   = (uint64_t *)rsp;
    uint64_t *end = p + (STACK_SCAN_MAX_BYTES / sizeof(uint64_t));
    uint64_t *hit1 = NULL;
    int hit_count = 0;

    for (; p < end; p++) {
        uint64_t val = *(volatile uint64_t *)p;
        if (val < STELLARIS_TEXT_START || val >= STELLARIS_TEXT_END)
            continue;

        /* In strict mode, skip non-callsite hits silently. In loose mode,
         * accept them and fall through to logging. */
        int callsite = sfix_looks_like_callsite_return(val);
        if (strict && !callsite) continue;

        hit_count++;
        log_scan_hit(hit_count, (uint64_t)p, val);

        if (hit1 == NULL) {
            hit1 = p;
            continue;
        }

        uint64_t f_frame_bytes = (uint64_t)(p - hit1) * sizeof(uint64_t);

        if (g_log_fd >= 0) {
            char buf[256], *bp = buf;
            bp = lb_str(bp, "[stellaris-fix] path-b pair: hit1=");
            bp = lb_hex(bp, (uint64_t)hit1);
            bp = lb_str(bp, " hit2=");
            bp = lb_hex(bp, (uint64_t)p);
            bp = lb_str(bp, " frame_bytes=");
            bp = lb_dec(bp, f_frame_bytes);
            if (f_frame_bytes < MIN_F_FRAME_BYTES || f_frame_bytes > MAX_F_FRAME_BYTES) {
                bp = lb_str(bp, " -> reject (out of bounds), sliding hit1\n");
            } else {
                bp = lb_str(bp, " -> accept\n");
            }
            lb_flush(buf, bp);
        }

        if (f_frame_bytes < MIN_F_FRAME_BYTES || f_frame_bytes > MAX_F_FRAME_BYTES) {
            hit1 = p;
            continue;
        }

        state->__rip = *(volatile uint64_t *)hit1;
        state->__rsp = (uint64_t)(hit1 + 1);
        state->__rbp = (uint64_t)p - 8;
        state->__rax = 0;
        state->__rdx = 0;
        return 1;
    }

    if (g_log_fd >= 0) {
        char buf[256], *bp = buf;
        bp = lb_str(bp, strict
                       ? "[stellaris-fix] path-b strict pass exhausted: hits="
                       : "[stellaris-fix] path-b loose pass exhausted: hits=");
        bp = lb_dec(bp, (uint64_t)hit_count);
        bp = lb_str(bp, " (no valid pair within scan window)\n");
        lb_flush(buf, bp);
    }
    return 0;
}

static int sfix_unwind_to_stellaris_frame(x86_thread_state64_t *state) {
    uint64_t rsp = state->__rsp;

    if (g_log_fd >= 0) {
        char buf[256], *p = buf;
        p = lb_str(p, "[stellaris-fix] path-b scan begin: rsp=");
        p = lb_hex(p, rsp);
        p = lb_str(p, " limit_bytes=");
        p = lb_dec(p, (uint64_t)STACK_SCAN_MAX_BYTES);
        *p++ = '\n';
        lb_flush(buf, p);
    }

    if ((rsp & 0x7) != 0) {
        if (g_log_fd >= 0) {
            char buf[128], *p = buf;
            p = lb_str(p, "[stellaris-fix] path-b abort: rsp not 8-byte aligned\n");
            lb_flush(buf, p);
        }
        return 0;
    }

    if (sfix_scan_for_pair(state, /*strict=*/1)) return 1;

    if (g_log_fd >= 0) {
        char buf[128], *p = buf;
        p = lb_str(p, "[stellaris-fix] path-b strict failed, retrying loose\n");
        lb_flush(buf, p);
    }
    return sfix_scan_for_pair(state, /*strict=*/0);
}

/* Outcome of a recovery attempt. Both the signal-handler and the Mach
 * exception-port handler call sfix_attempt_recovery() and dispatch on the
 * return value: NONE means the caller should chain to whatever fallback
 * exists (signal handler chains to PLCR's sigaction; Mach handler replies
 * KERN_FAILURE so the kernel falls back to signal delivery, which then
 * runs our sigaction handler — defense in depth). */
typedef enum {
    SFIX_REC_NONE     = 0,
    SFIX_REC_PATH_A   = 1,
    SFIX_REC_PATH_B   = 2,
    SFIX_REC_DATA     = 3,
} sfix_recovery_t;

/* Shared recovery decision. Operates on a thread state in place; modifies
 * it iff a recovery succeeds. `source_label` is "sigsegv" or "mach-exc"
 * — used in the entry log line so it's clear which delivery mechanism
 * carried the fault.
 *
 * Returns SFIX_REC_NONE if no recovery applied. The caller chains in the
 * way appropriate for its delivery mechanism. */
static sfix_recovery_t sfix_attempt_recovery(
    x86_thread_state64_t *state,
    uint64_t si_addr,
    const char *source_label
) {
    uint64_t rip = state->__rip;
    uint64_t rsp_log = state->__rsp;
    uint64_t rbp_log = state->__rbp;
    int trigger = (si_addr == rip);
    uint64_t sig_hash = sfix_crash_signature(rip, rsp_log);

    if (g_log_fd >= 0) {
        char buf[256], *p = buf;
        *p++ = '[';
        p = lb_str(p, "stellaris-fix] ");
        p = lb_str(p, source_label);
        p = lb_str(p, ": rip=");
        p = lb_hex(p, rip);
        p = lb_str(p, " si_addr=");
        p = lb_hex(p, si_addr);
        p = lb_str(p, " rsp=");
        p = lb_hex(p, rsp_log);
        p = lb_str(p, " rbp=");
        p = lb_hex(p, rbp_log);
        p = lb_str(p, " trigger=");
        p = lb_str(p, trigger ? "match" : "skip");
        *p++ = '\n';
        lb_flush(buf, p);
    }

    /* Trigger only on instruction-fetch faults (RIP itself is unmapped /
     * non-executable). For data-access faults during normal execution,
     * si_addr is the data address being touched, not RIP — they won't
     * match by coincidence, so this gate is precise.
     *
     * Non-trigger faults (data-segv) we don't try to recover (would need
     * x86_64 disassembly to safely zero the dest register and skip the
     * instruction; out of scope for v1.6). But we DO emit a SUMMARY line
     * so the doctor can see them and we can quantify how big this class
     * is in real-world play. */
    if (!trigger) {
        /* Data-SEGV path. Try data-recovery first (v1.7); classify + return
         * NONE if the fault doesn't match a recognized recoverable pattern.
         *
         * v1.9 widens this gate to recover two new classes:
         *   1. RIP in *any* loaded image (was: only stellaris.text). Crashes
         *      whose RIP lives in a framework/dylib/driver are now eligible.
         *      Required because real-world UAF stacks frequently fault deep
         *      inside Apple frameworks (GL/Metal/CF) called from stellaris.
         *   2. si_addr above LOW_MEMORY_BOUND (was: NULL-class only) when
         *      g_recover_stale_ptr is set. Use-after-free / stale-heap-
         *      pointer derefs land at arbitrary high addresses; zeroing the
         *      destination register and skipping the load lets execution
         *      continue at the cost of returning 0 instead of the stale
         *      object. The decoder still validates opcode form so we only
         *      mutate state for instructions we understand. */
        int rip_ok        = sfix_rip_in_any_text(rip);
        int si_addr_ok    = (si_addr < LOW_MEMORY_BOUND) || g_recover_stale_ptr;
        if (rip_ok && si_addr_ok) {
            int dest_reg = -1;
            int instr_len = sfix_decode_simple_load(
                (const uint8_t *)rip, &dest_reg);
            if (instr_len > 0 && dest_reg >= 0 && dest_reg <= 15) {
                if (!sfix_recovery_slot_take()) {
                    if (g_log_fd >= 0) {
                        char buf[160], *p = buf;
                        p = lb_str(p, "[stellaris-fix] data-recovery rate-limited (total=");
                        p = lb_dec(p, (uint64_t)g_total_recoveries);
                        p = lb_str(p, "), forwarding\n");
                        lb_flush(buf, p);
                    }
                    sfix_log_summary_data_segv(rip, si_addr, sig_hash);
                    return SFIX_REC_NONE;
                }
                uint64_t *dest = sfix_thread_state_reg_ptr(state, dest_reg);
                if (dest != NULL) {
                    *dest = 0;
                    uint64_t resume_rip = rip + (uint64_t)instr_len;
                    state->__rip = resume_rip;
                    if (g_log_fd >= 0) {
                        char buf[256], *p = buf;
                        p = lb_str(p, "[stellaris-fix] data-recovered: zeroed reg ");
                        p = lb_dec(p, (uint64_t)dest_reg);
                        p = lb_str(p, " advanced RIP by ");
                        p = lb_dec(p, (uint64_t)instr_len);
                        *p++ = '\n';
                        lb_flush(buf, p);
                    }
                    sfix_log_summary_data_recovered(
                        rip, si_addr, sig_hash, dest_reg, resume_rip);
                    SFIX_LOG_SAFE("[stellaris-fix] recovered: data-segv (zero+skip)\n");
                    return SFIX_REC_DATA;
                }
            }
            /* Fallthrough: load decode rejected. Try CMP-form (v1.10.2) —
             * `cmp [reg+disp], imm8` faults on stale pointers but writes only
             * RFLAGS, not a destination register. Recovery emulates the flags
             * for `0 cmp imm` and advances RIP; downstream branches take the
             * "field was zero" path. */
            struct sfix_decoded_cmp cmp;
            if (sfix_decode_cmp_mem_imm((const uint8_t *)rip, &cmp)) {
                if (!sfix_recovery_slot_take()) {
                    if (g_log_fd >= 0) {
                        char buf[160], *p = buf;
                        p = lb_str(p, "[stellaris-fix] data-recovery-cmp rate-limited (total=");
                        p = lb_dec(p, (uint64_t)g_total_recoveries);
                        p = lb_str(p, "), forwarding\n");
                        lb_flush(buf, p);
                    }
                    sfix_log_summary_data_segv(rip, si_addr, sig_hash);
                    return SFIX_REC_NONE;
                }
                sfix_apply_cmp_zero_flags(state, &cmp);
                uint64_t resume_rip = rip + (uint64_t)cmp.instr_len;
                state->__rip = resume_rip;
                if (g_log_fd >= 0) {
                    char buf[256], *p = buf;
                    p = lb_str(p, "[stellaris-fix] data-recovered-cmp: op_size=");
                    p = lb_dec(p, (uint64_t)cmp.operand_size);
                    p = lb_str(p, " imm=");
                    p = lb_hex(p, (uint64_t)(cmp.imm & 0xff));
                    p = lb_str(p, " advanced RIP by ");
                    p = lb_dec(p, (uint64_t)cmp.instr_len);
                    *p++ = '\n';
                    lb_flush(buf, p);
                }
                sfix_log_summary_data_recovered_cmp(
                    rip, si_addr, sig_hash, cmp.operand_size, cmp.imm, resume_rip);
                SFIX_LOG_SAFE("[stellaris-fix] recovered: data-segv (cmp flag-emulate)\n");
                return SFIX_REC_DATA;
            }
            /* Fallthrough: CMP decode also rejected. Try indirect-call (v1.10.3) —
             * the classic cascade is `mov rax, [stale]; call [rax+disp]`: the load
             * recovery zeros RAX, then the next instruction faults on the NULL
             * vtable read. Recovering the call as a no-op-returning-(0,0) lets
             * the dispatching loop continue past the dead element. */
            int call_len = sfix_decode_call_indirect_mem((const uint8_t *)rip);
            if (call_len > 0) {
                if (!sfix_recovery_slot_take()) {
                    if (g_log_fd >= 0) {
                        char buf[160], *p = buf;
                        p = lb_str(p, "[stellaris-fix] data-recovery-call rate-limited (total=");
                        p = lb_dec(p, (uint64_t)g_total_recoveries);
                        p = lb_str(p, "), forwarding\n");
                        lb_flush(buf, p);
                    }
                    sfix_log_summary_data_segv(rip, si_addr, sig_hash);
                    return SFIX_REC_NONE;
                }
                /* Simulate the indirect call returning (0, 0) per the SysV
                 * AMD64 ABI integer/pointer return slots. Skip past the call
                 * — the program continues at the next instruction as if the
                 * virtual method had been a no-op. */
                state->__rax = 0;
                state->__rdx = 0;
                uint64_t resume_rip = rip + (uint64_t)call_len;
                state->__rip = resume_rip;
                if (g_log_fd >= 0) {
                    char buf[256], *p = buf;
                    p = lb_str(p, "[stellaris-fix] data-recovered-call: zeroed rax/rdx, advanced RIP by ");
                    p = lb_dec(p, (uint64_t)call_len);
                    *p++ = '\n';
                    lb_flush(buf, p);
                }
                sfix_log_summary_data_recovered_call(
                    rip, si_addr, sig_hash, call_len, resume_rip);
                SFIX_LOG_SAFE("[stellaris-fix] recovered: data-segv (call no-op)\n");
                return SFIX_REC_DATA;
            }
        }
        /* Not recoverable: classify and return NONE for caller to chain. */
        sfix_log_summary_data_segv(rip, si_addr, sig_hash);
        return SFIX_REC_NONE;
    }

    /* Bad-RIP fault: caller pushed return address before the jump faulted. */
    uint64_t rsp = state->__rsp;
    uint64_t ret_addr = *(uint64_t *)rsp;
    int caller_in_stellaris =
        (ret_addr >= STELLARIS_TEXT_START && ret_addr < STELLARIS_TEXT_END);

    if (g_log_fd >= 0) {
        char buf[256], *p = buf;
        p = lb_str(p, "[stellaris-fix] caller: ret_addr=");
        p = lb_hex(p, ret_addr);
        p = lb_str(p, " in_stellaris=");
        *p++ = caller_in_stellaris ? '1' : '0';
        *p++ = '\n';
        lb_flush(buf, p);
    }

    /* Path (a): immediate caller is in stellaris. Simulate ret. */
    if (caller_in_stellaris) {
        if (!sfix_recovery_slot_take()) {
            if (g_log_fd >= 0) {
                char buf[128], *p = buf;
                p = lb_str(p, "[stellaris-fix] path-a rate-limited (total=");
                p = lb_dec(p, (uint64_t)g_total_recoveries);
                p = lb_str(p, "), forwarding\n");
                lb_flush(buf, p);
            }
            SFIX_LOG_SAFE("[stellaris-fix] rate limit hit, forwarding crash\n");
            sfix_log_summary("chain", rip, 0, sig_hash);
            return SFIX_REC_NONE;
        }
        state->__rip = ret_addr;
        state->__rsp = rsp + 8;
        state->__rax = 0;
        state->__rdx = 0;
        if (g_log_fd >= 0) {
            char buf[128], *p = buf;
            p = lb_str(p, "[stellaris-fix] path-a recovered: resume_rip=");
            p = lb_hex(p, ret_addr);
            *p++ = '\n';
            lb_flush(buf, p);
        }
        sfix_log_summary("a", rip, ret_addr, sig_hash);
        SFIX_LOG_SAFE("[stellaris-fix] recovered: bad-RIP call (direct ret)\n");
        return SFIX_REC_PATH_A;
    }

    /* Path (b): caller is in a system framework. Walk frames up to find
     * the outermost stellaris frame and unwind to it. */
    if (!sfix_recovery_slot_take()) {
        if (g_log_fd >= 0) {
            char buf[128], *p = buf;
            p = lb_str(p, "[stellaris-fix] path-b rate-limited (total=");
            p = lb_dec(p, (uint64_t)g_total_recoveries);
            p = lb_str(p, "), forwarding\n");
            lb_flush(buf, p);
        }
        SFIX_LOG_SAFE("[stellaris-fix] rate limit hit, forwarding crash\n");
        sfix_log_summary("chain", rip, 0, sig_hash);
        return SFIX_REC_NONE;
    }
    if (sfix_unwind_to_stellaris_frame(state)) {
        if (g_log_fd >= 0) {
            char buf[256], *p = buf;
            p = lb_str(p, "[stellaris-fix] path-b recovered: resume_rip=");
            p = lb_hex(p, state->__rip);
            p = lb_str(p, " resume_rsp=");
            p = lb_hex(p, state->__rsp);
            p = lb_str(p, " resume_rbp=");
            p = lb_hex(p, state->__rbp);
            *p++ = '\n';
            lb_flush(buf, p);
        }
        sfix_log_summary("b", rip, state->__rip, sig_hash);
        SFIX_LOG_SAFE("[stellaris-fix] recovered: bad-RIP call (stack scan)\n");
        return SFIX_REC_PATH_B;
    }
    /* Unwind failed. Fall through. */
    if (g_log_fd >= 0) {
        char buf[128], *p = buf;
        p = lb_str(p, "[stellaris-fix] chain to PLCrashReporter (no recovery)\n");
        lb_flush(buf, p);
    }
    sfix_log_summary("chain", rip, 0, sig_hash);
    return SFIX_REC_NONE;
}

/* Thin sigaction handler: extract thread state from ucontext, delegate the
 * recovery decision to the shared helper, chain to PLCR if no recovery. */
static void sfix_sigsegv_handler(int sig, siginfo_t *info, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    uint64_t si_addr = info ? (uint64_t)info->si_addr : 0;

    sfix_recovery_t r = sfix_attempt_recovery(
        &uc->uc_mcontext->__ss, si_addr, "sigsegv");
    if (r != SFIX_REC_NONE) return;  /* recovered — return resumes there */

    /* Not our crash (or recovery failed) — forward to the chained handler
     * (PLCrashReporter). */
    if (g_have_chained && g_chained_sigsegv) {
        g_chained_sigsegv(sig, info, ctx);
        return;
    }
    if (g_have_chained && g_chained_sa.sa_handler != SIG_DFL &&
        g_chained_sa.sa_handler != SIG_IGN) {
        g_chained_sa.sa_handler(sig);
        return;
    }

    /* No chained handler — re-raise with default disposition */
    struct sigaction dfl;
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    dfl.sa_flags = 0;
    sigaction(sig, &dfl, NULL);
    raise(sig);
}

/*
 * Interpose sigaction() to chain our SIGSEGV handler before PLCrashReporter's.
 *
 * When PLCrashReporter calls sigaction(SIGSEGV, &their_handler, &old), we:
 *   1. Save their handler as the forwarding target
 *   2. Install our handler instead
 *   3. Return success — PLCrashReporter thinks its handler is installed
 */
int sfix_sigaction(int sig, const struct sigaction *__restrict act,
                   struct sigaction *__restrict oact) {

    if (sig == SIGSEGV && act != NULL) {
        /* Save the handler being installed as our chain target */
        g_chained_sa = *act;
        if (act->sa_flags & SA_SIGINFO)
            g_chained_sigsegv = act->sa_sigaction;
        else
            g_chained_sigsegv = NULL;
        g_have_chained = 1;

        SFIX_LOG("sigaction(SIGSEGV): chained handler %p, installing recovery handler",
                 (void *)(act->sa_flags & SA_SIGINFO ?
                          (void *)act->sa_sigaction : (void *)act->sa_handler));

        /* Install our handler instead, preserving their flags */
        struct sigaction ours = *act;
        ours.sa_sigaction = sfix_sigsegv_handler;
        ours.sa_flags |= SA_SIGINFO; /* we need siginfo + ucontext */
        return sigaction(sig, &ours, oact);
    }

    return sigaction(sig, act, oact);
}

DYLD_INTERPOSE(sfix_sigaction, sigaction)

/* ══════════════════════════════════════════════════════════════════════════
 *  FIX 5: Mach exception port handler (v1.8)
 *
 *  Modern macOS delivers exceptions via Mach IPC — the kernel sends a Mach
 *  message to a registered exception port BEFORE converting the exception
 *  to a signal. Our Mach handler intercepts before any signal is generated,
 *  meaning we run before PLCrashReporter's sigaction handler ever sees the
 *  fault. If we recover, we reply KERN_SUCCESS and the thread resumes —
 *  no signal delivery, no PLCR involvement at all.
 *
 *  When we can't recover (rate limit, no pair found, unrecognized fault),
 *  we reply KERN_FAILURE. The kernel then falls back to signal delivery,
 *  which triggers our sigaction handler (the v1.7 path) — defense in depth.
 *  The sigaction handler tries the same recovery logic again (cheap, mostly
 *  fails for the same reason) and chains to PLCR for the crash dump.
 *
 *  Kept the sigaction handler installed alongside the Mach handler (via
 *  the existing constructor + sigaction interpose). If Mach init fails for
 *  any reason — entitlement issues, port exhaustion, anything — we still
 *  have the sigaction layer.
 *
 *  Message format: we use the EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES
 *  flavor, which sends a `mach_exception_raise` message (msgh_id 2405)
 *  with 64-bit code values. code[0] is the exception subcode (e.g.
 *  KERN_INVALID_ADDRESS); code[1] is the faulting address (corresponds to
 *  what siginfo_t.si_addr would contain).
 * ══════════════════════════════════════════════════════════════════════════ */

/* mach_exception_raise request — 64-bit code values.
 *
 * Pack to 4 bytes, matching Apple's MIG-generated layout in <mach/exc.h>
 * and <mach/mach_exc.h> (which wraps the equivalent struct in
 * `#pragma pack(push, 4)` under `__MigPackStructs`). Without this, the
 * default 8-byte alignment of `int64_t code[2]` inserts a 4-byte hole
 * after `codeCnt` that the wire format doesn't have — so `code[1]`
 * (the faulting address) is read 4 bytes past where the kernel placed
 * it, yielding garbage. Empirically: the misread value matches the
 * top 16 bits of the true address (the next 4 bytes of code[1] are
 * the high bits of an x86_64 user-space pointer, which are always 0). */
#pragma pack(push, 4)
typedef struct {
    mach_msg_header_t          Head;
    mach_msg_body_t            msgh_body;
    mach_msg_port_descriptor_t thread;
    mach_msg_port_descriptor_t task;
    NDR_record_t               NDR;
    exception_type_t           exception;
    mach_msg_type_number_t     codeCnt;
    int64_t                    code[2];
    mach_msg_trailer_t         trailer;
} sfix_exc_request_t;

/* Reply: just header + NDR + return code. */
typedef struct {
    mach_msg_header_t Head;
    NDR_record_t      NDR;
    kern_return_t     RetCode;
} sfix_exc_reply_t;
#pragma pack(pop)

static mach_port_t g_mach_exc_port = MACH_PORT_NULL;
static int g_mach_installed = 0;

/* Mach handler thread — never returns. Runs an infinite mach_msg loop
 * receiving exceptions on g_mach_exc_port, attempting recovery, replying. */
static void *sfix_mach_handler_thread(void *arg) {
    (void)arg;
    SFIX_LOG("mach handler thread started, port=%u", g_mach_exc_port);

    for (;;) {
        sfix_exc_request_t req;
        memset(&req, 0, sizeof(req));
        req.Head.msgh_size = sizeof(req);
        req.Head.msgh_local_port = g_mach_exc_port;

        kern_return_t kr = mach_msg(
            &req.Head, MACH_RCV_MSG, 0, sizeof(req),
            g_mach_exc_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

        if (kr != KERN_SUCCESS) {
            /* Recv failures shouldn't happen on a healthy port. Log and
             * continue — losing one message is better than killing the
             * handler thread (which would silently disable Mach recovery
             * for the rest of the process lifetime). */
            if (g_log_fd >= 0) {
                char buf[160], *p = buf;
                p = lb_str(p, "[stellaris-fix] mach recv failed kr=");
                p = lb_hex(p, (uint64_t)kr);
                *p++ = '\n';
                lb_flush(buf, p);
            }
            continue;
        }

        kern_return_t reply_code = KERN_FAILURE;
        mach_port_t thr = req.thread.name;

        /* Read faulting thread state */
        x86_thread_state64_t state;
        mach_msg_type_number_t cnt = x86_THREAD_STATE64_COUNT;
        kr = thread_get_state(thr, x86_THREAD_STATE64,
                              (thread_state_t)&state, &cnt);
        if (kr == KERN_SUCCESS) {
            uint64_t fault_addr = (uint64_t)req.code[1];
            sfix_recovery_t r = sfix_attempt_recovery(
                &state, fault_addr, "mach-exc");
            if (r != SFIX_REC_NONE) {
                kr = thread_set_state(thr, x86_THREAD_STATE64,
                                      (thread_state_t)&state,
                                      x86_THREAD_STATE64_COUNT);
                if (kr == KERN_SUCCESS) {
                    reply_code = KERN_SUCCESS;
                } else if (g_log_fd >= 0) {
                    char buf[128], *p = buf;
                    p = lb_str(p, "[stellaris-fix] mach thread_set_state failed kr=");
                    p = lb_hex(p, (uint64_t)kr);
                    *p++ = '\n';
                    lb_flush(buf, p);
                }
            }
        } else if (g_log_fd >= 0) {
            char buf[128], *p = buf;
            p = lb_str(p, "[stellaris-fix] mach thread_get_state failed kr=");
            p = lb_hex(p, (uint64_t)kr);
            *p++ = '\n';
            lb_flush(buf, p);
        }

        /* Build and send reply. KERN_SUCCESS = exception handled (kernel
         * resumes the faulting thread); KERN_FAILURE = not handled (kernel
         * falls through to signal delivery, which lands in our sigaction
         * handler as a backup recovery attempt). */
        sfix_exc_reply_t rep;
        memset(&rep, 0, sizeof(rep));
        rep.Head.msgh_bits = MACH_MSGH_BITS(
            MACH_MSGH_BITS_REMOTE(req.Head.msgh_bits), 0);
        rep.Head.msgh_size = sizeof(rep);
        rep.Head.msgh_remote_port = req.Head.msgh_remote_port;
        rep.Head.msgh_local_port = MACH_PORT_NULL;
        rep.Head.msgh_id = req.Head.msgh_id + 100;  /* reply id convention */
        rep.NDR = NDR_record;
        rep.RetCode = reply_code;
        kr = mach_msg(&rep.Head, MACH_SEND_MSG, sizeof(rep), 0,
                      MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS && g_log_fd >= 0) {
            char buf[128], *p = buf;
            p = lb_str(p, "[stellaris-fix] mach reply send failed kr=");
            p = lb_hex(p, (uint64_t)kr);
            *p++ = '\n';
            lb_flush(buf, p);
        }

        /* Release the port references the kernel transferred to us. */
        mach_port_deallocate(mach_task_self(), req.thread.name);
        mach_port_deallocate(mach_task_self(), req.task.name);
    }
    return NULL;
}

/* Install the Mach exception port + handler thread. Called from the
 * constructor. Failure is non-fatal — we fall back to the sigaction
 * handler, which is independently installed. */
static void install_mach_exception_handler(void) {
    kern_return_t kr;
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                            &g_mach_exc_port);
    if (kr != KERN_SUCCESS) {
        SFIX_LOG("  mach: port_allocate failed kr=0x%x", kr);
        return;
    }
    kr = mach_port_insert_right(mach_task_self(), g_mach_exc_port,
                                g_mach_exc_port, MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        SFIX_LOG("  mach: port_insert_right failed kr=0x%x", kr);
        mach_port_deallocate(mach_task_self(), g_mach_exc_port);
        g_mach_exc_port = MACH_PORT_NULL;
        return;
    }
    /* EXCEPTION_DEFAULT delivers via mach_msg (vs EXCEPTION_STATE which
     * sends + receives state in one round trip — incompatible with our
     * recover-then-modify pattern). MACH_EXCEPTION_CODES selects the
     * 64-bit `code` variant, required for 64-bit fault addresses. */
    kr = task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS,
                                  g_mach_exc_port,
                                  EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES,
                                  x86_THREAD_STATE64);
    if (kr != KERN_SUCCESS) {
        SFIX_LOG("  mach: task_set_exception_ports failed kr=0x%x", kr);
        return;
    }

    pthread_t t;
    if (pthread_create(&t, NULL, sfix_mach_handler_thread, NULL) != 0) {
        SFIX_LOG("  mach: pthread_create failed errno=%d", errno);
        return;
    }
    pthread_detach(t);
    g_mach_installed = 1;
    SFIX_LOG("  mach exception handler installed (port=%u)", g_mach_exc_port);
}

static void install_sigsegv_recovery(void) {
    /* Install our handler now. PLCrashReporter may overwrite it later
     * via sigaction() — our interposition will catch that and re-chain. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sfix_sigsegv_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;

    struct sigaction old;
    if (sigaction(SIGSEGV, &sa, &old) == 0) {
        /* Save any pre-existing handler */
        if (!g_have_chained && old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
            g_chained_sa = old;
            if (old.sa_flags & SA_SIGINFO)
                g_chained_sigsegv = old.sa_sigaction;
            g_have_chained = 1;
        }
        SFIX_LOG("  SIGSEGV recovery handler installed");
    }
}

#endif /* __x86_64__ */

/* ══════════════════════════════════════════════════════════════════════════
 *  Constructor: runs at dylib load, before main()
 * ══════════════════════════════════════════════════════════════════════════ */

/* Open the persistent diagnostic log and write a session-start marker.
 * Called once from the constructor. Failures are silent (file logging is
 * a diagnostic luxury, not a correctness dependency). */
static void sfix_open_log(void) {
    const char *home = getenv("HOME");
    if (home == NULL) return;
    char path[1024];
    int n = snprintf(path, sizeof(path),
                     "%s/Documents/Paradox Interactive/Stellaris/stellaris-fix.log",
                     home);
    if (n < 0 || (size_t)n >= sizeof(path)) return;
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) return;
    g_log_fd = fd;

    /* Session-start marker — anchors the log so multiple game sessions
     * are easy to demarcate when grepping. */
    char buf[256], *p = buf;
    p = lb_str(p, "\n=== [stellaris-fix] session start v");
    p = lb_str(p, STELLARIS_FIX_VERSION);
    p = lb_str(p, " pid=");
    p = lb_dec(p, (uint64_t)getpid());
    p = lb_str(p, " time=");
    time_t t = time(NULL);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char ts[32];
    size_t tslen = strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    if (tslen > 0) p = lb_str(p, ts);
    p = lb_str(p, " ===\n");
    lb_flush(buf, p);
}

#ifdef __x86_64__
/* Walk every image dyld knows about right now and record each one's
 * runtime __TEXT range. Image 0 is the main executable (stellaris); its
 * range also becomes g_stellaris_text_start / g_stellaris_text_end, used
 * by the path-a/path-b unwinders and the stack-scan signature.
 *
 * Async-signal-safe consumers read g_text_ranges + g_text_range_count
 * after this runs (the constructor completes before any handler can
 * fire), so no locking is needed. Cross-arch declarations live behind
 * #ifdef __x86_64__ because they only matter to the x86 recovery code;
 * on arm64 nothing reads the table, so we save the work entirely. */
static void sfix_resolve_text_bounds(void) {
    uint32_t n = _dyld_image_count();
    int count = 0;
    int found_main = 0;

    for (uint32_t i = 0; i < n && count < SFIX_MAX_TEXT_RANGES; i++) {
        const struct mach_header *mh_any = _dyld_get_image_header(i);
        if (mh_any == NULL) continue;
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);

        const struct load_command *lc = NULL;
        uint32_t ncmds = 0;
        uint32_t filetype = 0;
        if (mh_any->magic == MH_MAGIC_64 || mh_any->magic == MH_CIGAM_64) {
            const struct mach_header_64 *mh64 = (const struct mach_header_64 *)mh_any;
            lc = (const struct load_command *)(mh64 + 1);
            ncmds = mh64->ncmds;
            filetype = mh64->filetype;
        } else if (mh_any->magic == MH_MAGIC || mh_any->magic == MH_CIGAM) {
            lc = (const struct load_command *)(mh_any + 1);
            ncmds = mh_any->ncmds;
            filetype = mh_any->filetype;
        } else {
            continue;
        }

        /* Identify the main executable by filetype, not by image index.
         * dyld does NOT guarantee image 0 is the main binary — it can be
         * dyld itself, an injected dylib, or whichever image landed first.
         * MH_EXECUTE is set for exactly one image (the host program). */
        int is_main = (filetype == MH_EXECUTE);

        for (uint32_t j = 0; j < ncmds; j++) {
            if (lc->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *sc = (const struct segment_command_64 *)lc;
                if (strncmp(sc->segname, "__TEXT", 16) == 0 && sc->vmsize > 0) {
                    uint64_t s = (uint64_t)sc->vmaddr + (uint64_t)slide;
                    uint64_t e = s + sc->vmsize;
                    g_text_ranges[count].start = s;
                    g_text_ranges[count].end   = e;
                    if (is_main && !found_main) {
                        g_stellaris_text_start = s;
                        g_stellaris_text_end   = e;
                        found_main = 1;
                    }
                    count++;
                    break;
                }
            } else if (lc->cmd == LC_SEGMENT) {
                const struct segment_command *sc = (const struct segment_command *)lc;
                if (strncmp(sc->segname, "__TEXT", 16) == 0 && sc->vmsize > 0) {
                    uint64_t s = (uint64_t)sc->vmaddr + (uint64_t)slide;
                    uint64_t e = s + sc->vmsize;
                    g_text_ranges[count].start = s;
                    g_text_ranges[count].end   = e;
                    if (is_main && !found_main) {
                        g_stellaris_text_start = s;
                        g_stellaris_text_end   = e;
                        found_main = 1;
                    }
                    count++;
                    break;
                }
            }
            lc = (const struct load_command *)((const char *)lc + lc->cmdsize);
        }
    }

    g_text_range_count = count;

    if (g_log_fd >= 0) {
        char buf[256], *p = buf;
        p = lb_str(p, "[stellaris-fix] text-bounds: stellaris=[");
        p = lb_hex(p, g_stellaris_text_start);
        p = lb_str(p, ",");
        p = lb_hex(p, g_stellaris_text_end);
        p = lb_str(p, ") found_main=");
        p = lb_dec(p, (uint64_t)found_main);
        p = lb_str(p, " images=");
        p = lb_dec(p, (uint64_t)count);
        *p++ = '\n';
        lb_flush(buf, p);
    }
}

/* Returns 1 if `rip` falls inside any loaded image's __TEXT segment.
 * Async-signal-safe (only reads). Linear scan; image count is in the low
 * hundreds at most, so this is cheap. */
static int sfix_rip_in_any_text(uint64_t rip) {
    int n = (int)g_text_range_count;
    for (int i = 0; i < n; i++) {
        if (rip >= g_text_ranges[i].start && rip < g_text_ranges[i].end)
            return 1;
    }
    return 0;
}
#endif /* __x86_64__ */

__attribute__((constructor))
static void stellaris_fix_init(void) {
    const char *dbg = getenv("STELLARIS_FIX_DEBUG");
    g_debug = (dbg != NULL && dbg[0] == '1');

    sfix_open_log();

#ifdef __x86_64__
    /* Resolve __TEXT bounds of all loaded images. Must run before any
     * Mach/sigaction handler is installed so signal-handler reads of the
     * range table see fully-populated data. */
    sfix_resolve_text_bounds();

    /* Default-on stale-pointer recovery (extends data-SEGV zero+skip to
     * non-NULL si_addr — covers use-after-free / stale heap pointers,
     * the class that bit us in the 2026-05-11 framework-RIP crash). */
    g_recover_stale_ptr = (getenv("STELLARIS_FIX_NO_STALE_PTR") == NULL);
#endif

    SFIX_LOG("v%s loaded", STELLARIS_FIX_VERSION);
    SFIX_LOG("  target stack: %lu MiB, floor: %lu MiB",
             TARGET_STACK_SIZE / (1024 * 1024),
             MIN_STACK_THRESHOLD / (1024 * 1024));

    /* Raise file descriptor soft limit */
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        SFIX_LOG("  fd limits: soft=%llu hard=%llu",
                 (unsigned long long)rl.rlim_cur,
                 (unsigned long long)rl.rlim_max);

        if (rl.rlim_cur < TARGET_NOFILE) {
            rlim_t target = TARGET_NOFILE;
            if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < target)
                target = rl.rlim_max;

            rl.rlim_cur = target;
            if (setrlimit(RLIMIT_NOFILE, &rl) == 0) {
                SFIX_LOG("  fd soft limit raised to %llu",
                         (unsigned long long)rl.rlim_cur);
            } else {
                SFIX_LOG("  WARNING: setrlimit(NOFILE) failed: %s",
                         strerror(errno));
            }
        }
    }

#ifdef __x86_64__
    /* Initialize mach_absolute_time conversion for rate limiting */
    mach_timebase_info_data_t tb;
    if (mach_timebase_info(&tb) == KERN_SUCCESS) {
        g_timebase_numer = tb.numer;
        g_timebase_denom = tb.denom;
    }

    /* Install Mach exception port handler (primary path) — runs before
     * any signal is generated. If init fails (entitlement issues, port
     * exhaustion, anything), the sigaction handler below still runs. */
    if (getenv("STELLARIS_FIX_NO_MACH") == NULL) {
        install_mach_exception_handler();
    } else {
        SFIX_LOG("  mach handler disabled by STELLARIS_FIX_NO_MACH=1");
    }

    /* Sigaction handler (fallback path) — always installed. Catches
     * anything the Mach handler reply=KERN_FAILURE'd, plus everything
     * if Mach init failed entirely. */
    install_sigsegv_recovery();
#endif
}
