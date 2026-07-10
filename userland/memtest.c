/*
 * memtest.c — ModuOS Userland Memory Diagnostic Utility
 *
 * Tests:
 *   1. Basic alloc/free
 *   2. Pattern integrity (write & verify)
 *   3. realloc (grow, shrink, from NULL)
 *   4. Fragmentation & coalescing
 *   5. Boundary / heap header corruption detection
 *   6. Stress test (many allocs, random free order)
 *   7. calloc (zeroing, overflow)
 *   8. Large allocation ladder
 *   9. Alignment checks
 *  10. Heap integrity after mixed operations
 */

#include "libc.h"

/* ── Result tracking ─────────────────────────────────────────────────── */

static int g_pass = 0;
static int g_fail = 0;

static void pass(const char *msg) {
    printf("    \033[32m\b[PASS]\033[0m\b %s\n", msg);
    g_pass++;
}

static void fail(const char *msg) {
    printf("    \033[31m\b[FAIL]\033[0m\b %s\n", msg);
    g_fail++;
}

static void section(const char *title) {
    printf("\n\033[1m\b\033[97m\b── %s\033[0m\b\n", title);
}

static void info(const char *msg) {
    printf("    \033[90m\b%s\033[0m\b\n", msg);
}

#define CHECK(cond, label) do { if (cond) pass(label); else fail(label); } while(0)

/* ── Memory helpers ──────────────────────────────────────────────────── */

static int pattern_write_verify(void *buf, size_t len, unsigned char pat) {
    unsigned char *p = (unsigned char *)buf;
    for (size_t i = 0; i < len; i++) p[i] = pat;
    for (size_t i = 0; i < len; i++) if (p[i] != pat) return 0;
    return 1;
}

static int mem_all_eq(void *buf, size_t len, unsigned char val) {
    unsigned char *p = (unsigned char *)buf;
    for (size_t i = 0; i < len; i++) if (p[i] != val) return 0;
    return 1;
}

static int mem_all_zero(void *buf, size_t len) {
    return mem_all_eq(buf, len, 0);
}

/* ── Test 1: Basic malloc / free ─────────────────────────────────────── */

static void test_basic(void) {
    section("Test 1: Basic malloc / free");

    void *p = malloc(64);
    printf("    malloc(64)  → 0x%x\n", (unsigned int)(uintptr_t)p);
    CHECK(p != NULL, "malloc(64) returns non-NULL");

    void *q = malloc(256);
    printf("    malloc(256) → 0x%x\n", (unsigned int)(uintptr_t)q);
    CHECK(q != NULL, "malloc(256) returns non-NULL");

    CHECK(p != q, "Two allocations return distinct pointers");

    free(p);
    free(q);
    info("free(p), free(q) called");

    /* Re-malloc should reuse without growing heap */
    void *brk_before = sbrk(0);
    void *r = malloc(64);
    void *brk_after  = sbrk(0);
    printf("    re-malloc(64) → 0x%x  brk delta=%d\n",
           (unsigned int)(uintptr_t)r,
           (int)((char*)brk_after - (char*)brk_before));
    CHECK(r != NULL, "Re-malloc after free returns non-NULL");
    CHECK(brk_after == brk_before, "Re-malloc reuses freed block (no brk growth)");
    free(r);

    CHECK(malloc(0) == NULL, "malloc(0) returns NULL");
}

/* ── Test 2: Pattern integrity ───────────────────────────────────────── */

static void test_pattern(void) {
    section("Test 2: Pattern integrity (write & verify)");

    size_t sizes[] = { 1, 7, 16, 63, 64, 65, 127, 128, 255, 256, 1023, 1024, 4096 };
    int n = (int)(sizeof(sizes) / sizeof(sizes[0]));

    for (int i = 0; i < n; i++) {
        void *p = malloc(sizes[i]);
        if (!p) { fail("malloc returned NULL"); continue; }
        unsigned char pat = (unsigned char)(0xA5 ^ (unsigned char)i);
        int ok = pattern_write_verify(p, sizes[i], pat);
        char label[64];
        snprintf(label, sizeof(label), "Pattern 0x%02x over %d bytes", pat, (int)sizes[i]);
        CHECK(ok, label);
        free(p);
    }

    /* Two live buffers must not overlap */
    unsigned char *a = malloc(128);
    unsigned char *b = malloc(128);
    if (a && b) {
        for (int i = 0; i < 128; i++) { a[i] = 0xAA; b[i] = 0x55; }
        CHECK(mem_all_eq(a, 128, 0xAA) && mem_all_eq(b, 128, 0x55),
              "Two live buffers do not overlap (0xAA/0x55 isolation)");
    } else {
        fail("Could not allocate two 128-byte buffers");
    }
    free(a);
    free(b);

    /* Walking bit pattern */
    unsigned char *wb = malloc(256);
    if (wb) {
        for (int i = 0; i < 256; i++) wb[i] = (unsigned char)(1 << (i % 8));
        int ok = 1;
        for (int i = 0; i < 256; i++)
            if (wb[i] != (unsigned char)(1 << (i % 8))) { ok = 0; break; }
        CHECK(ok, "Walking bit pattern (256 bytes) intact");
        free(wb);
    }
}

/* ── Test 3: realloc ─────────────────────────────────────────────────── */

static void test_realloc(void) {
    section("Test 3: realloc (grow, shrink, NULL, zero)");

    void *p = realloc(NULL, 64);
    CHECK(p != NULL, "realloc(NULL, 64) acts as malloc");
    memset(p, 0xBB, 64);

    void *q = realloc(p, 256);
    printf("    grow  0x%x → 0x%x\n", (unsigned int)(uintptr_t)p, (unsigned int)(uintptr_t)q);
    CHECK(q != NULL, "realloc grow to 256 succeeds");
    if (q) {
        CHECK(mem_all_eq(q, 64, 0xBB), "realloc grow preserves original 64 bytes");
        memset(q, 0xCC, 256);
    }

    void *r = realloc(q, 32);
    printf("    shrink 0x%x → 0x%x\n", (unsigned int)(uintptr_t)q, (unsigned int)(uintptr_t)r);
    CHECK(r != NULL, "realloc shrink to 32 succeeds");
    if (r) CHECK(mem_all_eq(r, 32, 0xCC), "realloc shrink preserves first 32 bytes");

    CHECK(realloc(r, 0) == NULL, "realloc(ptr, 0) returns NULL (acts as free)");

    /* Grow chain: verify data survives every step */
    void *t = malloc(16);
    if (t) {
        memset(t, 0x12, 16);
        size_t steps[] = { 32, 64, 128, 256, 512, 1024, 2048 };
        int ok = 1;
        for (int i = 0; i < 7; i++) {
            void *u = realloc(t, steps[i]);
            if (!u) { ok = 0; break; }
            if (!mem_all_eq(u, 16, 0x12)) { ok = 0; free(u); break; }
            memset(u, 0x12, steps[i]);
            t = u;
        }
        CHECK(ok, "realloc chain 16→32→64→…→2048 preserves sentinel byte");
        free(t);
    }
}

/* ── Test 4: Fragmentation & coalescing ──────────────────────────────── */

static void test_fragmentation(void) {
    section("Test 4: Fragmentation & coalescing");

    void *blk[8];
    for (int i = 0; i < 8; i++) {
        blk[i] = malloc(128);
        CHECK(blk[i] != NULL, "fragmentation setup: alloc 128");
    }

    info("Freeing even-indexed blocks (0,2,4,6)…");
    for (int i = 0; i < 8; i += 2) free(blk[i]);
    info("Freeing odd-indexed blocks (1,3,5,7) — triggers coalesce…");
    for (int i = 1; i < 8; i += 2) free(blk[i]);

    void *brk_before = sbrk(0);
    void *big = malloc(512);
    void *brk_after  = sbrk(0);
    printf("    post-coalesce malloc(512) → 0x%x  brk delta=%d\n",
           (unsigned int)(uintptr_t)big,
           (int)((char*)brk_after - (char*)brk_before));
    CHECK(big != NULL, "Post-coalesce malloc(512) succeeds");
    CHECK(brk_after == brk_before, "Post-coalesce malloc(512) reuses memory (no brk growth)");
    free(big);

    /* Hole-in-the-middle */
    void *x = malloc(64);
    void *y = malloc(64);
    void *z = malloc(64);
    free(y);
    void *y2 = malloc(32);
    CHECK(y2 != NULL, "malloc(32) into middle hole succeeds");
    void *y3 = malloc(32);
    CHECK(y3 != NULL, "second malloc(32) after middle hole succeeds");
    free(x); free(y2); free(y3); free(z);

    /* Checkerboard: alloc 16 × 64, free every 3rd, re-alloc small blocks */
    info("Checkerboard free pattern…");
    void *cb[16];
    for (int i = 0; i < 16; i++) cb[i] = malloc(64);
    for (int i = 0; i < 16; i += 3) { free(cb[i]); cb[i] = NULL; }
    int cb_ok = 1;
    for (int i = 0; i < 16; i++) {
        if (cb[i]) {
            if (!mem_all_eq(cb[i], 0, 0)) { /* just touch it */ }
        } else {
            cb[i] = malloc(32);
            if (!cb[i]) cb_ok = 0;
        }
    }
    CHECK(cb_ok, "Checkerboard re-alloc into freed slots all succeeded");
    for (int i = 0; i < 16; i++) free(cb[i]);
}

/* ── Test 5: Boundary & corruption detection ─────────────────────────── */

static void test_boundary(void) {
    section("Test 5: Boundary & corruption detection");

    unsigned char *a = malloc(32);
    unsigned char *b = malloc(32);
    if (!a || !b) { fail("Setup alloc failed"); return; }

    memset(a, 0xDE, 32);
    memset(b, 0xAD, 32);
    CHECK(mem_all_eq(a, 32, 0xDE), "Buffer a (0xDE) intact after b written");
    CHECK(mem_all_eq(b, 32, 0xAD), "Buffer b (0xAD) intact after a written");

    ptrdiff_t dist = (unsigned char*)b > (unsigned char*)a
                   ? (unsigned char*)b - (unsigned char*)a
                   : (unsigned char*)a - (unsigned char*)b;
    printf("    Distance between a and b: %d bytes\n", (int)dist);
    CHECK(dist >= 32, "Allocations are at least 32 bytes apart");
    free(a); free(b);

    /* Double-free: should silently no-op, not crash */
    info("Double-free safety check…");
    void *df = malloc(64);
    CHECK(df != NULL, "Alloc for double-free test");
    free(df);
    free(df);
    pass("Double-free did not crash (magic/flag guard)");

    /* NULL free */
    free(NULL);
    pass("free(NULL) did not crash");

    /* 1 MB alloc with full pattern verify */
    void *big = malloc(1024 * 1024);
    printf("    malloc(1MB) → 0x%x\n", (unsigned int)(uintptr_t)big);
    CHECK(big != NULL, "malloc(1MB) succeeds");
    if (big) {
        CHECK(pattern_write_verify(big, 1024 * 1024, 0x5A), "1MB pattern 0x5A write/verify");
        free(big);
    }

    /* Verify first and last byte of an allocation are writable */
    unsigned char *edge = malloc(1024);
    if (edge) {
        edge[0]    = 0x11;
        edge[1023] = 0x22;
        CHECK(edge[0] == 0x11 && edge[1023] == 0x22,
              "First and last byte of 1024-byte alloc writable");
        free(edge);
    }
}

/* ── Test 6: Stress test ─────────────────────────────────────────────── */

#define STRESS_N 64

static void test_stress(void) {
    section("Test 6: Stress test (64 allocs, scrambled free order)");

    void  *ptrs[STRESS_N];
    size_t szs[STRESS_N];

    for (int i = 0; i < STRESS_N; i++)
        szs[i] = (size_t)(16 + (i * 37) % 480 + 1);

    int all_ok = 1;
    for (int i = 0; i < STRESS_N; i++) {
        ptrs[i] = malloc(szs[i]);
        if (!ptrs[i]) { all_ok = 0; continue; }
        memset(ptrs[i], (unsigned char)(i + 1), szs[i]);
    }
    CHECK(all_ok, "All 64 stress allocations succeeded");

    int patterns_ok = 1;
    for (int i = 0; i < STRESS_N; i++) {
        if (!ptrs[i]) continue;
        if (!mem_all_eq(ptrs[i], szs[i], (unsigned char)(i + 1))) {
            patterns_ok = 0;
            printf("    Pattern mismatch at index %d (size %d)\n", i, (int)szs[i]);
        }
    }
    CHECK(patterns_ok, "All 64 patterns intact while fully live");

    info("Freeing in reverse order…");
    for (int i = STRESS_N - 1; i >= 0; i--) free(ptrs[i]);

    void *brk_before = sbrk(0);
    for (int i = 0; i < STRESS_N; i++) {
        ptrs[i] = malloc(szs[i]);
        if (ptrs[i]) memset(ptrs[i], 0xFF, szs[i]);
    }
    void *brk_after = sbrk(0);
    printf("    Re-alloc brk delta: %d bytes\n",
           (int)((char*)brk_after - (char*)brk_before));
    CHECK(brk_after == brk_before, "Re-alloc after full free reuses memory (no brk growth)");
    for (int i = 0; i < STRESS_N; i++) free(ptrs[i]);

    /* Interleave: alloc 32, free 32, alloc 32 */
    info("Interleave: alloc 32, free 32, alloc 32…");
    for (int i = 0; i < 32; i++) ptrs[i] = malloc(szs[i]);
    for (int i = 0; i < 32; i++) free(ptrs[i]);
    brk_before = sbrk(0);
    for (int i = 0; i < 32; i++) ptrs[i] = malloc(szs[i]);
    brk_after = sbrk(0);
    CHECK(brk_after == brk_before, "Interleaved alloc/free/alloc reuses memory");
    for (int i = 0; i < 32; i++) free(ptrs[i]);
}

/* ── Test 7: calloc ──────────────────────────────────────────────────── */

static void test_calloc(void) {
    section("Test 7: calloc (zeroing & overflow)");

    void *p = calloc(16, 8);   /* 128 bytes */
    CHECK(p != NULL, "calloc(16, 8) returns non-NULL");
    if (p) {
        CHECK(mem_all_zero(p, 128), "calloc(16, 8) — all 128 bytes are zero");
        free(p);
    }

    /* Single element */
    int *ip = calloc(1, sizeof(int));
    CHECK(ip != NULL, "calloc(1, sizeof(int)) succeeds");
    if (ip) {
        CHECK(*ip == 0, "calloc single int is zero");
        *ip = 42;
        CHECK(*ip == 42, "calloc'd int writable");
        free(ip);
    }

    /* Large calloc */
    void *big = calloc(1024, 64);   /* 64 KB */
    CHECK(big != NULL, "calloc(1024, 64) = 64KB succeeds");
    if (big) {
        CHECK(mem_all_zero(big, 1024 * 64), "calloc 64KB — all bytes zero");
        free(big);
    }

    /* Overflow: nmemb * size overflows size_t — must return NULL */
    void *ov = calloc((size_t)-1, 2);
    CHECK(ov == NULL, "calloc overflow (SIZE_MAX * 2) returns NULL");

    /* calloc(0, n) and calloc(n, 0) */
    CHECK(calloc(0, 64) == NULL, "calloc(0, 64) returns NULL");
    CHECK(calloc(64, 0) == NULL, "calloc(64, 0) returns NULL");
}

/* ── Test 8: Large allocation ladder ────────────────────────────────── */

static void test_large(void) {
    section("Test 8: Large allocation ladder");

    size_t ladder[] = {
        4096, 8192, 16384, 32768, 65536,
        131072, 262144, 524288, 1048576   /* 1 MB */
    };
    int n = (int)(sizeof(ladder) / sizeof(ladder[0]));

    for (int i = 0; i < n; i++) {
        void *p = malloc(ladder[i]);
        printf("    malloc(%6d KB) → 0x%x\n",
               (int)(ladder[i] / 1024), (unsigned int)(uintptr_t)p);
        char label[64];
        snprintf(label, sizeof(label), "malloc(%d KB) non-NULL", (int)(ladder[i]/1024));
        CHECK(p != NULL, label);
        if (p) {
            /* Write first and last byte only to keep it fast */
            ((unsigned char*)p)[0]           = 0xF0;
            ((unsigned char*)p)[ladder[i]-1] = 0x0F;
            int rw_ok = ((unsigned char*)p)[0]           == 0xF0
                     && ((unsigned char*)p)[ladder[i]-1] == 0x0F;
            snprintf(label, sizeof(label), "malloc(%d KB) edges R/W", (int)(ladder[i]/1024));
            CHECK(rw_ok, label);
            free(p);
        }
    }

    /* All freed — one big re-alloc should reuse */
    void *brk_before = sbrk(0);
    void *reuse = malloc(1048576);
    void *brk_after  = sbrk(0);
    CHECK(reuse != NULL, "Post-ladder malloc(1MB) succeeds");
    CHECK(brk_after == brk_before, "Post-ladder malloc(1MB) reuses memory");
    free(reuse);
}

/* ── Test 9: Alignment ───────────────────────────────────────────────── */

static void test_alignment(void) {
    section("Test 9: Pointer alignment (16-byte)");

    size_t sizes[] = { 1, 3, 7, 8, 9, 15, 16, 17, 31, 32, 63, 64, 127, 128, 255, 256 };
    int n = (int)(sizeof(sizes) / sizeof(sizes[0]));

    for (int i = 0; i < n; i++) {
        void *p = malloc(sizes[i]);
        if (!p) { fail("malloc returned NULL for alignment test"); continue; }
        int aligned = ((uintptr_t)p % 16) == 0;
        char label[64];
        snprintf(label, sizeof(label), "malloc(%3d) is 16-byte aligned (0x%x)",
                 (int)sizes[i], (unsigned int)(uintptr_t)p);
        CHECK(aligned, label);
        free(p);
    }

    /* Verify realloc'd pointers are also aligned */
    void *p = malloc(1);
    int ok = 1;
    size_t steps[] = { 17, 33, 65, 129, 257, 513 };
    for (int i = 0; i < 6; i++) {
        p = realloc(p, steps[i]);
        if (!p || ((uintptr_t)p % 16) != 0) { ok = 0; break; }
    }
    CHECK(ok, "realloc chain produces 16-byte aligned pointers");
    free(p);
}

/* ── Test 10: Mixed heap integrity ───────────────────────────────────── */

static void test_mixed(void) {
    section("Test 10: Mixed heap integrity");

    /*
     * Simulate a realistic workload: varied sizes, interleaved alloc/realloc/free,
     * verify no allocation ever lands on top of another live one.
     */
#define MIX_N 32
    void   *live[MIX_N];
    size_t  live_sz[MIX_N];
    memset(live, 0, sizeof(live));
    memset(live_sz, 0, sizeof(live_sz));

    int overlap = 0;
    int ops = 0;

    for (int round = 0; round < 8; round++) {
        /* Allocate up to MIX_N slots */
        for (int i = 0; i < MIX_N; i++) {
            if (live[i]) continue;
            live_sz[i] = (size_t)(8 + ((round * MIX_N + i) * 53) % 512 + 1);
            live[i] = malloc(live_sz[i]);
            if (live[i]) {
                memset(live[i], (unsigned char)(i + 1), live_sz[i]);
                ops++;
            }
        }

        /* Verify all live buffers still hold their pattern */
        for (int i = 0; i < MIX_N; i++) {
            if (!live[i]) continue;
            if (!mem_all_eq(live[i], live_sz[i], (unsigned char)(i + 1)))
                overlap++;
        }

        /* Free even slots */
        for (int i = 0; i < MIX_N; i += 2) {
            free(live[i]);
            live[i] = NULL;
            live_sz[i] = 0;
        }

        /* Realloc odd slots */
        for (int i = 1; i < MIX_N; i += 2) {
            if (!live[i]) continue;
            size_t new_sz = live_sz[i] * 2;
            void *r = realloc(live[i], new_sz);
            if (r) {
                live[i]    = r;
                live_sz[i] = new_sz;
                memset(live[i], (unsigned char)(i + 1), live_sz[i]);
            }
        }
    }

    /* Free everything */
    for (int i = 0; i < MIX_N; i++) { free(live[i]); live[i] = NULL; }

    printf("    Total operations: %d\n", ops);
    CHECK(overlap == 0, "No pattern corruption detected across mixed operations");

    /* After full cleanup, heap should be fully reclaimable */
    void *brk_before = sbrk(0);
    void *probe = malloc(4096);
    void *brk_after  = sbrk(0);
    CHECK(probe != NULL, "Post-mixed malloc(4096) succeeds");
    CHECK(brk_after == brk_before, "Post-mixed malloc(4096) reuses memory");
    free(probe);
#undef MIX_N
}

/* ── Banner art ──────────────────────────────────────────────────────── */

static void print_pass_banner(void) {
    printf("\033[32m\b");
    printf("    ██████████████████████████████████\n");
    printf("    ██                              ██\n");
    printf("    ██  █████    ███    ████   ████ ██\n");
    printf("    ██  █    █  █   █  █      █     ██\n");
    printf("    ██  █    █  █████   ███    ███  ██\n");
    printf("    ██  █████   █   █      █      █ ██\n");
    printf("    ██  █       █   █  ████   ████  ██\n");
    printf("    ██                              ██\n");
    printf("    ██████████████████████████████████\n");
    printf("\033[0m\b");
}

static void print_fail_banner(void) {
    printf("\033[31m\b");
    printf("    ██████████████████████████████████\n");
    printf("    ██                              ██\n");
    printf("    ██     ██████  ███   █  █       ██\n");
    printf("    ██     █      █   █  █  █       ██\n");
    printf("    ██     ████   █████  █  █       ██\n");
    printf("    ██     █      █   █  █  █       ██\n");
    printf("    ██     █      █   █  █  ████    ██\n");
    printf("    ██                              ██\n");
    printf("    ██████████████████████████████████\n");
    printf("\033[0m\b");
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    printf("\033[1m\b\033[97m\b");
    printf("╔══════════════════════════════════════╗\n");
    printf("║   ModuOS Memory Diagnostic Utility   ║\n");
    printf("╚══════════════════════════════════════╝\n");
    printf("\033[0m\b");
    printf("Program break at start: 0x%x\n", (unsigned int)(uintptr_t)sbrk(0));

    test_basic();
    test_pattern();
    test_realloc();
    test_fragmentation();
    test_boundary();
    test_stress();
    test_calloc();
    test_large();
    test_alignment();
    test_mixed();

    /* ── Summary ── */
    printf("\n\033[1m\b\033[97m\b── Summary ──────────────────────────────\033[0m\b\n");
    printf("  Total : %d\n", g_pass + g_fail);
    printf("  \033[32m\bPassed: %d\033[0m\b\n", g_pass);
    if (g_fail > 0)
        printf("  \033[31m\bFailed: %d\033[0m\b\n", g_fail);
    else
        printf("  \033[32m\bFailed: 0\033[0m\b\n");
    printf("Program break at end  : 0x%x\n\n", (unsigned int)(uintptr_t)sbrk(0));

    if (g_fail == 0)
        print_pass_banner();
    else
        print_fail_banner();

    return g_fail > 0 ? 1 : 0;
}