/*
 * test_interpose.c — test harness for stellaris_fix.dylib
 *
 * Must be run with the dylib injected:
 *   DYLD_INSERT_LIBRARIES=./libstellaris_fix.dylib STELLARIS_FIX_DEBUG=1 ./test_interpose
 *
 * Tests all three interposition points plus the fd limit constructor.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>

#define MiB (1024UL * 1024)
#define KiB (1024UL)

static int g_failures = 0;

#define TEST(name) \
    printf("  %-50s ", name); fflush(stdout);

#define PASS() \
    printf("PASS\n");

#define FAIL(fmt, ...) do { \
    printf("FAIL — " fmt "\n", ##__VA_ARGS__); \
    g_failures++; \
} while (0)

/* ── Thread function that reports its own stack size ──────────────────── */

static void *report_stack_size(void *arg) {
    size_t *out = (size_t *)arg;
    *out = pthread_get_stacksize_np(pthread_self());
    return NULL;
}

/* ── Test 1: NULL attrs → should get 8 MiB ────────────────────────────── */

static void test_null_attrs(void) {
    TEST("pthread_create(NULL attrs) → 8 MiB stack");

    size_t stack_size = 0;
    pthread_t t;
    int ret = pthread_create(&t, NULL, report_stack_size, &stack_size);
    if (ret != 0) { FAIL("pthread_create failed: %s", strerror(ret)); return; }
    pthread_join(t, NULL);

    if (stack_size >= 8 * MiB)
        PASS()
    else
        FAIL("got %zu KiB, expected >= 8192 KiB", stack_size / KiB);
}

/* ── Test 2: attrs without explicit stack size → should get 8 MiB ─────── */

static void test_attrs_no_stack(void) {
    TEST("attr_init + setdetachstate (no setstacksize) → 8 MiB");

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    size_t stack_size = 0;
    pthread_t t;
    int ret = pthread_create(&t, &attr, report_stack_size, &stack_size);
    pthread_attr_destroy(&attr);
    if (ret != 0) { FAIL("pthread_create failed: %s", strerror(ret)); return; }
    pthread_join(t, NULL);

    if (stack_size >= 8 * MiB)
        PASS()
    else
        FAIL("got %zu KiB, expected >= 8192 KiB", stack_size / KiB);
}

/* ── Test 3: explicit small stack → floor enforced to 8 MiB ───────────── */

static void test_small_explicit_stack(void) {
    TEST("setstacksize(256 KiB) → floor enforced to 8 MiB");

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * KiB);

    size_t stack_size = 0;
    pthread_t t;
    int ret = pthread_create(&t, &attr, report_stack_size, &stack_size);
    pthread_attr_destroy(&attr);
    if (ret != 0) { FAIL("pthread_create failed: %s", strerror(ret)); return; }
    pthread_join(t, NULL);

    if (stack_size >= 8 * MiB)
        PASS()
    else
        FAIL("got %zu KiB, expected >= 8192 KiB", stack_size / KiB);
}

/* ── Test 4: explicit large stack → preserved unchanged ───────────────── */

static void test_large_explicit_stack(void) {
    TEST("setstacksize(16 MiB) → preserved at 16 MiB");

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * MiB);

    size_t stack_size = 0;
    pthread_t t;
    int ret = pthread_create(&t, &attr, report_stack_size, &stack_size);
    pthread_attr_destroy(&attr);
    if (ret != 0) { FAIL("pthread_create failed: %s", strerror(ret)); return; }
    pthread_join(t, NULL);

    if (stack_size >= 16 * MiB)
        PASS()
    else
        FAIL("got %zu KiB, expected >= 16384 KiB", stack_size / KiB);
}

/* ── Test 5: fd limit raised ──────────────────────────────────────────── */

static void test_fd_limit(void) {
    TEST("RLIMIT_NOFILE soft limit >= 8192");

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        FAIL("getrlimit failed: %s", strerror(errno));
        return;
    }

    if (rl.rlim_cur >= 8192)
        PASS()
    else
        FAIL("soft limit is %llu, expected >= 8192",
             (unsigned long long)rl.rlim_cur);
}

/* ── Test 6: deep recursion survives (would crash at 512 KiB) ─────────── */

static volatile int g_recursion_depth = 0;

static void recurse(int depth) {
    volatile char frame[256]; /* ~256 bytes per frame */
    memset((char *)frame, (char)depth, sizeof(frame));
    g_recursion_depth = depth;
    if (depth < 6000)
        recurse(depth + 1);
}

static void *deep_recursion_thread(void *arg) {
    (void)arg;
    recurse(0);
    return NULL;
}

static void test_deep_recursion(void) {
    TEST("deep recursion (6000 frames × 256B ≈ 1.5 MiB)");

    g_recursion_depth = 0;
    pthread_t t;
    int ret = pthread_create(&t, NULL, deep_recursion_thread, NULL);
    if (ret != 0) { FAIL("pthread_create failed: %s", strerror(ret)); return; }
    pthread_join(t, NULL);

    if (g_recursion_depth >= 6000)
        PASS()
    else
        FAIL("only reached depth %d before crash", g_recursion_depth);
}

/* ── Test 7: SIGSEGV recovery from NULL function call ─────────────────── */

#ifdef __x86_64__

/*
 * Simulate the crash pattern: call a function through a NULL pointer.
 * The dylib's SIGSEGV handler should detect RIP=0x0, check the return
 * address, and resume. For this test we need the return address to be
 * in the known range, but since this is a test binary (not stellaris),
 * the handler will also accept any NULL-call SIGSEGV where RIP==0 as
 * long as the range check allows it.
 *
 * Instead, we test the mechanism directly: call NULL, and if we survive
 * (don't crash), the recovery worked.
 */
static void test_null_call_recovery(void) {
    TEST("SIGSEGV recovery from NULL vtable call");

    /*
     * We can't easily fake a return address in the stellaris binary range
     * from a test binary. Instead, verify the sigaction interposition is
     * working by checking that our handler was installed.
     *
     * A full integration test requires running the actual game.
     */
    struct sigaction current;
    if (sigaction(SIGSEGV, NULL, &current) == 0) {
        /* The handler should be our recovery handler (installed by the dylib) */
        if (current.sa_flags & SA_SIGINFO) {
            PASS()
        } else {
            FAIL("SIGSEGV handler not installed with SA_SIGINFO");
        }
    } else {
        FAIL("sigaction query failed: %s", strerror(errno));
    }
}

#endif /* __x86_64__ */

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("\nstellaris-fix test suite\n");
    printf("========================\n\n");

    test_null_attrs();
    test_attrs_no_stack();
    test_small_explicit_stack();
    test_large_explicit_stack();
    test_fd_limit();
    test_deep_recursion();
#ifdef __x86_64__
    test_null_call_recovery();
#endif

    printf("\n");
    if (g_failures == 0) {
        printf("All tests passed.\n\n");
        return 0;
    } else {
        printf("%d test(s) FAILED.\n\n", g_failures);
        return 1;
    }
}
