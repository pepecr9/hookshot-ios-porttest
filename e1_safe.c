/* E1-Safe: a reduced, standalone copy of E1 (e1.c is unchanged). Same NXNative DM_GUEST_FILE mechanism
   (dm/host/hle_svc.c:60-189): sparse files we create, O_EXCL, unlinked at once, protection class C, sized with
   ftruncate, mapped MAP_SHARED | MAP_FIXED over one 4 GB PROT_NONE reservation. No game files.
   Working sets 128 and 256 MB only (384 MB written, hard cap 512 MB), no spill, no address-space probes.
   Measures footprint / internal / compressed / external / resident, os_proc_available_memory, page-ins and disk
   reads/writes; checks every written word, again after msync forces the data to disk. */
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
int open_dprotected_np(const char *path, int flags, int cls, int dpflags, ...);
#endif
int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer);

#define MB (1ull << 20)
#define GB (1ull << 30)
#define LOGICAL (4 * GB)            /* logical reservation: 4 GB (the iPhone 13's largest was 6 GB) */
#define CHUNK (1 * GB)
#define NFILES (LOGICAL / CHUNK)
#define WRITE_CAP (512 * MB)        /* never write more than this in all */
#define GUARD_AVAIL (600 * MB)
#define GUARD_FOOT (256 * MB)
#define GUARD_DISK (2 * GB)

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
    return 0;
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
    say("%-22s foot %5.0f int %5.0f comp %4.0f ext %5.0f res %5.0f | avail %5.0f | pagein %6llu rd %5.0f wr %5.0f MB | disk %4.0f MB",
        what, v.foot / (double)MB, v.internal / (double)MB, v.compressed / (double)MB, v.external / (double)MB,
        v.resident / (double)MB, avail() / (double)MB, (unsigned long long)(r.ri_pageins - ru0.ri_pageins),
        (r.ri_diskio_bytesread - ru0.ri_diskio_bytesread) / (double)MB, (r.ri_diskio_byteswritten - ru0.ri_diskio_byteswritten) / (double)MB,
        disk_used_mb());
    if (mbs >= 0) say(" | %6.0f MB/s, worst step %5.1f ms", mbs, worst_ms);
    say("\n");
}
static int guard(void) {
    Vm v = vm(); uint64_t a = avail(); double df = disk_free_gb();
    if (a && a < GUARD_AVAIL) { say("GUARD: available %llu MB < %llu: stopping\n", (unsigned long long)(a / MB), (unsigned long long)(GUARD_AVAIL / MB)); return 1; }
    if (v.foot > base.foot + GUARD_FOOT) { say("GUARD: footprint grew %llu MB: stopping\n", (unsigned long long)((v.foot - base.foot) / MB)); return 1; }
    if (df >= 0 && df * GB < GUARD_DISK) { say("GUARD: %.1f GB free on disk: stopping\n", df); return 1; }
    return 0;
}

static uint64_t tag(uint64_t off) { uint64_t x = off * 0x9E3779B97F4A7C15ull; return x ^ (x >> 29); }

static uint64_t written;   /* bytes of the window dirtied so far, checked against WRITE_CAP */
static double pass_seq(uint8_t *w, uint64_t first, uint64_t bytes, int write, uint64_t *bad, double *worst) {
    const uint64_t pg = (uint64_t)getpagesize(), step = 4 * MB;
    double t0 = now_s(); *worst = 0;
    for (uint64_t s = 0; s < bytes; s += step) {
        double ts = now_s();
        for (uint64_t o = first + s; o < first + s + step && o < first + bytes; o += pg) {
            volatile uint64_t *p = (volatile uint64_t *)(w + o);
            for (int k = 0; k < 8; k++) {
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
static double pass_rand(uint8_t *w, uint64_t first, uint64_t bytes, uint64_t n, uint64_t *bad, double *worst) {
    const uint64_t pg = (uint64_t)getpagesize(), pages = bytes / pg;
    double t0 = now_s(), ts = t0; *worst = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t off = first + (rnd() % pages) * pg + 8 * (rnd() % 8);
        volatile uint64_t *p = (volatile uint64_t *)(w + off);
        if (*p != tag(off)) (*bad)++;
        *p = tag(off);   /* rewrites the same value inside the set: no new data */
        if ((i & 1023) == 1023) { double t = now_s(); if ((t - ts) * 1000 > *worst) *worst = (t - ts) * 1000; ts = t; }
    }
    double t = now_s() - t0;
    return t > 0 ? n * 64.0 / MB / t : 0;
}

int e1_safe_all(char *buf, unsigned long cap, int unused_states, int unused_touch) {
    (void)unused_states; (void)unused_touch;
    o_p = buf; o_left = cap; buf[0] = 0; written = 0;
    for (int i = 0; i < (int)NFILES; i++) fds[i] = -1;
    say("E1-Safe file-backed memory (NXNative DM_GUEST_FILE pattern), %s, page %d bytes\n", TARGET_OS_IPHONE ? "iOS" : "macOS", getpagesize());
    say("limits: 4 GB logical, sets 128 + 256 MB, at most %llu MB written, no spill\n", (unsigned long long)(WRITE_CAP / MB));
    char dir[1024] = "";
    if (!confstr(_CS_DARWIN_USER_TEMP_DIR, dir, sizeof dir)) { say("no temporary folder (errno %d)\n", errno); return 1; }
    ru_get(&ru0); base = vm();
    row("start", -1, 0);

    uint8_t *w = mmap(NULL, LOGICAL, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (w == MAP_FAILED) { say("reserve 4 GB: FAILED errno %d\n", errno); return 1; }
    say("reserved %llu GB at %p\n", (unsigned long long)(LOGICAL / GB), w);
    int bad_setup = 0;
    for (int i = 0; i < (int)NFILES && !bad_setup; i++) {
        char path[1200]; snprintf(path, sizeof path, "%s/e1safe-%d-%d.mem", dir, (int)getpid(), i);
#if TARGET_OS_IPHONE
        fds[i] = open_dprotected_np(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 3, 0, 0600);   /* class C */
        if (fds[i] < 0) fds[i] = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
#else
        fds[i] = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
#endif
        if (fds[i] < 0) { say("open %s: errno %d\n", path, errno); bad_setup = 1; break; }
        unlink(path);
        if (i == 0 && disk_free_gb() * GB < GUARD_DISK + 1 * GB) { say("only %.1f GB free on disk: not starting\n", disk_free_gb()); bad_setup = 1; break; }
        if (ftruncate(fds[i], (off_t)CHUNK)) { say("ftruncate: errno %d\n", errno); bad_setup = 1; break; }
        if (mmap(w + (uint64_t)i * CHUNK, CHUNK, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fds[i], 0) != w + (uint64_t)i * CHUNK) {
            say("map file %d: errno %d\n", i, errno); bad_setup = 1; break;
        }
    }
    if (bad_setup) { munmap(w, LOGICAL); for (int i = 0; i < (int)NFILES; i++) if (fds[i] >= 0) close(fds[i]); return 1; }
    say("%d sparse files of 1 GB (unlinked), mapped shared over it; %.1f GB free on their disk\n", (int)NFILES, disk_free_gb());
    row("files mapped", -1, 0);

    uint64_t bad = 0; int stopped = 0, done = 0;
    static const uint64_t sets[] = {128 * MB, 256 * MB};
    for (unsigned s = 0; s < 2 && !stopped; s++) {
        uint64_t W = sets[s], first = (uint64_t)s * GB;   /* each set in its own file, so each starts cold */
        if (written + W > WRITE_CAP) { say("write cap reached: skipping W=%llu\n", (unsigned long long)(W / MB)); break; }
        char lab[64]; double worst, r;
        snprintf(lab, sizeof lab, "W=%llu seq write", (unsigned long long)(W / MB)); r = pass_seq(w, first, W, 1, &bad, &worst); written += W; row(lab, r, worst);
        if ((stopped = guard())) break;
        snprintf(lab, sizeof lab, "W=%llu seq read", (unsigned long long)(W / MB)); r = pass_seq(w, first, W, 0, &bad, &worst); row(lab, r, worst);
        snprintf(lab, sizeof lab, "W=%llu random x%llu", (unsigned long long)(W / MB), (unsigned long long)(W / getpagesize() * 4));
        r = pass_rand(w, first, W, W / getpagesize() * 4, &bad, &worst); row(lab, r, worst);
        stopped = guard(); done = s + 1;
    }
    say("working sets: %llu bad words, %llu MB written\n", (unsigned long long)bad, (unsigned long long)(written / MB));

    /* force the data to disk, then check every set again: the disk write counters show the files' own traffic */
    double t0 = now_s(); int serr = 0;
    for (int s = 0; s < done; s++) if (msync(w + (uint64_t)s * GB, sets[s], MS_SYNC)) serr = errno;
    row("after msync", (double)written / MB / (now_s() - t0 + 1e-9), 0);
    if (serr) say("msync: errno %d\n", serr);
    uint64_t vbad = 0;
    for (int s = 0; s < done; s++) { double worst; char lab[64]; snprintf(lab, sizeof lab, "W=%llu verify", (unsigned long long)(sets[s] / MB));
        double r = pass_seq(w, (uint64_t)s * GB, sets[s], 0, &vbad, &worst); row(lab, r, worst); }
    say("verify after msync: %llu bad words\n", (unsigned long long)vbad);

    munmap(w, LOGICAL);
    for (int i = 0; i < (int)NFILES; i++) if (fds[i] >= 0) close(fds[i]);
    row("released", -1, 0);
    int fail = bad || vbad || serr || written > WRITE_CAP || done == 0;
    say("E1-Safe RESULT: %s (%d of 2 sets%s)\n", fail ? "FAILED" : "data intact", done, stopped ? ", stopped by a guard" : "");
    return fail;
}
