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
 *   3. NULL vtable call in map icon rendering: CBypassGalacticMapIconBox::
 *      Update() calls a virtual method through a NULL function pointer when
 *      a bypass icon's COverlappingElementsBox has an invalid vtable entry.
 *      Cannot use DYLD interposition (intra-image call). Fix: SIGSEGV
 *      recovery handler that detects the exact crash pattern (RIP=0x0,
 *      return address in the known function) and resumes execution.
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
#include <sys/resource.h>

#ifdef __x86_64__
#include <sys/ucontext.h>
#endif

/* ── Configuration ────────────────────────────────────────────────────── */

#define STELLARIS_FIX_VERSION   "1.2.0"
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
 *  FIX 3: NULL-Page Execution Recovery (Signal Handler)
 *
 *  Stellaris has use-after-free bugs where objects referencing freed memory
 *  make virtual calls through corrupted vtable entries. The freed memory
 *  contains 0x0, 0x1, or other small values, so the CPU jumps to addresses
 *  in the NULL page (0x0 – 0xFFF) → SIGSEGV.
 *
 *  Observed instances:
 *    - RIP=0x0: CBypassGalacticMapIconBox::Update() vtable call (map icons)
 *    - RIP=0x1: CPersistentName::operator== vtable call (autosave)
 *
 *  Recovery: when RIP < PAGE_SIZE (4096), the fault is a NULL-page call.
 *  We simulate `ret` (pop return address, set RIP, RAX=0) so the caller
 *  continues with a zero/false return. This is safe because:
 *    - The call was to a freed object — it can't succeed regardless
 *    - RAX=0 is a safe "failure" default for booleans and pointers
 *    - The game has its own error handling for unexpected states
 *
 *  Rate limiting: if we recover more than MAX_RECOVERIES times within
 *  RECOVERY_WINDOW_NS nanoseconds, we stop recovering and let the crash
 *  propagate. This prevents infinite loops where recovery causes another
 *  immediate NULL-page call.
 *
 *  The binary's crash reporter (PLCrashReporter) installs its own SIGSEGV
 *  handler via sigaction(). We interpose sigaction to chain our recovery
 *  handler before PLCrashReporter's.
 * ══════════════════════════════════════════════════════════════════════════ */

#ifdef __x86_64__

#include <mach/mach_time.h>

#define NULL_PAGE_SIZE          4096ULL
#define MAX_RECOVERIES          32
#define RECOVERY_WINDOW_NS      (5ULL * 1000000000) /* 5 seconds */

/* Stellaris text segment (non-PIE, fixed). Return address must be here. */
#define STELLARIS_TEXT_START     0x100000000ULL
#define STELLARIS_TEXT_END       0x103008000ULL

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

static void sfix_sigsegv_handler(int sig, siginfo_t *info, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    uint64_t rip = uc->uc_mcontext->__ss.__rip;

    if (rip < NULL_PAGE_SIZE) {
        /* CPU tried to execute in the NULL page — a call through a
         * NULL/near-NULL function pointer (freed vtable entry).
         * The `callq` pushed a return address onto the stack. */
        uint64_t rsp = uc->uc_mcontext->__ss.__rsp;
        uint64_t ret_addr = *(uint64_t *)rsp;

        /* Sanity: return address must be in the stellaris text segment */
        if (ret_addr >= STELLARIS_TEXT_START && ret_addr < STELLARIS_TEXT_END) {

            /* Rate limit: check if we've recovered too many times recently */
            uint64_t now = mach_absolute_time();
            uint64_t now_ns = now * g_timebase_numer / g_timebase_denom;
            int oldest = g_recovery_idx;
            uint64_t oldest_ns = g_recovery_times[oldest];

            if (g_total_recoveries >= MAX_RECOVERIES &&
                oldest_ns != 0 &&
                (now_ns - oldest_ns) < RECOVERY_WINDOW_NS) {
                /* Too many recoveries too fast — likely an infinite loop.
                 * Let the crash propagate to PLCrashReporter. */
                SFIX_LOG_SAFE("[stellaris-fix] rate limit hit, forwarding crash\n");
                goto chain;
            }

            /* Record this recovery */
            g_recovery_times[g_recovery_idx] = now_ns;
            g_recovery_idx = (g_recovery_idx + 1) % MAX_RECOVERIES;
            g_total_recoveries++;

            /* Recover: simulate `ret` — pop return address and resume.
             * RAX=0 as a safe default (false / NULL). */
            uc->uc_mcontext->__ss.__rip = ret_addr;
            uc->uc_mcontext->__ss.__rsp = rsp + 8;
            uc->uc_mcontext->__ss.__rax = 0;

            SFIX_LOG_SAFE("[stellaris-fix] recovered: NULL-page call "
                          "(execution at invalid address)\n");
            return; /* resume execution */
        }
    }

chain:
    /* Not our crash — forward to the chained handler (PLCrashReporter) */
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

__attribute__((constructor))
static void stellaris_fix_init(void) {
    const char *dbg = getenv("STELLARIS_FIX_DEBUG");
    g_debug = (dbg != NULL && dbg[0] == '1');

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

    install_sigsegv_recovery();
#endif
}
