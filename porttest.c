/* Hookshot lifter portability test: every instruction of insns.S runs (a) natively on this ARM64 CPU and (b) as the
   C that Hookshot's lifter made from it, from the same random states; registers, flags and memory are compared.
   Plus: can this OS reserve Hookshot's fixed guest window, and how much memory may this process use. */
#include "a64.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

extern const int PT_N;
extern void (*const PT_LIFT[])(Ctx *);
extern void (*const PT_NATIVE[])(void *);
extern const int PT_FLAGS[];
extern const unsigned PT_WORD[];
extern const char *const PT_NAME[];

typedef struct { uint64_t x[8], nzcv, fpcr, fpsr, pad, v[8][2]; } St;   /* native.S offsets: 0, 64, 72, 80, 96 */
_Static_assert(sizeof(St) == 224, "native.S offsets");

#define SCR 0x10000
static uint8_t scr_a[SCR] __attribute__((aligned(64))), scr_b[SCR] __attribute__((aligned(64)));
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static char *out_p; static size_t out_left;
static void say(const char *f, ...) {
    va_list a; va_start(a, f);
    int n = vsnprintf(out_p, out_left, f, a);
    va_end(a);
    if (n > 0 && (size_t)n < out_left) { out_p += n; out_left -= n; }
}

static uint64_t rand_val(void) {
    static const uint64_t edge[] = {0, 1, 0xFFFFFFFFull, 0x80000000ull, 1ull << 63, ~0ull, 0x7FF8000000000000ull,
                                    0x3F800000ull, 0x7F800000ull, 0x00800000ull};
    uint64_t r = rnd();
    return (r & 7) < 5 ? rnd() : edge[(r >> 8) % 10];
}

static void make_state(St *s, int mem) {
    for (int i = 0; i < 8; i++) s->x[i] = rand_val();
    if (mem) s->x[2] &= 7;                        /* the index register of [x0, x2, lsl #3] */
    s->x[0] = (uint64_t)(uintptr_t)(scr_a + SCR / 2);   /* replaced per run: the scratch buffer's middle */
    s->nzcv = (rnd() & 0xF) << 28;
    s->fpcr = (rnd() & 1) ? (1u << 24) : 0;       /* FZ on/off, as TOTK/OoT run */
    s->fpsr = 0;
    for (int i = 0; i < 8; i++)
        for (int h = 0; h < 2; h++) {
            if (rnd() & 1) s->v[i][h] = rnd();
            else {   /* two "reasonable" floats */
                float f0 = (float)((int64_t)(rnd() % 2000001) - 1000000) / 1000.0f, f1 = (float)(rnd() % 1000) / 7.0f;
                uint32_t a, b; memcpy(&a, &f0, 4); memcpy(&b, &f1, 4);
                s->v[i][h] = a | (uint64_t)b << 32;
            }
        }
}

/* returns 0 same, 1 differs (registers/flags/memory), 2 FPSR only */
static int run_one(int i, const St *in, char *why, size_t wn) {
    St n = *in;
    for (int k = 0; k < SCR; k++) scr_a[k] = scr_b[k] = (uint8_t)(k * 131 + i);
    n.x[0] = (uint64_t)(uintptr_t)(scr_a + SCR / 2);
    PT_NATIVE[i](&n);

    Ctx c; memset(&c, 0, sizeof c);
    for (int k = 0; k < 8; k++) c.x[k] = in->x[k];
    c.x[0] = (uint64_t)(uintptr_t)(scr_b + SCR / 2);   /* A64_MEM_ZERO: guest address = host address */
    c.n = (in->nzcv >> 31) & 1; c.z = (in->nzcv >> 30) & 1; c.c = (in->nzcv >> 29) & 1; c.v = (in->nzcv >> 28) & 1;
    c.fpcr = in->fpcr; c.fpsr = in->fpsr;
    for (int k = 0; k < 8; k++) { c.vreg[k][0] = in->v[k][0]; c.vreg[k][1] = in->v[k][1]; }
    PT_LIFT[i](&c);
    if (c.halted) { snprintf(why, wn, "halted %d", c.halted); return 1; }

    /* x0 points at a different buffer in each run: compare it as an offset */
    uint64_t xn0 = n.x[0] - (uint64_t)(uintptr_t)scr_a, xc0 = c.x[0] - (uint64_t)(uintptr_t)scr_b;
    if (xn0 != xc0) { snprintf(why, wn, "x0 off %llx vs %llx", (unsigned long long)xn0, (unsigned long long)xc0); return 1; }
    for (int k = 1; k < 8; k++)
        if (n.x[k] != c.x[k]) {
            snprintf(why, wn, "x%d native %016llx lifted %016llx", k, (unsigned long long)n.x[k], (unsigned long long)c.x[k]);
            return 1;
        }
    uint64_t cn = (uint64_t)c.n << 31 | (uint64_t)c.z << 30 | (uint64_t)c.c << 29 | (uint64_t)c.v << 28;
    if ((n.nzcv & 0xF0000000u) != cn) { snprintf(why, wn, "nzcv %llx vs %llx", (unsigned long long)n.nzcv, (unsigned long long)cn); return 1; }
    for (int k = 0; k < 8; k++)
        if (n.v[k][0] != c.vreg[k][0] || n.v[k][1] != c.vreg[k][1]) {
            snprintf(why, wn, "v%d native %016llx%016llx lifted %016llx%016llx", k, (unsigned long long)n.v[k][1],
                     (unsigned long long)n.v[k][0], (unsigned long long)c.vreg[k][1], (unsigned long long)c.vreg[k][0]);
            return 1;
        }
    if (memcmp(scr_a, scr_b, SCR)) { snprintf(why, wn, "memory"); return 1; }
    if ((n.fpsr & 0x9F) != (c.fpsr & 0x9F)) { snprintf(why, wn, "fpsr %llx vs %llx", (unsigned long long)n.fpsr, (unsigned long long)c.fpsr); return 2; }
    return 0;
}

/* lifter test: returns the number of instructions whose lifted C differs from the CPU */
int pt_lifter(int states) {
    int bad = 0, fpsr_only = 0, untrans = 0;
    for (int i = 0; i < PT_N; i++) {
        if (PT_FLAGS[i] & 1) { untrans++; say("NOT TRANSLATED %08x %s\n", PT_WORD[i], PT_NAME[i]); continue; }
        int mem = strchr(PT_NAME[i], '[') != NULL, worst = 0;
        char why[160] = "";
        for (int s = 0; s < states; s++) {
            St st; make_state(&st, mem);
            char w[160];
            int r = run_one(i, &st, w, sizeof w);
            if (r > worst) { worst = r; memcpy(why, w, sizeof why); }
            if (r == 1) break;
        }
        if (worst == 1) { bad++; say("DIFF %08x %-36s %s\n", PT_WORD[i], PT_NAME[i], why); }
        else if (worst == 2) { fpsr_only++; say("FPSR %08x %-36s %s\n", PT_WORD[i], PT_NAME[i], why); }
    }
    say("lifter: %d instructions x %d states: %d same, %d differ, %d differ only in FPSR flags, %d not translated\n",
        PT_N, states, PT_N - bad - fpsr_only - untrans, bad, fpsr_only, untrans);
    return bad + untrans;
}

/* Hookshot reserves 2^39 bytes at A64_HOST_BASE (0x100000000000, hos_main.c reserve_guest) */
void pt_vaprobe(void) {
    const uint64_t base = 0x100000000000ull, win = 1ull << 39;
    void *p = mmap((void *)base, win, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) say("va: Hookshot window (512 GB at 0x100000000000): FAILED, errno %d\n", errno);
    else {
        say("va: Hookshot window (512 GB): %s (got %p)\n", p == (void *)base ? "OK at the fixed address" : "only elsewhere", p);
        munmap(p, win);
    }
    uint64_t lo = 0, hi = 1ull << 44;   /* largest single reservation, to 1 GB */
    void *where = NULL;
    while (hi - lo > (1ull << 30)) {
        uint64_t mid = lo + (hi - lo) / 2;
        void *q = mmap(NULL, mid, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (q != MAP_FAILED) { lo = mid; where = q; munmap(q, mid); } else hi = mid;
    }
    say("va: largest reservation %.1f GB (last at %p)\n", lo / 1073741824.0, where);
    for (int g = 4; g <= 64; g *= 2) {   /* the 4 GB the a64-kit 32-bit model needs, and more */
        void *q = mmap(NULL, (uint64_t)g << 30, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        say("va: %2d GB anywhere: %s\n", g, q == MAP_FAILED ? "no" : "yes");
        if (q != MAP_FAILED) munmap(q, (uint64_t)g << 30);
    }
}

static uint64_t footprint(void) {
    task_vm_info_data_t vi; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) == KERN_SUCCESS ? vi.phys_footprint : 0;
}

/* how much memory this process may really use: touch 64 MB chunks until the OS says < 256 MB are left (iOS) */
void pt_memprobe(int touch) {
    uint64_t ram = 0; size_t len = sizeof ram;
    sysctlbyname("hw.memsize", &ram, &len, NULL, 0);
    say("mem: device RAM %.2f GB, footprint now %.0f MB\n", ram / 1073741824.0, footprint() / 1048576.0);
#if TARGET_OS_IPHONE
    say("mem: os_proc_available_memory at start: %.0f MB\n", os_proc_available_memory() / 1048576.0);
    if (!touch) return;
    const size_t chunk = 64ull << 20;
    void *keep[256]; int k = 0;
    while (k < 256 && os_proc_available_memory() > (256ull << 20)) {
        void *q = mmap(NULL, chunk, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (q == MAP_FAILED) break;
        for (size_t o = 0; o < chunk; o += 4096) ((volatile uint8_t *)q)[o] = (uint8_t)(o >> 12) | 1;   /* dirty: counts */
        keep[k++] = q;
    }
    say("mem: dirtied %d MB before the limit's last 256 MB; footprint %.0f MB, available %.0f MB\n",
        k * 64, footprint() / 1048576.0, os_proc_available_memory() / 1048576.0);
    while (k) munmap(keep[--k], chunk);
#else
    (void)touch;
#endif
}

/* everything, into buf; returns the lifter's failure count */
int pt_all(char *buf, size_t cap, int states, int touch) {
    out_p = buf; out_left = cap; buf[0] = 0;
    say("Hookshot porttest (lifter %s, %s)\n", "a64-kit a64emit + nx2/rtgen ADDR64", TARGET_OS_IPHONE ? "iOS" : "macOS");
    int bad = pt_lifter(states);
    pt_vaprobe();
    pt_memprobe(touch);
    return bad;
}
