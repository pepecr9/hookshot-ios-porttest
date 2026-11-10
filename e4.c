/* E4: guest address space without one large contiguous reservation. Synthetic only (no game code or data).
   1. Address space: the largest single reservation (binary search) and the total in separate 256 MB reservations.
   2. A 16 GB guest space (as large as the fork's 17.6 GB layout needs, roughly) mapped over separate 256 MB host
      reservations through a segment table, segments made on first use; data written and checked through it.
   3. Cost per access of each translation, on the same 64 MB of data placed at a guest address:
      direct (Hookshot today: base + a), segments of 4 GB / 256 MB / 16 MB (host = table[a >> s] + a), and a two-level
      page table with a 64-entry software TLB (16 KB pages). Workloads: sequential, random, dependent pointer chase.
   Memory: PROT_NONE reservations cost no RAM; at most ~160 MB is touched; guards on os_proc_available_memory. */
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

#define MB (1ull << 20)
#define GB (1ull << 30)
#define SEG_SHIFT 28                    /* 256 MB segments for the mapped guest space */
#define SEG (1ull << SEG_SHIFT)
#define GUEST_SPACE (16 * GB)
#define NSEG (GUEST_SPACE / SEG)        /* 64 */
#define AVAIL_FLOOR (700 * MB)
#define DATA (64 * MB)                  /* benchmark working set */
#define G0 0x12F000000ull               /* its guest address: crosses 256 MB and 16 MB segment boundaries */

static char *o_p; static size_t o_left;
static void say(const char *f, ...) {
    va_list a; va_start(a, f); int n = vsnprintf(o_p, o_left, f, a); va_end(a);
    if (n > 0 && (size_t)n < o_left) { o_p += n; o_left -= n; }
}
static double now_ns(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return (double)mach_absolute_time() * tb.numer / tb.denom; }
static uint64_t avail(void) {
#if TARGET_OS_IPHONE
    return os_proc_available_memory();
#else
    return 0;
#endif
}
static double foot_mb(void) {
    task_vm_info_data_t v; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&v, &n) == KERN_SUCCESS ? v.phys_footprint / (double)MB : 0;
}
static int room(uint64_t bytes) { uint64_t a = avail(); return !a || a > bytes + AVAIL_FLOOR; }
static void *reserve(uint64_t n) { void *p = mmap(NULL, n, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0); return p == MAP_FAILED ? NULL : p; }

/* ---------- 1. address space ---------- */
static void va_probe(void) {
    uint64_t lo = 0, hi = 128;   /* in 256 MB units: 0 .. 32 GB */
    while (lo < hi) { uint64_t mid = (lo + hi + 1) / 2; void *p = reserve(mid * 256 * MB); if (p) { munmap(p, mid * 256 * MB); lo = mid; } else hi = mid - 1; }
    say("[1] largest single reservation: %.2f GB\n", lo * 0.25);
    static void *r[256]; int n = 0;
    while (n < 256 && (r[n] = reserve(256 * MB))) n++;
    say("[1] separate 256 MB reservations: %d (%.2f GB in all%s)\n", n, n * 0.25, n == 256 ? ", the test's cap" : "");
    while (n) munmap(r[--n], 256 * MB);
}

/* ---------- 2. a 16 GB guest space over separate host segments ---------- */
static uintptr_t segtab[NSEG];  /* host = segtab[a >> 28] + a, in integer arithmetic (a pointer below its object is undefined) */
static uint8_t *seghost[NSEG];
static int nseg_made, nseg_refused, phase_verify;   /* phase_verify: a segment made while checking is a bug */
static uint8_t *g2h(uint64_t a) {   /* translation with segments made on first use (a fault handler would do this) */
    uint64_t s = a >> SEG_SHIFT;
    if (s >= NSEG) return NULL;
    if (!seghost[s]) {   /* made or not by the host pointer: the offset is 0 when the system places a segment at its own guest address */
        uint8_t *h = mmap(NULL, SEG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (h == MAP_FAILED) { nseg_refused++; return NULL; }
        seghost[s] = h; segtab[s] = (uintptr_t)h - (uintptr_t)(s * SEG); nseg_made++;
        if (phase_verify) say("    DIAG: segment %llu made during the check (guest %#llx, host %p)\n", (unsigned long long)s, (unsigned long long)a, (void *)h);
    }
    return (uint8_t *)(segtab[s] + (uintptr_t)a);
}
static uint64_t tag(uint64_t a) { uint64_t x = a * 0x9E3779B97F4A7C15ull; return x ^ (x >> 31); }
static int guest_space_test(void) {
    /* 40 guest segments spread over the 16 GB (as an alias region's allocations spread), 2 MB written in each */
    const int want = 40; uint64_t written = 0, bad = 0, lost = 0; double f0 = foot_mb();
    static int pick[NSEG]; for (int i = 0; i < NSEG; i++) pick[i] = i;
    uint64_t rs = 99; for (int i = NSEG - 1; i > 0; i--) { rs = rs * 6364136223846793005ull + 1; int j = (int)(rs >> 33) % (i + 1); int t = pick[i]; pick[i] = pick[j]; pick[j] = t; }
    for (int k = 0; k < want; k++) {
        if (!room(4 * MB)) { say("  GUARD: room to the limit low, stopping at %d segments\n", k); break; }
        uint64_t base = (uint64_t)pick[k] * SEG + 37 * MB;   /* 2 MB inside one segment */
        for (uint64_t o = 0; o < 2 * MB; o += 64) {
            uint8_t *h = g2h(base + o); if (!h) { lost++; break; }
            *(uint64_t *)h = tag(base + o); written += 8;
        }
    }
    int distinct = 0; { char seen[NSEG] = {0}; for (int k = 0; k < want; k++) if (!seen[pick[k]]++) distinct++; }
    say("    write phase: %d segments made for %d distinct picks\n", nseg_made, distinct);
    phase_verify = 1;
    for (int k = 0; k < want; k++) {
        uint64_t base = (uint64_t)pick[k] * SEG + 37 * MB;
        if (!seghost[base >> SEG_SHIFT]) continue;
        for (uint64_t o = 0; o < 2 * MB; o += 64) {
            uint64_t a = base + o, got = *(uint64_t *)g2h(a);
            if (got != tag(a) && bad++ < 8) say("    DIAG: guest %#llx (segment %llu, pick %d) host %p: got %#llx, want %#llx\n", (unsigned long long)a,
                (unsigned long long)(a >> SEG_SHIFT), k, (void *)g2h(a), (unsigned long long)got, (unsigned long long)tag(a));
        }
    }
    /* host contiguity: segments are separate reservations, so a guest range that crosses a segment boundary is not one host range */
    int adjacent = 0; for (int s = 0; s + 1 < NSEG; s++) if (seghost[s] && seghost[s + 1] && seghost[s + 1] == seghost[s] + SEG) adjacent++;
    say("[2] 16 GB guest space in %d segments of 256 MB: %d made on first use, %d refused by the system, %.0f MB of host address space in use\n",
        (int)NSEG, nseg_made, nseg_refused, nseg_made * 256.0);
    say("    %llu KB written through the table, %llu bad words, %llu writes without a segment; footprint +%.1f MB; %d guest-adjacent pairs happen to be host-adjacent\n",
        (unsigned long long)(written / 1024), (unsigned long long)bad, (unsigned long long)lost, foot_mb() - f0, adjacent);
    for (int s = 0; s < NSEG; s++) if (seghost[s]) { munmap(seghost[s], SEG); seghost[s] = NULL; segtab[s] = 0; }
    return bad || lost || nseg_made == 0;
}

/* ---------- 3. cost per access ---------- */
static uint8_t *hbuf, *dbase;                       /* dbase = hbuf - G0: direct translation */
static uint8_t *t32[128], *t28[2048], **t24;        /* segment tables (39-bit guest space) */
static uint8_t **l1[8192];                          /* page table: l1[a >> 26] -> l2[(a >> 14) & 4095] -> host page */
static uint64_t tlb_tag[64]; static uint8_t *tlb_host[64]; static uint64_t tlb_miss;
#define T_DIRECT(a) (dbase + (a))
#define T_S32(a) (t32[(a) >> 32] + (a))
#define T_S28(a) (t28[(a) >> 28] + (a))
#define T_S24(a) (t24[(a) >> 24] + (a))
static inline uint8_t *T_PT(uint64_t a) {
    uint64_t pn = a >> 14; unsigned i = (unsigned)pn & 63;
    if (__builtin_expect(tlb_tag[i] == pn, 1)) return tlb_host[i] + (a & 16383);
    tlb_miss++;
    uint8_t *h = l1[a >> 26][(a >> 14) & 4095];
    tlb_tag[i] = pn; tlb_host[i] = h; return h + (a & 16383);
}
static void tables_init(void) {
    for (uint64_t a = G0; a < G0 + DATA; a += 16 * MB) { t32[a >> 32] = dbase; t28[a >> 28] = dbase; t24[a >> 24] = dbase; }
    for (uint64_t a = G0 & ~((1ull << 26) - 1); a < G0 + DATA; a += 1ull << 26) {
        uint8_t **l2 = calloc(4096, sizeof *l2); l1[a >> 26] = l2;
        for (unsigned j = 0; j < 4096; j++) { uint64_t p = (a & ~((1ull << 26) - 1)) + ((uint64_t)j << 14); if (p >= G0 && p < G0 + DATA) l2[j] = hbuf + (p - G0); }
    }
    memset(tlb_tag, 0xff, sizeof tlb_tag);
}
/* one benchmark per translation; volatile sink keeps the loads */
static volatile uint64_t sink;
#define BENCH(NAME, T) \
static double NAME##_seq(void) { uint64_t s = 0; double t = now_ns(); \
    for (uint64_t a = G0; a < G0 + DATA; a += 8) s += *(uint64_t *)T(a); sink = s; return (now_ns() - t) / (DATA / 8); } \
static double NAME##_rand(void) { uint64_t s = 0, x = 12345; double t = now_ns(); const uint64_t n = DATA / 8; \
    for (uint64_t i = 0; i < n; i++) { x = x * 6364136223846793005ull + 1442695040888963407ull; uint64_t a = G0 + ((x >> 20) % (DATA / 8)) * 8; s += *(uint64_t *)T(a); } \
    sink = s; return (now_ns() - t) / n; } \
static double NAME##_chase(void) { uint64_t a = G0; double t = now_ns(); const uint64_t n = DATA / 64 * 2; \
    for (uint64_t i = 0; i < n; i++) a = *(uint64_t *)T(a); sink = a; return (now_ns() - t) / n; }
BENCH(direct, T_DIRECT)
BENCH(seg32, T_S32)
BENCH(seg28, T_S28)
BENCH(seg24, T_S24)
BENCH(ptab, T_PT)
static double best3(double (*f)(void)) { double b = 1e30; for (int i = 0; i < 3; i++) { double v = f(); if (v < b) b = v; } return b; }

int e4_all(char *buf, unsigned long cap, int unused_states, int unused_touch) {
    (void)unused_states; (void)unused_touch;
    o_p = buf; o_left = cap; buf[0] = 0;
    int bad = 0;
    say("E4 guest address space without one large reservation, %s, page %d bytes; footprint %.1f MB, avail %.0f MB\n",
        TARGET_OS_IPHONE ? "iOS" : "macOS", getpagesize(), foot_mb(), avail() / (double)MB);
    va_probe();
    bad |= guest_space_test();

    if (!room(DATA + 32 * MB)) { say("[3] skipped: not enough room for the 64 MB working set\n"); goto out; }
    hbuf = mmap(NULL, DATA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    t24 = calloc(1u << 15, sizeof *t24);
    if (hbuf == MAP_FAILED || !t24) { say("[3] allocation failed\n"); bad = 1; goto out; }
    dbase = hbuf - G0; tables_init();
    {   /* fill: every 64-byte line holds the guest address of the next line of one random cycle (for the chase) */
        const uint64_t lines = DATA / 64; uint32_t *perm = malloc(lines * 4);
        for (uint64_t i = 0; i < lines; i++) perm[i] = (uint32_t)i;
        uint64_t x = 7; for (uint64_t i = lines - 1; i > 0; i--) { x = x * 6364136223846793005ull + 1; uint64_t j = (x >> 20) % (i + 1); uint32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
        for (uint64_t i = 0; i < lines; i++) *(uint64_t *)(hbuf + (uint64_t)perm[i] * 64) = G0 + (uint64_t)perm[(i + 1) % lines] * 64;
        free(perm);
        /* the chase starts at G0: make sure G0 is on the cycle (every line is) */
    }
    /* correctness of each translation: all must give the same host address */
    for (uint64_t a = G0; a < G0 + DATA; a += 4096 + 8) {
        uint8_t *h = T_DIRECT(a);
        if (T_S32(a) != h || T_S28(a) != h || T_S24(a) != h || T_PT(a) != h) { say("[3] translation mismatch at %#llx\n", (unsigned long long)a); bad = 1; break; }
    }
    say("[3] ns per 8-byte load on 64 MB at guest %#llx (best of 3), and overhead against direct:\n", (unsigned long long)G0);
    say("    %-28s %10s %10s %10s\n", "translation", "sequential", "random", "chase");
    struct { const char *n; double (*s)(void), (*r)(void), (*c)(void); } B[] = {
        {"direct (base + a, Hookshot)", direct_seq, direct_rand, direct_chase},
        {"segments of 4 GB (128)", seg32_seq, seg32_rand, seg32_chase},
        {"segments of 256 MB (2048)", seg28_seq, seg28_rand, seg28_chase},
        {"segments of 16 MB (32768)", seg24_seq, seg24_rand, seg24_chase},
        {"page table + 64-entry TLB", ptab_seq, ptab_rand, ptab_chase}};
    double d[3] = {0};
    for (int i = 0; i < 5; i++) {
        tlb_miss = 0;
        double s = best3(B[i].s), r = best3(B[i].r), c = best3(B[i].c);
        if (i == 0) { d[0] = s; d[1] = r; d[2] = c; }
        say("    %-28s %7.3f ns %7.3f ns %7.3f ns", B[i].n, s, r, c);
        if (i) say("   (%+.0f%% / %+.0f%% / %+.0f%%)", 100 * (s / d[0] - 1), 100 * (r / d[1] - 1), 100 * (c / d[2] - 1));
        if (i == 4) say("   TLB misses %llu", (unsigned long long)tlb_miss);
        say("\n");
    }
    say("    (the sequential loop may let the compiler keep a segment base in a register; lifted code recomputes it per access)\n");
    munmap(hbuf, DATA); free(t24);
    for (int i = 0; i < 8192; i++) if (l1[i]) { free(l1[i]); l1[i] = NULL; }
out:
    say("footprint at the end %.1f MB, avail %.0f MB\n", foot_mb(), avail() / (double)MB);
    say("E4 RESULT: %s\n", bad ? "ERRORS (see above)" : "translations agree, data intact");
    return bad;
}
