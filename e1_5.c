/* E1.5: can file-backed pages be pushed out of memory and brought back cheaply? (e1*.c are unchanged)
   Same mechanism as E1-Safe / NXNative's DM_GUEST_FILE: one 4 GB PROT_NONE reservation, four sparse 1 GB files
   (O_EXCL, unlinked at once, protection class C, ftruncate) mapped MAP_SHARED | MAP_FIXED over it. No game files.
   Sets of 128 and 256 MB (384 MB written, cap 512 MB), written once and synced so their pages are clean. Then, for each
   release request (madvise DONTNEED, msync INVALIDATE, madvise PAGEOUT when the SDK has it, unmap + map again):
   residency by mincore() and the task's counters before and after, a verdict (released / partial / IGNORED / refused),
   then recovery: a random read touching each page once (latency per access) and, after a second release, a sequential
   read. Every read checks the data. No anonymous allocations; at least 2 GB must stay free on disk before every phase. */
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#define LOGICAL (4 * GB)
#define CHUNK (1 * GB)
#define NFILES (LOGICAL / CHUNK)
#define WRITE_CAP (512 * MB)
#define GUARD_AVAIL (600 * MB)
#define GUARD_FOOT (256 * MB)
#ifndef GUARD_DISK
#define GUARD_DISK (2 * GB)         /* minimum free storage, checked before every phase */
#endif
#define MAXSET (256 * MB)
#define MAXPAGES (MAXSET / 4096)    /* (pages are 16 KB on arm64; 4 KB is the safe bound) */

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
static uint64_t pageins(void) { struct rusage_info_v4 r; return proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&r) ? 0 : r.ri_pageins; }
static uint64_t pi0;
static Vm base;

/* share of the range's pages in memory (mincore: the file's pages, mapped by us or only cached) */
static double incore(uint8_t *p, uint64_t bytes) {
    static char vec[MAXPAGES];
    uint64_t pg = (uint64_t)getpagesize(), n = bytes / pg, r = 0;
    if (n > MAXPAGES || mincore(p, bytes, vec)) return -1;
    for (uint64_t i = 0; i < n; i++) r += (vec[i] & MINCORE_INCORE) != 0;
    return (double)r / n;
}
static void row(const char *what, uint8_t *p, uint64_t bytes) {
    Vm v = vm();
    say("  %-30s foot %4.0f ext %4.0f res %4.0f comp %3.0f MB | avail %5.0f | pagein %6llu | incore %5.1f%% | disk %4.0f MB\n",
        what, v.foot / (double)MB, v.external / (double)MB, v.resident / (double)MB, v.compressed / (double)MB,
        avail() / (double)MB, (unsigned long long)(pageins() - pi0), 100 * incore(p, bytes), disk_used_mb());
}
static int guard(void) {
    Vm v = vm(); uint64_t a = avail(); double df = disk_free_gb();
    if (a && a < GUARD_AVAIL) { say("GUARD: available %llu MB < %llu: stopping\n", (unsigned long long)(a / MB), (unsigned long long)(GUARD_AVAIL / MB)); return 1; }
    if (v.foot > base.foot + GUARD_FOOT) { say("GUARD: footprint grew %llu MB: stopping\n", (unsigned long long)((v.foot - base.foot) / MB)); return 1; }
    if (df >= 0 && df * GB < GUARD_DISK) { say("GUARD: %.2f GB free on disk: stopping\n", df); return 1; }
    return 0;
}
static int disk_ok(const char *phase, uint64_t need) {
    double df = disk_free_gb();
    int ok = df < 0 || df * GB >= GUARD_DISK + need;
    if (!ok) say("disk check before %s: %.0f MB free, %llu MB needed + %llu MB margin: STOP\n", phase, df * 1024,
                 (unsigned long long)(need / MB), (unsigned long long)(GUARD_DISK / MB));
    return ok;
}

static uint64_t tag(uint64_t off) { uint64_t x = off * 0x9E3779B97F4A7C15ull; return x ^ (x >> 29); }

/* sequential pass over [p, p + bytes) (window offset `first`): 8 words a page; returns MB/s and the worst 4 MB step */
static double pass_seq(uint8_t *w, uint64_t first, uint64_t bytes, int write, uint64_t *bad, double *worst) {
    const uint64_t pg = (uint64_t)getpagesize(), step = 4 * MB;
    double t0 = now_s(); *worst = 0;
    for (uint64_t s = 0; s < bytes; s += step) {
        double ts = now_s();
        for (uint64_t o = first + s; o < first + s + step && o < first + bytes; o += pg) {
            volatile uint64_t *q = (volatile uint64_t *)(w + o);
            for (int k = 0; k < 8; k++) {
                uint64_t off = o + 8 * (uint64_t)k;
                if (write) q[k] = tag(off); else if (q[k] != tag(off)) (*bad)++;
            }
        }
        double d = (now_s() - ts) * 1000; if (d > *worst) *worst = d;
    }
    double t = now_s() - t0;
    return t > 0 ? bytes / (double)MB / t : 0;
}
static uint64_t rs = 0x2545F4914F6CDD1Dull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
/* random read: every page once in a shuffled order, one checked word each; latency per access in microseconds */
static void pass_rand(uint8_t *w, uint64_t first, uint64_t bytes, uint64_t *bad, float *p50, float *p99, float *mx, double *total_ms) {
    static uint32_t idx[MAXPAGES]; static float lat[MAXPAGES];
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    const uint64_t pg = (uint64_t)getpagesize(), n = bytes / pg;
    for (uint64_t i = 0; i < n; i++) idx[i] = (uint32_t)i;
    for (uint64_t i = n - 1; i > 0; i--) { uint64_t j = rnd() % (i + 1); uint32_t t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    double t0 = now_s();
    for (uint64_t i = 0; i < n; i++) {
        uint64_t off = first + (uint64_t)idx[i] * pg + 8 * (rnd() % 8);
        uint64_t a = mach_absolute_time();
        uint64_t v = *(volatile uint64_t *)(w + off);
        lat[i] = (float)((mach_absolute_time() - a) * tb.numer / tb.denom) / 1000.0f;
        if (v != tag(off)) (*bad)++;
    }
    *total_ms = (now_s() - t0) * 1000;
    qsort(lat, n, sizeof lat[0], cmpf);
    *p50 = lat[n / 2]; *p99 = lat[n * 99 / 100]; *mx = lat[n - 1];
}

enum { M_DONTNEED, M_INVALIDATE, M_PAGEOUT, M_REMAP, NM };
static const char *mname[NM] = {"madvise(MADV_DONTNEED)", "msync(MS_SYNC|MS_INVALIDATE)", "madvise(MADV_PAGEOUT)", "unmap + map again"};
/* 0 = accepted, -1 = not in this SDK, otherwise errno */
static int release(int m, uint8_t *p, uint64_t bytes, int fd, uint64_t foff) {
    switch (m) {
    case M_DONTNEED: return madvise(p, bytes, MADV_DONTNEED) ? errno : 0;
    case M_INVALIDATE: return msync(p, bytes, MS_SYNC | MS_INVALIDATE) ? errno : 0;
    case M_PAGEOUT:
#ifdef MADV_PAGEOUT
        return madvise(p, bytes, MADV_PAGEOUT) ? errno : 0;
#else
        (void)p; return -1;
#endif
    case M_REMAP:   /* as NXNative's guest_release takes a range away: replace the mapping, then map the file back */
        if (mmap(p, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != p) return errno;
        return mmap(p, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, (off_t)foff) == p ? 0 : errno;
    }
    return EINVAL;
}

int e15_all(char *buf, unsigned long cap, int unused_states, int unused_touch) {
    (void)unused_states; (void)unused_touch;
    o_p = buf; o_left = cap; buf[0] = 0;
    for (int i = 0; i < (int)NFILES; i++) fds[i] = -1;
    say("E1.5 page release and recovery of file-backed memory (NXNative DM_GUEST_FILE pattern), %s, page %d bytes\n",
        TARGET_OS_IPHONE ? "iOS" : "macOS", getpagesize());
    say("limits: 4 GB logical, sets 128 + 256 MB, at most %llu MB written, at least %llu MB free on disk, no anonymous allocations\n",
        (unsigned long long)(WRITE_CAP / MB), (unsigned long long)(GUARD_DISK / MB));
#ifndef MADV_PAGEOUT
    say("note: MADV_PAGEOUT is not declared by this SDK; that request is reported as not available\n");
#endif
    char dir[1024] = "";
    if (!confstr(_CS_DARWIN_USER_TEMP_DIR, dir, sizeof dir)) { say("no temporary folder (errno %d)\n", errno); return 1; }
    pi0 = pageins(); base = vm();

    uint8_t *w = mmap(NULL, LOGICAL, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (w == MAP_FAILED) { say("reserve 4 GB: FAILED errno %d\n", errno); return 1; }
    say("reserved %llu GB at %p\n", (unsigned long long)(LOGICAL / GB), w);
    int bad_setup = 0;
    for (int i = 0; i < (int)NFILES && !bad_setup; i++) {
        char path[1200]; snprintf(path, sizeof path, "%s/e15-%d-%d.mem", dir, (int)getpid(), i);
#if TARGET_OS_IPHONE
        fds[i] = open_dprotected_np(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 3, 0, 0600);   /* class C */
        if (fds[i] < 0) fds[i] = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
#else
        fds[i] = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
#endif
        if (fds[i] < 0) { say("open %s: errno %d\n", path, errno); bad_setup = 1; break; }
        unlink(path);
        if (i == 0 && disk_free_gb() * GB < GUARD_DISK + 384 * MB) { say("only %.2f GB free on disk (needs the margin + 384 MB): not starting\n", disk_free_gb()); bad_setup = 1; break; }
        if (ftruncate(fds[i], (off_t)CHUNK)) { say("ftruncate: errno %d\n", errno); bad_setup = 1; break; }
        if (mmap(w + (uint64_t)i * CHUNK, CHUNK, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fds[i], 0) != w + (uint64_t)i * CHUNK) {
            say("map file %d: errno %d\n", i, errno); bad_setup = 1; break;
        }
    }
    if (bad_setup) { munmap(w, LOGICAL); for (int i = 0; i < (int)NFILES; i++) if (fds[i] >= 0) close(fds[i]); return 1; }
    say("%d sparse files of 1 GB (unlinked), mapped shared over it; %.1f GB free on their disk\n", (int)NFILES, disk_free_gb());

    static const uint64_t sets[] = {128 * MB, 256 * MB};
    uint64_t bad = 0, written = 0; int stopped = 0, done = 0, wrote = 0, accepted_ignored = 0, released_ok = 0;
    for (int s = 0; s < 2 && !stopped; s++) {
        uint64_t W = sets[s], first = (uint64_t)s * GB; uint8_t *p = w + first; int fd = fds[s];
        double worst, r; float p50, p99, mx; double tms;
        say("\n== set %llu MB (file %d)\n", (unsigned long long)(W / MB), s);
        if (written + W > WRITE_CAP || !disk_ok("write", W)) { stopped = 1; break; }
        r = pass_seq(w, first, W, 1, &bad, &worst); written += W; wrote = s + 1;
        int se = msync(p, W, MS_SYNC) ? errno : 0;   /* clean pages: a release then loses nothing */
        say("  write %.0f MB/s, msync %s\n", r, se ? strerror(se) : "ok");
        row("written and synced", p, W);
        uint64_t pi = pageins();
        r = pass_seq(w, first, W, 0, &bad, &worst);
        say("  warm seq read      %8.0f MB/s, worst 4 MB step %6.2f ms, pageins %llu\n", r, worst, (unsigned long long)(pageins() - pi));
        pi = pageins(); pass_rand(w, first, W, &bad, &p50, &p99, &mx, &tms);
        say("  warm random read   %8.1f ms for every page, latency p50 %.2f / p99 %.2f / max %.1f us, pageins %llu\n",
            tms, p50, p99, mx, (unsigned long long)(pageins() - pi));
        if ((stopped = guard())) break;

        for (int m = 0; m < NM && !stopped; m++) {
            if (!disk_ok(mname[m], 0)) { stopped = 1; break; }
            say("-- %s\n", mname[m]);
            pass_seq(w, first, W, 0, &bad, &worst);   /* all pages in before each request */
            double fb = incore(p, W); Vm vb = vm();
            double t0 = now_s(); int e = release(m, p, W, fd, 0); double tr = (now_s() - t0) * 1000;
            double fa = incore(p, W); Vm va = vm();
            if (e == -1) { say("  not available in this SDK\n"); continue; }
            /* mincore sees the file's pages whether we map them or the file cache only keeps them; the task's resident
               size sees only what we map. Both are needed to tell "ignored" from "left the process, kept in the cache". */
            int left_task = vb.resident > va.resident && vb.resident - va.resident >= W * 9 / 10;
            const char *verdict = e ? "REFUSED" : fa < 0 ? "unknown (mincore failed)"
                                : fa <= 0.1 ? "released: out of the process and out of the file cache"
                                : fa >= 0.9 * fb && left_task ? "out of the process only: the file cache keeps the pages"
                                : fa >= 0.9 * fb ? "IGNORED: accepted, pages still in memory" : "partial";
            if (!e && fa >= 0.9 * fb && !left_task) accepted_ignored++;
            if (!e && fa >= 0 && fa <= 0.1) released_ok++;
            say("  request: %s in %.2f ms -> %s%s%s\n", e ? "error" : "accepted", tr, verdict, e ? ", " : "", e ? strerror(e) : "");
            say("  incore %.1f%% -> %.1f%% | resident %.0f -> %.0f MB | external %.0f -> %.0f MB | footprint %.0f -> %.0f MB\n",
                100 * fb, 100 * fa, vb.resident / (double)MB, va.resident / (double)MB, vb.external / (double)MB, va.external / (double)MB,
                vb.foot / (double)MB, va.foot / (double)MB);
            if (e) continue;
            pi = pageins(); pass_rand(w, first, W, &bad, &p50, &p99, &mx, &tms);
            say("  random read after  %8.1f ms for every page, latency p50 %.2f / p99 %.2f / max %.1f us, pageins %llu\n",
                tms, p50, p99, mx, (unsigned long long)(pageins() - pi));
            pass_seq(w, first, W, 0, &bad, &worst);
            release(m, p, W, fd, 0);
            double f2 = incore(p, W);
            pi = pageins(); r = pass_seq(w, first, W, 0, &bad, &worst);
            say("  seq read after     %8.0f MB/s, worst 4 MB step %6.2f ms, pageins %llu (incore %.1f%% before it)\n",
                r, worst, (unsigned long long)(pageins() - pi), 100 * f2);
            row("after recovery", p, W);
            stopped = guard();
        }
        if (!stopped) done = s + 1;
    }

    uint64_t vbad = 0;
    for (int s = 0; s < wrote; s++) {
        double worst; pass_seq(w, (uint64_t)s * GB, sets[s], 0, &vbad, &worst);
    }
    say("\nfinal check of everything written: %llu bad words; during the run: %llu bad words; %llu MB written\n",
        (unsigned long long)vbad, (unsigned long long)bad, (unsigned long long)(written / MB));
    say("requests accepted but ignored: %d; requests that released at least 90%% of the pages from memory: %d\n", accepted_ignored, released_ok);
    munmap(w, LOGICAL);
    double held = disk_used_mb();   /* unlinked files: their blocks go back when closed */
    for (int i = 0; i < (int)NFILES; i++) if (fds[i] >= 0) close(fds[i]);
    Vm v = vm(); say("released: footprint %.0f MB, resident %.0f MB; the files held %.0f MB on disk until closed\n", v.foot / (double)MB, v.resident / (double)MB, held);
    int fail = bad || vbad || written > WRITE_CAP || done == 0;
    say("E1.5 RESULT: %s (%d of 2 sets%s)\n", fail ? "FAILED" : "data intact", done, stopped ? ", stopped by a guard" : "");
    return fail;
}
