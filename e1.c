/* E1: file-backed guest memory on iOS, as NXNative's DM_GUEST_FILE does it (dm/host/hle_svc.c:60-189):
   sparse files we create, opened O_EXCL, unlinked at once, protection class C, sized with ftruncate and mapped
   MAP_SHARED | MAP_FIXED over one PROT_NONE reservation. No game files.
   Measures footprint / internal / compressed / external / resident, os_proc_available_memory, page-ins, disk use,
   throughput and stalls for working sets of 128 / 256 / 512 MB (sequential and random, data verified), then a
   guarded "spill" past what the footprint could hold as anonymous memory, then address-space probes for a segmented
   replacement of Hookshot's fixed window. Safety: every step checks the guards below and stops early. */
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
int open_dprotected_np(const char *path, int flags, int cls, int dpflags, ...);   /* as hle_svc.c uses it */
#endif
int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer);   /* libproc.h, not in the iOS SDK (as hle_svc.c) */

#define MB (1ull << 20)
#define GB (1ull << 30)
#define LOGICAL (4 * GB)            /* the file-backed window: 4 GB (the iPhone's largest reservation was 6 GB) */
#define CHUNK (1 * GB)              /* one file a GB, as DM_GUEST_FILE_CHUNK's default */
#define NFILES (LOGICAL / CHUNK)
#define GUARD_AVAIL (600 * MB)      /* stop when os_proc_available_memory() falls under this */
#define GUARD_FOOT (256 * MB)       /* stop when the footprint grows more than this over the start */
#define GUARD_DISK (3 * GB)         /* stop when the disk's free space falls under this */
#define SPILL_MAX (2560 * MB)       /* the spill phase writes at most this much file-backed data */

static char *o_p; static size_t o_left;
static void say(const char *f, ...) {
    va_list a; va_start(a, f);
    int n = vsnprintf(o_p, o_left, f, a);
    va_end(a);
    if (n > 0 && (size_t)n < o_left) { o_p += n; o_left -= n; }
}

typedef struct { uint64_t foot, internal, compressed, external, resident; } Vm;
static Vm vm(void) {
    task_vm_info_data_t v; mach_msg_type_number_t n = TASK_VM_INFO_COUNT; Vm r = {0};
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&v, &n) == KERN_SUCCESS)
        r = (Vm){v.phys_footprint, v.internal, v.compressed, v.external, v.resident_size};
    return r;
}
static uint64_t avail(void) {
#if TARGET_OS_IPHONE
    return os_proc_available_memory();
#else
    return 0;   /* (macOS: no per-app limit) */
#endif
}
static double now_s(void) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}
static int fds[NFILES];
static double disk_used_mb(void) {
    double b = 0; struct stat st;
    for (int i = 0; i < (int)NFILES; i++) if (fds[i] >= 0 && !fstat(fds[i], &st)) b += st.st_blocks * 512.0;
    return b / MB;
}
static double disk_free_gb(void) { struct statfs fs; return fds[0] >= 0 && !fstatfs(fds[0], &fs) ? (double)fs.f_bavail * fs.f_bsize / GB : -1; }
static struct rusage_info_v4 ru0;
static void ru_get(struct rusage_info_v4 *r) { if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)r)) memset(r, 0, sizeof *r); }

static Vm base;
static void row(const char *what, double mbs, double worst_ms) {
    Vm v = vm(); struct rusage_info_v4 r; ru_get(&r);
    say("%-26s foot %5.0f int %5.0f comp %4.0f ext %5.0f res %5.0f | avail %5.0f | pagein %6llu rd %6.0f wr %6.0f MB | disk %5.0f MB",
        what, v.foot / (double)MB, v.internal / (double)MB, v.compressed / (double)MB, v.external / (double)MB,
        v.resident / (double)MB, avail() / (double)MB, (unsigned long long)(r.ri_pageins - ru0.ri_pageins),
        (r.ri_diskio_bytesread - ru0.ri_diskio_bytesread) / (double)MB, (r.ri_diskio_byteswritten - ru0.ri_diskio_byteswritten) / (double)MB,
        disk_used_mb());
    if (mbs >= 0) say(" | %7.0f MB/s, worst step %6.1f ms", mbs, worst_ms);
    say("\n");
}
/* 0 = fine, otherwise the reason printed */
static int guard(void) {
    Vm v = vm(); uint64_t a = avail(); double df = disk_free_gb();
    if (a && a < GUARD_AVAIL) { say("GUARD: available %llu MB < %llu: stopping\n", (unsigned long long)(a / MB), (unsigned long long)(GUARD_AVAIL / MB)); return 1; }
    if (v.foot > base.foot + GUARD_FOOT) { say("GUARD: footprint grew %llu MB: stopping\n", (unsigned long long)((v.foot - base.foot) / MB)); return 1; }
    if (df >= 0 && df * GB < GUARD_DISK) { say("GUARD: %.1f GB free on disk: stopping\n", df); return 1; }
    return 0;
}

static uint64_t tag(uint64_t off) { uint64_t x = off * 0x9E3779B97F4A7C15ull; return x ^ (x >> 29); }   /* the value stored at each 8-byte offset */

/* pages [first, first + bytes) of the window; returns MB/s, worst 4 MB step in ms, and counts bad words */
static double pass_seq(uint8_t *w, uint64_t first, uint64_t bytes, int write, uint64_t *bad, double *worst) {
    const uint64_t pg = (uint64_t)getpagesize(), step = 4 * MB;
    double t0 = now_s(); *worst = 0;
    for (uint64_t s = 0; s < bytes; s += step) {
        double ts = now_s();
        for (uint64_t o = first + s; o < first + s + step && o < first + bytes; o += pg) {
            volatile uint64_t *p = (volatile uint64_t *)(w + o);
            for (int k = 0; k < 8; k++) {   /* 64 bytes a page: enough to dirty or read it */
                uint64_t off = o + 8 * (uint64_t)k;
                if (write) p[k] = tag(off); else if (p[k] != tag(off)) (*bad)++;
            }
        }
        double d = (now_s() - ts) * 1000; if (d > *worst) *worst = d;
    }
    double t = now_s() - t0;
    return t > 0 ? bytes / (double)MB / t : 0;
}
static uint64_t rs = 0x2545F4914F6CDD1Dull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
/* random pages of [first, first + bytes): read-check-rewrite one word each, n accesses */
static double pass_rand(uint8_t *w, uint64_t first, uint64_t bytes, uint64_t n, uint64_t *bad, double *worst) {
    const uint64_t pg = (uint64_t)getpagesize(), pages = bytes / pg;
    double t0 = now_s(), ts = t0; *worst = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t off = first + (rnd() % pages) * pg + 8 * (rnd() % 8);
        volatile uint64_t *p = (volatile uint64_t *)(w + off);
        if (*p != tag(off)) (*bad)++;
        *p = tag(off);
        if ((i & 1023) == 1023) { double t = now_s(); if ((t - ts) * 1000 > *worst) *worst = (t - ts) * 1000; ts = t; }
    }
    double t = now_s() - t0;
    return t > 0 ? n * 64.0 / MB / t : 0;   /* (64 bytes of useful traffic an access) */
}

static void vaprobes(void) {
    /* total address space in 1 GB reservations (a segmented layout reserves only the segments it uses) */
    void *r[64]; int n = 0;
    while (n < 64 && (r[n] = mmap(NULL, GB, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0)) != MAP_FAILED) n++;
    say("va: %d separate 1 GB reservations (%d GB in all, the window above included: 4 GB)\n", n, n);
    while (n) munmap(r[--n], GB);
    /* fixed 4 GB bases: could a smaller Hookshot window sit at a constant address? */
    static const uint64_t bases[] = {0x200000000ull, 0x300000000ull, 0x400000000ull, 0x800000000ull, 0x1000000000ull, 0x2000000000ull, 0x4000000000ull};
    for (unsigned i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        void *p = mmap((void *)bases[i], 4 * GB, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        say("va: 4 GB at %#llx: %s\n", (unsigned long long)bases[i],
            p == MAP_FAILED ? "no" : p == (void *)bases[i] ? "yes, at that address" : "only elsewhere");
        if (p != MAP_FAILED) munmap(p, 4 * GB);
    }
}

int e1_all(char *buf, unsigned long cap, int unused_states, int unused_touch) {
    (void)unused_states; (void)unused_touch;
    o_p = buf; o_left = cap; buf[0] = 0;
    for (int i = 0; i < (int)NFILES; i++) fds[i] = -1;
    say("E1 file-backed memory (NXNative DM_GUEST_FILE pattern), %s, page %d bytes\n", TARGET_OS_IPHONE ? "iOS" : "macOS", getpagesize());
    char dir[1024] = "";
    if (!confstr(_CS_DARWIN_USER_TEMP_DIR, dir, sizeof dir)) { say("no temporary folder (errno %d)\n", errno); return 1; }
    ru_get(&ru0); base = vm();
    row("start", -1, 0);

    uint8_t *w = mmap(NULL, LOGICAL, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (w == MAP_FAILED) { say("reserve 4 GB: FAILED errno %d\n", errno); return 1; }
    say("reserved %llu GB at %p\n", (unsigned long long)(LOGICAL / GB), w);
    int bad_setup = 0;
    for (int i = 0; i < (int)NFILES && !bad_setup; i++) {
        char path[1200]; snprintf(path, sizeof path, "%s/e1-%d-%d.mem", dir, (int)getpid(), i);
#if TARGET_OS_IPHONE
        fds[i] = open_dprotected_np(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 3, 0, 0600);   /* class C */
        if (fds[i] < 0) fds[i] = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
#else
        fds[i] = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
#endif
        if (fds[i] < 0) { say("open %s: errno %d\n", path, errno); bad_setup = 1; break; }
        unlink(path);   /* nothing is left behind, whatever happens */
        if (i == 0 && disk_free_gb() * GB < GUARD_DISK + 1 * GB) { say("only %.1f GB free on disk: not starting\n", disk_free_gb()); bad_setup = 1; break; }
        if (ftruncate(fds[i], (off_t)CHUNK)) { say("ftruncate: errno %d\n", errno); bad_setup = 1; break; }
        if (mmap(w + (uint64_t)i * CHUNK, CHUNK, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fds[i], 0) != w + (uint64_t)i * CHUNK) {
            say("map file %d: errno %d\n", i, errno); bad_setup = 1; break;
        }
    }
    if (bad_setup) { munmap(w, LOGICAL); for (int i = 0; i < (int)NFILES; i++) if (fds[i] >= 0) close(fds[i]); return 1; }
    say("%d sparse files of 1 GB (unlinked), mapped shared over it; %.1f GB free on their disk\n", (int)NFILES, disk_free_gb());
    row("files mapped", -1, 0);

    uint64_t bad = 0; int stopped = 0;
    static const uint64_t sets[] = {128 * MB, 256 * MB, 512 * MB};
    for (unsigned s = 0; s < 3 && !stopped; s++) {
        uint64_t W = sets[s], first = (uint64_t)s * GB;   /* each set in its own file, so each starts cold */
        char lab[64]; double worst, r;
        snprintf(lab, sizeof lab, "W=%llu seq write", (unsigned long long)(W / MB)); r = pass_seq(w, first, W, 1, &bad, &worst); row(lab, r, worst);
        if ((stopped = guard())) break;
        snprintf(lab, sizeof lab, "W=%llu seq read", (unsigned long long)(W / MB)); r = pass_seq(w, first, W, 0, &bad, &worst); row(lab, r, worst);
        snprintf(lab, sizeof lab, "W=%llu random x%llu", (unsigned long long)(W / MB), (unsigned long long)(W / getpagesize() * 4));
        r = pass_rand(w, first, W, W / getpagesize() * 4, &bad, &worst); row(lab, r, worst);
        if ((stopped = guard())) break;
    }
    say("working sets: %llu bad words\n", (unsigned long long)bad);

    /* spill: write file-backed data past the room the footprint has (as anonymous memory it would end the app), 128 MB a step */
    uint64_t room = avail(), spilled = 0, sbad = 0; double worst_all = 0, t0 = now_s();
    uint64_t target = room ? room + 512 * MB : 2 * GB;   /* on the iPhone: 512 MB past the footprint's room */
    if (target > SPILL_MAX) target = SPILL_MAX;
    say("spill: target %llu MB of file-backed data (room to the limit %llu MB)\n", (unsigned long long)(target / MB), (unsigned long long)(room / MB));
    while (!stopped && spilled < target) {
        double worst; uint64_t step = 128 * MB;
        pass_seq(w + 0, spilled, step, 1, &sbad, &worst);   /* from the window's start: overwrites the sets above, all one dataset */
        spilled += step; if (worst > worst_all) worst_all = worst;
        if ((spilled % (512 * MB)) == 0) { char lab[64]; snprintf(lab, sizeof lab, "spill %llu MB", (unsigned long long)(spilled / MB)); row(lab, -1, 0); }
        stopped = guard();
    }
    double ts = now_s() - t0;
    row("spill done", ts > 0 ? spilled / (double)MB / ts : 0, worst_all);
    /* read it all back: what was written out comes back through page-ins */
    double worst; uint64_t rbad = 0;
    double r = pass_seq(w, 0, spilled, 0, &rbad, &worst);
    row("spill read back", r, worst);
    say("spill: %llu MB written, %llu bad words on read-back%s\n", (unsigned long long)(spilled / MB), (unsigned long long)rbad, stopped ? " (stopped by a guard)" : "");

    munmap(w, LOGICAL);
    for (int i = 0; i < (int)NFILES; i++) if (fds[i] >= 0) close(fds[i]);
    row("released", -1, 0);
    vaprobes();
    int fail = bad || rbad;
    say("E1 RESULT: %s\n", fail ? "DATA ERRORS" : "data intact");
    return fail;
}
