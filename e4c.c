/* E4-C: a 17.6 GB guest address space (the fork's mem_reserve 0x468000000) in 2 MB chunks made on first use, never
   reserved whole on the host. Synthetic only (no game code or data). Design: E4C_DISENO.md.
   A. Address-space exhaustion or fragmentation: total reservable in 1 GB / 256 MB / 16 MB / 2 MB pieces, region map.
   B. Chunks made on first use from one pre-reserved arena: clustered and scattered writes, chunk-boundary writes, zero check.
   C. Aliases: alias-region chunks are table entries pointing at heap chunks (no host address space of their own).
   D. Recovery under a host budget: file-backed chunks in 512 MB of host addresses, evicted and remapped, data checked.
   B-D run ROUNDS times with different seeds (intermittent faults). E. Cost per access, rotated order, median of 9.
   Memory: at most ~40 MB touched in B-D (one or two 16 KB pages per chunk), 64 MB + 4 MB in E; stops below 700 MB of room.
   Disk (D only): one unlinked sparse file, about 18 MB of pages written, needs 2 GB + 64 MB free or D is skipped. */
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <unistd.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

#define MB (1ull << 20)
#define GB (1ull << 30)
#define CSHIFT 21
#define CHUNK (1ull << CSHIFT)                  /* 2 MB */
#define PAGE16 16384ull
#define GUEST_SPACE 0x468000000ull              /* 17.5625 GB */
#define NCHUNK (GUEST_SPACE >> CSHIFT)          /* 9024 */
#define HEAP_END (25 * GB / 4)                  /* heap  [0, 6.25 GB) */
#define ALIAS_END (HEAP_END + 8 * GB)           /* alias [6.25, 14.25 GB): views of the heap; app pool above, to 17.5625 GB */
#define HEAP_N (HEAP_END >> CSHIFT)             /* 3200 */
#define ALIAS_N ((ALIAS_END - HEAP_END) >> CSHIFT)
#define ARENA (3 * GB)                          /* host addresses for B and C, reserved once */
#define DSLOTS ((512 * MB) >> CSHIFT)           /* D: 256 host slots */
#define DCHUNKS 1100                            /* D: file capacity in chunks */
#define AVAIL_FLOOR (700 * MB)
#define DISK_NEED (64 * MB)
#define DISK_MARGIN (2 * GB)
#define ROUNDS 5
#define WORDS 256                               /* 2 KB of tags per written block */

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
static uint64_t rng(uint64_t *s) { *s = *s * 6364136223846793005ull + 1442695040888963407ull; return *s >> 17; }
static uint64_t tag(uint64_t a, uint64_t seed) { uint64_t x = (a ^ (seed << 48)) * 0x9E3779B97F4A7C15ull; return (x ^ (x >> 31)) | 1; }  /* never 0 */

/* ---------- A. exhaustion or fragmentation ---------- */
static void part_a(void) {
    uint64_t lo = 0, hi = 128;   /* 256 MB units */
    while (lo < hi) { uint64_t mid = (lo + hi + 1) / 2; void *p = reserve(mid * 256 * MB); if (p) { munmap(p, mid * 256 * MB); lo = mid; } else hi = mid - 1; }
    say("[A] largest single reservation: %.2f GB\n", lo * 0.25);
    static void *r[8192]; const uint64_t sz[4] = {GB, 256 * MB, 16 * MB, 2 * MB}; double tot[4]; int capped = 0;
    for (int k = 0; k < 4; k++) {
        int n = 0; while (n < 8192 && (r[n] = reserve(sz[k]))) n++; capped |= n == 8192;
        tot[k] = n * (double)sz[k] / GB; say("[A] total in %4llu MB pieces: %5d pieces, %.2f GB%s\n", (unsigned long long)(sz[k] / MB), n, tot[k], n == 8192 ? " (cap)" : "");
        while (n) munmap(r[--n], sz[k]);
    }
    double mx = tot[0], mn = tot[0]; for (int k = 1; k < 4; k++) { if (tot[k] > mx) mx = tot[k]; if (tot[k] < mn) mn = tot[k]; }
    if (capped) say("[A] H1 not decidable here: the test's cap of 8192 pieces was reached (address space larger than the test looks)\n"); else
    say("[A] H1 %s: smallest pieces give %.0f%% more than 1 GB pieces (max/min %.3f)\n",
        mx <= mn * 1.05 ? "holds (exhaustion: the same total at every size)" : "does NOT hold (fragmentation: small pieces find more)", 100 * (tot[3] / tot[0] - 1), mx / mn);
    task_vm_info_data_t v; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&v, &n) != KERN_SUCCESS || n < TASK_VM_INFO_REV2_COUNT) { say("[A] no task address range\n"); return; }
    vm_address_t addr = (vm_address_t)v.min_address, prev = addr; uint64_t mapped = 0, gaps = 0, big = 0; int regions = 0;
    for (;;) {
        vm_size_t size = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) break;
        if (addr >= v.max_address) break;
        if (addr > prev) { gaps += addr - prev; if (addr - prev > big) big = addr - prev; }
        mapped += size; regions++; prev = addr + size; addr += size;
    }
    if (v.max_address > prev) { gaps += v.max_address - prev; if (v.max_address - prev > big) big = v.max_address - prev; }
    say("[A] task range %#llx - %#llx (%.2f GB): %d regions, %.2f GB mapped, %.2f GB free in gaps, largest gap %.2f GB\n",
        (unsigned long long)v.min_address, (unsigned long long)v.max_address, (v.max_address - v.min_address) / (double)GB, regions, mapped / (double)GB, gaps / (double)GB, big / (double)GB);
}

/* ---------- the chunk table (B, C, D) ---------- */
static uint8_t *chost[NCHUNK];      /* host base of each guest chunk; NULL = not made: the only presence test */
static int filemode;                /* 0: anonymous chunks from the arena (B, C); 1: file-backed slots with eviction (D) */
static uint8_t *arena; static uint32_t arena_next; static int made, refused, aliases, diag, verifying;
static uint8_t *dslots; static int dfd = -1, slot_owner[DSLOTS], victim; static int32_t foff[NCHUNK]; static int fnext;
static int evictions; static double remap_ns, remap_max;

static int is_alias(uint64_t c) { return c >= HEAP_N && c < HEAP_N + ALIAS_N; }
static uint8_t *make_anon(uint64_t c) {
    if (arena_next >= ARENA / CHUNK) { refused++; return NULL; }
    uint8_t *h = arena + (uint64_t)arena_next * CHUNK;
    if (mprotect(h, CHUNK, PROT_READ | PROT_WRITE)) { refused++; return NULL; }
    arena_next++; made++; return chost[c] = h;
}
static uint8_t *make_file(uint64_t c) {
    if (foff[c] < 0) { if (fnext >= DCHUNKS) { refused++; return NULL; } foff[c] = fnext++; }
    int s = victim; victim = (victim + 1) % DSLOTS;     /* shortcut: FIFO eviction, not LRU; enough to test recovery */
    if (slot_owner[s] >= 0) { chost[slot_owner[s]] = NULL; evictions++; }
    double t = now_ns();
    void *p = mmap(dslots + (uint64_t)s * CHUNK, CHUNK, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, dfd, (off_t)foff[c] * (off_t)CHUNK);
    t = now_ns() - t; remap_ns += t; if (t > remap_max) remap_max = t;
    if (p == MAP_FAILED) { slot_owner[s] = -1; refused++; return NULL; }
    slot_owner[s] = (int)c; made++; return chost[c] = p;
}
static uint8_t *g2h(uint64_t a) {
    uint64_t c = a >> CSHIFT;
    if (c >= NCHUNK) return NULL;
    uint8_t *h = chost[c];
    if (!h) {
        if (filemode) h = make_file(c);
        else {
            if (verifying && diag++ < 8) say("    DIAG: chunk %llu made during a check (guest %#llx)\n", (unsigned long long)c, (unsigned long long)a);
            if (is_alias(c)) { uint64_t hc = (c - HEAP_N) % HEAP_N; h = chost[hc] ? chost[hc] : make_anon(hc); if (h) { chost[c] = h; aliases++; } }
            else h = make_anon(c);
        }
        if (!h) return NULL;
    }
    return h + (a & (CHUNK - 1));
}
static int guest_copy(uint64_t a, void *buf, size_t len, int write) {   /* split at chunk boundaries: chunks are not host-contiguous */
    uint8_t *b = buf;
    while (len) {
        uint8_t *h = g2h(a); if (!h) return -1;
        size_t n = CHUNK - (a & (CHUNK - 1)); if (n > len) n = len;
        if (write) memcpy(h, b, n); else memcpy(b, h, n);
        a += n; b += n; len -= n;
    }
    return 0;
}
static void reset_table(void) {
    memset(chost, 0, sizeof chost); made = refused = aliases = diag = verifying = 0; arena_next = 0;
    evictions = 0; remap_ns = remap_max = 0; fnext = 0; victim = 0;
    memset(foff, 0xff, sizeof foff); for (int i = 0; i < DSLOTS; i++) slot_owner[i] = -1;
}

/* one written block: WORDS tags at guest a; returns words written */
static uint64_t write_block(uint64_t a, uint64_t seed, uint64_t *lost) {
    for (int i = 0; i < WORDS; i++) { uint8_t *h = g2h(a + 8ull * i); if (!h) { (*lost)++; return i; } *(uint64_t *)h = tag(a + 8ull * i, seed); }
    return WORDS;
}
static uint64_t check_block(uint64_t a, uint64_t seed, uint64_t *lost) {   /* returns bad words */
    uint64_t bad = 0;
    for (int i = 0; i < WORDS; i++) {
        uint8_t *h = g2h(a + 8ull * i); if (!h) { (*lost)++; return bad; }
        uint64_t got = *(uint64_t *)h, want = tag(a + 8ull * i, seed);
        if (got != want && bad++ < 2) say("    DIAG: guest %#llx (chunk %llu): got %#llx, want %#llx\n", (unsigned long long)(a + 8ull * i),
                                          (unsigned long long)((a + 8ull * i) >> CSHIFT), (unsigned long long)got, (unsigned long long)want);
    }
    return bad;
}
/* block offset inside a chunk: one of pages 1..126 except 64 (page 64: aliases; pages 0 and 127: boundary writes) */
static uint64_t block_off(uint64_t c, uint64_t seed) { uint64_t p = 1 + tag(c, seed) % 125; if (p >= 64) p++; return p * PAGE16 + 1024; }

/* ---------- B + C: one round ---------- */
static uint64_t blk[2048]; static int nblk;
static int round_bc(int r, uint64_t seed) {
    reset_table(); filemode = 0;
    uint64_t rs = seed, lost = 0, bad = 0, written = 0; nblk = 0; double f0 = foot_mb();
    static uint8_t picked[NCHUNK]; memset(picked, 0, sizeof picked);
    /* clustered: the heap from the bottom, as a bump allocator would use it */
    for (uint64_t c = 0; c < 200; c++) { picked[c] = 1; blk[nblk++] = c * CHUNK + block_off(c, seed); }
    /* scattered: 1000 chunks over the heap and the app pool (not the alias range) */
    while (nblk < 1200) {
        uint64_t c = rng(&rs) % NCHUNK; if (is_alias(c) || picked[c] || c + 1 >= NCHUNK || is_alias(c + 1)) continue;
        picked[c] = 1; blk[nblk++] = c * CHUNK + block_off(c, seed);
    }
    for (int i = 0; i < nblk; i++) { if (!room(4 * MB)) { say("  GUARD: room low, round %d stopped\n", r); return 1; } written += 8 * write_block(blk[i], seed, &lost); }
    /* chunk-boundary writes: 16 bytes starting 8 bytes before the end of 64 scattered chunks (the next chunk is made too) */
    int straddles = 0, host_adjacent = 0, nb0 = nblk; uint64_t sbad = 0;
    for (int i = 200; i < nb0 && straddles < 64; i += 15) {
        uint64_t c = blk[i] >> CSHIFT, a = (c + 1) * CHUNK - 8, w[2] = {tag(a, seed), tag(a + 8, seed)};
        if (guest_copy(a, w, 16, 1)) { lost++; continue; }
        if (chost[c] && chost[c + 1] && chost[c + 1] == chost[c] + CHUNK) host_adjacent++;
        straddles++;
    }
    /* zero check: the word before every block was never written */
    uint64_t nonzero = 0;
    verifying = 1;
    for (int i = 0; i < nb0; i++) { uint8_t *h = g2h(blk[i] - 8); if (h && *(uint64_t *)h) nonzero++; }
    for (int i = 0; i < nb0; i++) bad += check_block(blk[i], seed, &lost);
    for (int i = 200, s = 0; i < nb0 && s < straddles; i += 15, s++) {
        uint64_t c = blk[i] >> CSHIFT, a = (c + 1) * CHUNK - 8, w[2] = {0, 0};
        if (guest_copy(a, w, 16, 0) || w[0] != tag(a, seed) || w[1] != tag(a + 8, seed)) sbad++;
        if (*(uint64_t *)g2h(a) != tag(a, seed) || *(uint64_t *)g2h(a + 8) != tag(a + 8, seed)) sbad++;   /* each half through its own chunk */
    }
    verifying = 0;
    int made_b = made; uint64_t va_b = (uint64_t)arena_next * CHUNK;
    /* what the same touched chunks would cost with E4's 256 MB segments and with 16 MB segments */
    static uint8_t s28[(GUEST_SPACE >> 28) + 1], s24[(GUEST_SPACE >> 24) + 1]; memset(s28, 0, sizeof s28); memset(s24, 0, sizeof s24);
    int n28 = 0, n24 = 0; for (uint64_t c = 0; c < NCHUNK; c++) if (chost[c]) { if (!s28[c >> 7]++) n28++; if (!s24[c >> 3]++) n24++; }

    /* C: 100 alias chunks, each over a distinct heap chunk; half written through the alias, half through the heap */
    static uint8_t hused[HEAP_N]; memset(hused, 0, sizeof hused);
    int na = 0, made_before = made; uint64_t abad = 0, alost = 0; uint64_t aw[100], ar[100];
    while (na < 100) {
        uint64_t k = rng(&rs) % ALIAS_N, c = HEAP_N + k, hc = k % HEAP_N; if (hused[hc]) continue; hused[hc] = 1;
        uint64_t ga = c * CHUNK + 64 * PAGE16 + 512, gh = hc * CHUNK + 64 * PAGE16 + 512;
        aw[na] = (na & 1) ? gh : ga; ar[na] = (na & 1) ? ga : gh; na++;
    }
    int heap_made = 0;
    for (int i = 0; i < na; i++) {
        uint64_t hc = (ar[i] < HEAP_END ? ar[i] : aw[i]) >> CSHIFT; if (!chost[hc]) heap_made++;
        for (int j = 0; j < WORDS; j++) { uint8_t *h = g2h(aw[i] + 8ull * j); if (!h) { alost++; break; } *(uint64_t *)h = tag(aw[i] + 8ull * j, seed ^ 0xA11A5); }
    }
    for (int i = 0; i < na; i++)
        for (int j = 0; j < WORDS; j++) { uint8_t *h = g2h(ar[i] + 8ull * j); if (!h) { alost++; break; } if (*(uint64_t *)h != tag(aw[i] + 8ull * j, seed ^ 0xA11A5)) abad++; }
    int alias_own = made - made_before - heap_made;   /* chunks made for alias entries themselves: must be 0 */
    /* B's data must be untouched by C */
    verifying = 1; uint64_t bad2 = 0; for (int i = 0; i < nb0; i++) bad2 += check_block(blk[i], seed, &lost); verifying = 0;

    say("  round %d: B %d chunks made, %d refused, %llu KB written, %llu bad, %llu lost, %llu nonzero; %d boundary writes (%d host-adjacent), %llu bad;"
        " host %.0f MB for %.2f GB of guest (256 MB segments would need %.2f GB, 16 MB %.2f GB); C %d aliases, %llu bad, %d own chunks, %d heap chunks made;"
        " B after C %llu bad; footprint +%.1f MB\n", r, made_b, refused, (unsigned long long)(written / 1024), (unsigned long long)bad, (unsigned long long)lost,
        (unsigned long long)nonzero, straddles, host_adjacent, (unsigned long long)sbad, va_b / (double)MB, GUEST_SPACE / (double)GB, n28 * 0.25, n24 / 64.0,
        na, (unsigned long long)abad, alias_own, heap_made, (unsigned long long)bad2, foot_mb() - f0);
    int fail = refused || bad || lost || nonzero || sbad || straddles < 64 || abad || alost || alias_own || bad2 || diag;
    mmap(arena, ARENA, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);   /* drop every page, keep the reservation */
    return fail;
}

/* ---------- D: one round, file-backed chunks under a 512 MB host budget ---------- */
static int round_d(int r, uint64_t seed) {
    reset_table(); filemode = 1;
    if (ftruncate(dfd, 0) || ftruncate(dfd, (off_t)DCHUNKS * (off_t)CHUNK)) { say("  round %d D: ftruncate errno %d\n", r, errno); return 1; }
    uint64_t rs = seed ^ 0xD, lost = 0, bad = 0; static uint64_t dblk[1024]; static uint8_t picked[NCHUNK]; memset(picked, 0, sizeof picked);
    int n = 0; while (n < 1024) { uint64_t c = rng(&rs) % (NCHUNK - 1); if (picked[c]) continue; picked[c] = 1; dblk[n++] = c * CHUNK + block_off(c, seed); }
    for (int i = 0; i < n; i++) write_block(dblk[i], seed, &lost);
    int straddles = 0; uint64_t sbad = 0;
    for (int i = 0; i < n && straddles < 32; i += 31) { uint64_t a = ((dblk[i] >> CSHIFT) + 1) * CHUNK - 8, w[2] = {tag(a, seed), tag(a + 8, seed)}; if (guest_copy(a, w, 16, 1)) lost++; else straddles++; }
    int ev_write = evictions;
    for (int i = n - 1; i >= 0; i -= 2) bad += check_block(dblk[i], seed, &lost);   /* a different order: odd ones backwards, */
    for (int i = 0; i < n; i += 2) bad += check_block(dblk[i], seed, &lost);        /* even ones forwards */
    for (int i = 0, s = 0; i < n && s < straddles; i += 31, s++) { uint64_t a = ((dblk[i] >> CSHIFT) + 1) * CHUNK - 8, w[2] = {0, 0}; if (guest_copy(a, w, 16, 0) || w[0] != tag(a, seed) || w[1] != tag(a + 8, seed)) sbad++; }
    say("  round %d: D %d chunks (%.2f GB of guest) in %d host slots (%llu MB): %d evictions while writing, %d in all, %llu bad, %llu lost, %d boundary writes, %llu bad;"
        " remap %.1f us mean, %.1f us max; footprint %.1f MB\n", r, fnext, fnext * 2.0 / 1024, (int)DSLOTS, (unsigned long long)(DSLOTS * CHUNK / MB), ev_write, evictions,
        (unsigned long long)bad, (unsigned long long)lost, straddles, (unsigned long long)sbad, made ? remap_ns / made / 1000 : 0, remap_max / 1000, foot_mb());
    mmap(dslots, (uint64_t)DSLOTS * CHUNK, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    filemode = 0;
    return refused || bad || lost || sbad || straddles < 32 || evictions == 0;
}
static int open_dfile(void) {
    char dir[1024] = ""; if (!confstr(_CS_DARWIN_USER_TEMP_DIR, dir, sizeof dir)) return -1;
    struct statfs fs; if (statfs(dir, &fs)) return -1;
    double freeb = (double)fs.f_bavail * fs.f_bsize;
    say("[D] %.0f MB free on disk (needs %llu MB + %llu MB margin)\n", freeb / MB, (unsigned long long)(DISK_NEED / MB), (unsigned long long)(DISK_MARGIN / MB));
    if (freeb < DISK_NEED + DISK_MARGIN) return -2;
    char path[1200]; snprintf(path, sizeof path, "%s/e4c-%d.mem", dir, (int)getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) unlink(path);
    return fd;
}

/* ---------- E. cost per access ---------- */
#define DATA (64 * MB)
#define G0 0x12F000000ull                       /* crosses a 256 MB boundary */
static uintptr_t dbase, st28[(GUEST_SPACE >> 28) + 1]; static uint8_t *btab[NCHUNK]; static uint8_t *hbuf;
static volatile uint64_t sink;
#define OPAQUE(x) __asm__ volatile("" : "+r"(x))
static __attribute__((noinline)) uint8_t *slow_make(uint64_t a) { (void)a; abort(); }   /* unreachable: every chunk in range is mapped (checked first) */
#define T_DIRECT(a) ((uint8_t *)(db + (a)))
#define T_SEG(a) ((uint8_t *)(st[(a) >> 28] + (a)))
#define T_CH(a) (bt[(a) >> CSHIFT] + ((a) & (CHUNK - 1)))
#define T_CHK(a) ({ uint8_t *p_ = bt[(a) >> CSHIFT]; if (__builtin_expect(!p_, 0)) p_ = slow_make(a); p_ + ((a) & (CHUNK - 1)); })
#define BENCH(NAME, T) \
static __attribute__((noinline)) double NAME##_chase(uint64_t n) { uintptr_t db = dbase; const uintptr_t *st = st28; uint8_t *const *bt = btab; \
    OPAQUE(db); OPAQUE(st); OPAQUE(bt); uint64_t a = G0; double t = now_ns(); \
    for (uint64_t i = 0; i < n; i++) a = *(uint64_t *)T(a); sink = a; return (now_ns() - t) / n; } \
static __attribute__((noinline)) double NAME##_rand(uint64_t n, uint64_t w) { uintptr_t db = dbase; const uintptr_t *st = st28; uint8_t *const *bt = btab; \
    OPAQUE(db); OPAQUE(st); OPAQUE(bt); uint64_t s = 0, x = 12345, m = w / 8 - 1; double t = now_ns(); \
    for (uint64_t i = 0; i < n; i++) { x = x * 6364136223846793005ull + 1442695040888963407ull; uint64_t a = G0 + ((x >> 20) & m) * 8; s += *(uint64_t *)T(a); } \
    sink = s; return (now_ns() - t) / n; }
BENCH(direct, T_DIRECT)
BENCH(seg256, T_SEG)
BENCH(ch2, T_CH)
BENCH(ch2chk, T_CHK)
static void fill_chain(uint64_t w) {   /* every 64-byte line of the first w bytes holds the guest address of the next line of one random cycle */
    uint64_t lines = w / 64; uint32_t *perm = malloc(lines * 4); if (!perm) return;
    for (uint64_t i = 0; i < lines; i++) perm[i] = (uint32_t)i;
    uint64_t x = 7; for (uint64_t i = lines - 1; i > 0; i--) { uint64_t j = rng(&x) % (i + 1); uint32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
    for (uint64_t i = 0; i < lines; i++) *(uint64_t *)(hbuf + (uint64_t)perm[i] * 64) = G0 + (uint64_t)perm[(i + 1) % lines] * 64;
    /* the chase starts at G0 = line 0, which is on the cycle */
    free(perm);
}
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static int part_e(void) {
    if (!room(DATA + 32 * MB)) { say("[E] skipped: not enough room for 64 MB\n"); return 1; }
    hbuf = mmap(NULL, DATA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (hbuf == MAP_FAILED) { say("[E] allocation failed\n"); return 1; }
    dbase = (uintptr_t)hbuf - G0;
    for (uint64_t a = G0; a < G0 + DATA; a += CHUNK) { st28[a >> 28] = dbase; btab[a >> CSHIFT] = hbuf + (a - G0); }
    int bad = 0; { uintptr_t db = dbase; const uintptr_t *st = st28; uint8_t *const *bt = btab;
        for (uint64_t a = G0; a < G0 + DATA; a += 4096 + 8) if (T_SEG(a) != T_DIRECT(a) || T_CH(a) != T_DIRECT(a) || T_CHK(a) != T_DIRECT(a)) { say("[E] translation mismatch at %#llx\n", (unsigned long long)a); bad = 1; break; } }
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    { double t = now_ns(); uint64_t s = 1; while (now_ns() - t < 300e6) s = s * 3 + 1; sink = s; }   /* 300 ms warm-up */
    const char *names[5] = {"direct (base + a)", "segments 256 MB (E4)", "chunks 2 MB", "chunks 2 MB + presence check", "direct again (noise)"};
    double (*fc[5])(uint64_t) = {direct_chase, seg256_chase, ch2_chase, ch2chk_chase, direct_chase};
    double (*fr[5])(uint64_t, uint64_t) = {direct_rand, seg256_rand, ch2_rand, ch2chk_rand, direct_rand};
    const uint64_t wsz[2] = {1 * MB, DATA};
    for (int w = 0; w < 2; w++) {
        fill_chain(wsz[w]);
        say("[E] working set %llu MB, ns per load, median of 9 rounds [min - max], order rotated each round:\n", (unsigned long long)(wsz[w] / MB));
        say("    %-30s %26s %26s\n", "translation", "chase (latency)", "random (throughput)");
        double c[5][9], r[5][9]; uint64_t nc = wsz[w] == DATA ? 2000000 : 4000000, nr = 4000000;
        for (int k = 0; k < 9; k++) for (int j = 0; j < 5; j++) { int v = (j + k) % 5; c[v][k] = fc[v](nc); r[v][k] = fr[v](nr, wsz[w]); }
        double mc[5], mr[5];
        for (int v = 0; v < 5; v++) {
            qsort(c[v], 9, sizeof(double), cmpd); qsort(r[v], 9, sizeof(double), cmpd); mc[v] = c[v][4]; mr[v] = r[v][4];
            say("    %-30s %7.3f [%7.3f - %7.3f] %7.3f [%7.3f - %7.3f]", names[v], mc[v], c[v][0], c[v][8], mr[v], r[v][0], r[v][8]);
            if (v) say("   %+.1f%% / %+.1f%%", 100 * (mc[v] / mc[0] - 1), 100 * (mr[v] / mr[0] - 1));
            say("\n");
        }
        say("    noise (direct vs direct again): %.1f%% chase, %.1f%% random; differences below that are not measurable\n",
            100 * __builtin_fabs(mc[4] / mc[0] - 1), 100 * __builtin_fabs(mr[4] / mr[0] - 1));
    }
    munmap(hbuf, DATA);
    return bad;
}

int e4c_all(char *buf, unsigned long cap, int unused_states, int unused_touch) {
    (void)unused_states; (void)unused_touch;
    o_p = buf; o_left = cap; buf[0] = 0;
    say("E4-C %.4f GB guest space in 2 MB chunks made on first use, %s, page %d bytes; footprint %.1f MB, avail %.0f MB\n",
        GUEST_SPACE / (double)GB, TARGET_OS_IPHONE ? "iOS" : "macOS", getpagesize(), foot_mb(), avail() / (double)MB);
    say("    layout: heap [0, 6.25 GB), alias [6.25, 14.25 GB) over the heap, app pool [14.25, 17.5625 GB); %llu chunks, table %llu KB\n",
        (unsigned long long)NCHUNK, (unsigned long long)(sizeof chost / 1024));
    part_a();
    int bad = 0, fb = 0, fd = 0, dskip = 0;
    arena = reserve(ARENA);
    if (!arena) { say("[B] arena of %llu GB refused\n", (unsigned long long)(ARENA / GB)); bad = 1; }
    else {
        say("[B+C] %d rounds, host arena %llu GB reserved once (the guest space is never reserved whole):\n", ROUNDS, (unsigned long long)(ARENA / GB));
        for (int r = 1; r <= ROUNDS; r++) fb += round_bc(r, 0x5EED0000ull + r);
        munmap(arena, ARENA);
    }
    dfd = open_dfile();
    if (dfd == -2) { say("[D] SKIPPED: not enough free disk\n"); dskip = 1; }
    else if (dfd < 0) { say("[D] temporary file failed (errno %d)\n", errno); fd = 1; }
    else if (!(dslots = reserve((uint64_t)DSLOTS * CHUNK))) { say("[D] slot reservation refused\n"); fd = 1; }
    else {
        say("[D] %d rounds, 1024 chunks (2 GB of guest) through %d slots (512 MB of host addresses), file-backed:\n", ROUNDS, (int)DSLOTS);
        for (int r = 1; r <= ROUNDS; r++) fd += round_d(r, 0xD5EED000ull + r);
        munmap(dslots, (uint64_t)DSLOTS * CHUNK);
    }
    if (dfd >= 0) close(dfd);
    say("[B+C] rounds failed: %d of %d\n[D] rounds failed: %d of %d%s\n", fb, ROUNDS, fd, ROUNDS, dskip ? " (skipped)" : "");
    int be = part_e();
    bad |= fb || fd || dskip || be;
    say("footprint at the end %.1f MB, avail %.0f MB\n", foot_mb(), avail() / (double)MB);
    say("E4-C RESULT: %s\n", bad ? (dskip && !fb && !be ? "INCOMPLETE (D skipped for disk)" : "ERRORS (see above)") : "17.6 GB guest space in 2 MB chunks: data intact through aliases, chunk boundaries and eviction");
    return bad;
}
