// E2-Debug: a diagnostic copy of E2 (e2.m is unchanged) to find where HS MetalTest stops on the iPhone.
// - every phase writes BEGIN / END lines to Documents/e2_debug_log.txt with fsync, and the report so far to
//   Documents/e2_debug.txt after each phase, so a crash leaves both behind;
// - a signal handler and an uncaught-exception handler write the signal or exception and the current phase;
// - a phase that began but never ended in the previous run is disabled for good (Documents/e2_debug_disabled.txt,
//   which also takes names written by hand, one per line);
// - small tests (at most 64 MB of Metal memory alive), every compressed format only after the device says it has it;
// - every texture descriptor is logged and checked (format against the device's documented capabilities, size, block
//   alignment, usage, storage mode, sample count, mip levels) before newTextureWithDescriptor: Metal's validation aborts
//   the process on a descriptor it rejects (the E2 crash on the A15: MTLReportFailure in validateWithDevice from make_tex),
//   so an unsupported format is reported as UNSUPPORTED and never created. Synthetic data only.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

#define MB (1ull << 20)
#define BUDGET (64 * MB)
#define AVAIL_FLOOR (1000 * MB)

// ---------- report buffer ----------
static char *o_buf, *o_p; static size_t o_left;
static void say(const char *f, ...) {
    va_list a; va_start(a, f);
    int n = vsnprintf(o_p, o_left, f, a);
    va_end(a);
    if (n > 0 && (size_t)n < o_left) { o_p += n; o_left -= n; }
}
// ---------- persistent log, phases, disabling ----------
static NSString *docdir;
static int logfd = -1;
static char cur_phase[96] = "start";
static NSMutableSet *disabled;
static void logw(const char *s) { if (logfd >= 0) { (void)!write(logfd, s, strlen(s)); fsync(logfd); } }
static void logf_(const char *f, ...) { char b[512]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); logw(b); }
static void on_signal(int sig) {   // async-signal-safe: write only
    char b[160] = "SIGNAL "; char n[8]; int k = 0, s = sig; do { n[k++] = '0' + s % 10; s /= 10; } while (s);
    size_t L = strlen(b); while (k) b[L++] = n[--k]; b[L] = 0;
    strlcat(b, " during phase ", sizeof b); strlcat(b, cur_phase, sizeof b); strlcat(b, "\n", sizeof b);
    if (logfd >= 0) { (void)!write(logfd, b, strlen(b)); fsync(logfd); }
    signal(sig, SIG_DFL); raise(sig);   // iOS still writes its own crash report
}
static void on_exception(NSException *x) { logf_("UNCAUGHT EXCEPTION during phase %s: %s: %s\n", cur_phase, x.name.UTF8String, x.reason.UTF8String); }
static void save_report(void) {
    if (!docdir) return;
    [[NSString stringWithUTF8String:o_buf] writeToFile:[docdir stringByAppendingPathComponent:@"e2_debug.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
}
static void setup_log(void) {
#if TARGET_OS_IPHONE
    docdir = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
#else
    docdir = [[NSFileManager defaultManager] currentDirectoryPath];
#endif
    NSString *log = [docdir stringByAppendingPathComponent:@"e2_debug_log.txt"], *prev = [docdir stringByAppendingPathComponent:@"e2_debug_log_prev.txt"];
    NSString *dis = [docdir stringByAppendingPathComponent:@"e2_debug_disabled.txt"];
    NSFileManager *fm = [NSFileManager defaultManager];
    disabled = [NSMutableSet set];
    for (NSString *l in [[NSString stringWithContentsOfFile:dis encoding:NSUTF8StringEncoding error:nil] componentsSeparatedByString:@"\n"])
        if (l.length) [disabled addObject:[l stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]];
    NSString *last = [NSString stringWithContentsOfFile:log encoding:NSUTF8StringEncoding error:nil];
    if (last) {
        [fm removeItemAtPath:prev error:nil]; [fm moveItemAtPath:log toPath:prev error:nil];
        NSString *open = nil, *crash = nil;
        for (NSString *l in [last componentsSeparatedByString:@"\n"]) {
            if ([l hasPrefix:@"BEGIN "]) open = [l substringFromIndex:6];
            else if ([l hasPrefix:@"END "]) open = nil;
            else if ([l hasPrefix:@"SIGNAL "] || [l hasPrefix:@"UNCAUGHT"]) crash = l;
        }
        say("previous run: %s%s%s\n", open ? "stopped inside phase " : "finished every phase it began",
            open ? open.UTF8String : "", crash ? "" : (open ? " (no signal line: killed by the system or a debugger trap)" : ""));
        if (crash) say("previous run's last error line: %s\n", crash.UTF8String);
        if (open && ![disabled containsObject:open]) {
            [disabled addObject:open];
            NSString *all = [[disabled allObjects] componentsJoinedByString:@"\n"];
            [[all stringByAppendingString:@"\n"] writeToFile:dis atomically:YES encoding:NSUTF8StringEncoding error:nil];
            say("phase %s is now disabled (listed in e2_debug_disabled.txt; delete that file to run it again)\n", open.UTF8String);
        }
    } else say("previous run: none\n");
    logfd = open(log.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int sigs[] = {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE};
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) signal(sigs[i], on_signal);
    NSSetUncaughtExceptionHandler(on_exception);
}
static int begin(const char *name) {
    if ([disabled containsObject:@(name)]) { say("SKIP %s: disabled (it did not finish in an earlier run)\n", name); return 0; }
    strlcpy(cur_phase, name, sizeof cur_phase); logf_("BEGIN %s\n", name); return 1;
}
static void end(const char *name, const char *status) { logf_("END %s %s\n", name, status); strlcpy(cur_phase, "between phases", sizeof cur_phase); save_report(); }
// a phase: skipped when disabled, Objective-C exceptions caught and reported, END written either way
#define PHASE(ph_, ...) do { if (begin(ph_)) { const char *st_ = "ok"; @try { @autoreleasepool { __VA_ARGS__; } } \
    @catch (NSException *x_) { say("  EXCEPTION in %s: %s: %s\n", ph_, x_.name.UTF8String, x_.reason.UTF8String); st_ = "exception"; } end(ph_, st_); } } while (0)
// CI only: E2DBG_CRASH=<phase> makes that phase crash, to check the disabling above
static void ci_crash(const char *name) { const char *e = getenv("E2DBG_CRASH"); if (e && !strcmp(e, name)) raise(SIGSEGV); }

// ---------- measurement ----------
static id<MTLDevice> dev;
static id<MTLCommandQueue> q;
static id<MTLComputePipelineState> probe;
static double now_ms(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6; }
static uint64_t avail(void) {
#if TARGET_OS_IPHONE
    return os_proc_available_memory();
#else
    return 0;
#endif
}
typedef struct { double foot, gfx, alloc; } Mem;
static Mem mem(void) {
    task_vm_info_data_t v; mach_msg_type_number_t n = TASK_VM_INFO_COUNT; Mem m = {0};
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&v, &n) == KERN_SUCCESS) {
        m.foot = v.phys_footprint / (double)MB;
        if (n >= TASK_VM_INFO_REV3_COUNT) m.gfx = v.ledger_tag_graphics_footprint / (double)MB;
    }
    m.alloc = dev ? dev.currentAllocatedSize / (double)MB : 0;
    return m;
}
static void mrow(const char *what, Mem b) {
    Mem m = mem();
    say("  %-30s foot %+6.1f gfx %+6.1f MTL allocated %+6.1f MB | avail %5.0f MB\n", what, m.foot - b.foot, m.gfx - b.gfx, m.alloc - b.alloc, avail() / (double)MB);
}
static int room_for(uint64_t bytes) {
    uint64_t a = avail();
    if (bytes > BUDGET || (a && a < bytes + AVAIL_FLOOR)) { say("  SKIP: %llu MB over the budget or the room (avail %llu MB)\n", (unsigned long long)(bytes / MB), (unsigned long long)(a / MB)); return 0; }
    return 1;
}
static int cb_ok(id<MTLCommandBuffer> cb, const char *what) {
    if (cb.status == MTLCommandBufferStatusCompleted) return 1;
    say("  %s: command buffer status %ld, error %s\n", what, (long)cb.status, cb.error ? cb.error.localizedDescription.UTF8String : "none");
    return 0;
}
static int bc = -1;   // supportsBCTextureCompression: 1 yes, 0 no, -1 not asked / no such API
static const char *fmt_name(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatRGBA8Unorm: return "RGBA8Unorm"; case MTLPixelFormatRGBA16Float: return "RGBA16Float";
    case MTLPixelFormatDepth32Float: return "Depth32Float"; case MTLPixelFormatASTC_4x4_LDR: return "ASTC_4x4_LDR";
    case MTLPixelFormatBC1_RGBA: return "BC1_RGBA"; case MTLPixelFormatBC3_RGBA: return "BC3_RGBA"; case MTLPixelFormatBC7_RGBAUnorm: return "BC7_RGBAUnorm";
    default: return "other";
    }
}
// NULL when the descriptor is one this device documents as valid; otherwise why not (checked before Metal sees it)
static const char *desc_problem(MTLTextureDescriptor *d) {
    MTLPixelFormat f = d.pixelFormat; int comp = 0;
    switch (f) {
    case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA16Float: case MTLPixelFormatDepth32Float: break;
    case MTLPixelFormatASTC_4x4_LDR: comp = 1; if (![dev supportsFamily:MTLGPUFamilyApple2]) return "UNSUPPORTED: ASTC needs GPU family Apple2 or later"; break;
    case MTLPixelFormatBC1_RGBA: case MTLPixelFormatBC3_RGBA: case MTLPixelFormatBC7_RGBAUnorm:
        comp = 1; if (bc != 1) return "UNSUPPORTED: the device does not report supportsBCTextureCompression"; break;
    default: return "format not on this test's checked list";
    }
    if (d.textureType != MTLTextureType2D) return "only 2D textures are used here";
    if (!d.width || !d.height || d.width > 8192 || d.height > 8192) return "size out of 1..8192";
    if (comp && (d.width % 4 || d.height % 4)) return "compressed size is not a multiple of the 4x4 block";
    if (d.sampleCount != 1) return "sampleCount must be 1";
    if (d.mipmapLevelCount != 1) return "one mip level only";
    if (comp && (d.usage & ~MTLTextureUsageShaderRead)) return "compressed textures: shader read only";
    if (d.storageMode == MTLStorageModeMemoryless && (d.usage != MTLTextureUsageRenderTarget || ![dev supportsFamily:MTLGPUFamilyApple1]))
        return "memoryless: render-target usage only, Apple GPU family";
    if (f == MTLPixelFormatDepth32Float && d.storageMode == MTLStorageModeShared) return "depth textures cannot be shared";
    return NULL;
}
static id<MTLTexture> new_tex(MTLTextureDescriptor *d) {
    logf_("DESC %s %lux%lu usage 0x%lx storage %ld samples %lu mips %lu\n", fmt_name(d.pixelFormat), (unsigned long)d.width, (unsigned long)d.height,
          (unsigned long)d.usage, (long)d.storageMode, (unsigned long)d.sampleCount, (unsigned long)d.mipmapLevelCount);
    const char *p = desc_problem(d);
    if (p) { say("  SKIPPED %s %lux%lu: %s\n", fmt_name(d.pixelFormat), (unsigned long)d.width, (unsigned long)d.height, p); logf_("DESC not created: %s\n", p); return nil; }
    return [dev newTextureWithDescriptor:d];
}
static id<MTLTexture> make_tex(MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLStorageMode sm, const void *data, NSUInteger rowBytes, NSUInteger bytes) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.storageMode = sm; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = new_tex(d);
    if (!t) { say("  newTexture returned nil\n"); return nil; }
    if (!data) return t;
    if (sm == MTLStorageModeShared) { [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:data bytesPerRow:rowBytes]; return t; }
    id<MTLBuffer> src = [dev newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromBuffer:src sourceOffset:0 sourceBytesPerRow:rowBytes sourceBytesPerImage:bytes sourceSize:MTLSizeMake(w, h, 1)
            toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return cb_ok(cb, "upload") ? t : nil;
}
static int sample(id<MTLTexture> t, float out[4]) {
    id<MTLBuffer> o = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:probe]; [e setTexture:t atIndex:0]; [e setBuffer:o offset:0 atIndex:0];
    [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (!cb_ok(cb, "sample")) return 0;
    memcpy(out, o.contents, 16); return 1;
}
static void setbits(uint64_t b[2], int pos, int n, uint64_t v) { for (int i = 0; i < n; i++) if (v >> i & 1) b[(pos + i) >> 6] |= 1ull << ((pos + i) & 63); }
static void blk_rgba8(uint8_t *o) { for (int i = 0; i < 64; i++) { o[4 * i] = 200; o[4 * i + 1] = 100; o[4 * i + 2] = 50; o[4 * i + 3] = 255; } }
static void blk_bc1(uint8_t *o) { for (int k = 0; k < 4; k++) { uint8_t *p = o + 8 * k; memset(p, 0, 8); p[0] = p[2] = 0x00; p[1] = p[3] = 0xF8; } }
static void blk_bc3(uint8_t *o) { for (int k = 0; k < 4; k++) { uint8_t *p = o + 16 * k; memset(p, 0, 16); p[0] = p[1] = 128; p[9] = p[11] = 0xF8; } }
static void blk_bc7(uint8_t *o) {
    uint64_t b[2] = {0, 0}; setbits(b, 6, 1, 1); const int c7[4] = {32, 64, 96, 127};
    for (int ch = 0; ch < 4; ch++) { setbits(b, 7 + ch * 14, 7, c7[ch]); setbits(b, 14 + ch * 14, 7, c7[ch]); }
    for (int k = 0; k < 4; k++) memcpy(o + 16 * k, b, 16);
}
static void blk_astc(uint8_t *o) {
    uint64_t b[2] = {0xFFFFFFFFFFFFFDFCull, 0}; const uint16_t c[4] = {32 * 257, 96 * 257, 160 * 257, 224 * 257};
    for (int i = 0; i < 4; i++) b[1] |= (uint64_t)c[i] << (16 * i);
    for (int k = 0; k < 4; k++) memcpy(o + 16 * k, b, 16);
}
// one 8x8 texture of a known colour: create, upload, sample, compare
static int decode_check(const char *name, MTLPixelFormat f, NSUInteger rowBytes, NSUInteger bytes, void (*fill)(uint8_t *), const float want[4]) {
    uint8_t data[256]; fill(data);
    say("  %-14s creating...", name); save_report();
    id<MTLTexture> t = make_tex(f, 8, 8, MTLStorageModePrivate, data, rowBytes, bytes);
    if (!t) { say(" not created\n"); return 0; }
    float g[4] = {0};
    if (!sample(t, g)) { say(" sample failed\n"); return 0; }
    int match = 1; for (int c = 0; c < 4; c++) if (fabsf(g[c] - want[c]) > 2.5f / 255) match = 0;
    say(" sampled %.3f %.3f %.3f %.3f (expected %.3f %.3f %.3f %.3f): %s\n", g[0], g[1], g[2], g[3], want[0], want[1], want[2], want[3], match ? "decodes correctly" : "WRONG");
    return match ? 0 : 1;
}
static void tex_mem(const char *label, MTLPixelFormat f, NSUInteger W, int N, int bpp, MTLStorageMode sm, uint8_t *src) {
    uint64_t each = (uint64_t)W * W * bpp / 8;
    if (!room_for(each * N)) return;
    NSUInteger rowBytes = f == MTLPixelFormatRGBA8Unorm ? W * 4 : (W / 4) * 16;
    Mem b = mem(); NSMutableArray *a = [NSMutableArray array]; NSUInteger alloc = 0; double t0 = now_ms();
    for (int i = 0; i < N; i++) { id<MTLTexture> t = make_tex(f, W, W, sm, src, rowBytes, each); if (!t) break; [a addObject:t]; alloc += t.allocatedSize; }
    char l[96]; snprintf(l, sizeof l, "%s %dx%lu^2 (%.0f MB)", label, (int)a.count, (unsigned long)W, each * N / (double)MB);
    mrow(l, b); say("  %-30s allocatedSize %.1f MB, %.2f ms per texture\n", "", alloc / (double)MB, (now_ms() - t0) / (a.count ? a.count : 1));
}
static NSString *const kSrc = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "kernel void probe(texture2d<float> t [[texture(0)]], device float4 *o [[buffer(0)]]) {\n"
    "  constexpr sampler s(filter::nearest, coord::normalized);\n  o[0] = t.sample(s, float2(0.3, 0.3), level(0));\n}\n";

int e2_debug_all(char *buf, unsigned long cap, int unused_states, int unused_touch) {
    (void)unused_states; (void)unused_touch;
    o_buf = o_p = buf; o_left = cap; buf[0] = 0;
    int bad = 0;
    @autoreleasepool {
    say("E2-Debug (diagnostic copy of E2), %s, budget %llu MB, synthetic data only\n", TARGET_OS_IPHONE ? "iOS" : "macOS", (unsigned long long)(BUDGET / MB));
    setup_log();
    logf_("RUN start\n");
    Mem start = {0};
    PHASE("device", {
        dev = MTLCreateSystemDefaultDevice();
        say("[device] %s\n", dev ? dev.name.UTF8String : "NONE");
        ci_crash("device");
    });
    if (!dev) { say("no Metal device\n"); save_report(); return 0; }
    PHASE("queue", { q = [dev newCommandQueue]; say("[queue] %s\n", q ? "ok" : "nil"); });
    PHASE("memory_counters", { start = mem(); say("[memory] footprint %.1f MB, graphics %.1f MB, MTL allocated %.1f MB, avail %.0f MB\n", start.foot, start.gfx, start.alloc, avail() / (double)MB); });
    PHASE("families", {
        struct { const char *n; MTLGPUFamily f; } fam[] = {{"Apple1", MTLGPUFamilyApple1}, {"Apple2", MTLGPUFamilyApple2}, {"Apple3", MTLGPUFamilyApple3},
            {"Apple4", MTLGPUFamilyApple4}, {"Apple5", MTLGPUFamilyApple5}, {"Apple6", MTLGPUFamilyApple6}, {"Apple7", MTLGPUFamilyApple7},
            {"Apple8", MTLGPUFamilyApple8}, {"Apple9", MTLGPUFamilyApple9}, {"Mac2", MTLGPUFamilyMac2}, {"Common1", MTLGPUFamilyCommon1},
            {"Common2", MTLGPUFamilyCommon2}, {"Common3", MTLGPUFamilyCommon3}, {"Metal3", MTLGPUFamilyMetal3}};
        say("[families]"); for (unsigned i = 0; i < sizeof fam / sizeof fam[0]; i++) if ([dev supportsFamily:fam[i].f]) say(" %s", fam[i].n); say("\n");
    });
    PHASE("bc_query", {
        bc = [dev respondsToSelector:@selector(supportsBCTextureCompression)] ? (int)dev.supportsBCTextureCompression : -1;
        if (getenv("E2DBG_NO_BC")) { bc = 0; say("[bc] E2DBG_NO_BC: acting as a device without BC (CI check)\n"); }
        say("[bc] supportsBCTextureCompression: %s\n", bc < 0 ? "no such API" : bc ? "YES" : "NO");
    });
    PHASE("properties", {
        say("[properties] unified %d, recommendedMaxWorkingSetSize %.0f MB, maxBufferLength %.0f MB, threadgroup memory %lu KB\n",
            dev.hasUnifiedMemory, dev.recommendedMaxWorkingSetSize / (double)MB, dev.maxBufferLength / (double)MB, (unsigned long)(dev.maxThreadgroupMemoryLength / 1024));
        say("[properties] argument buffers tier %ld, read-write textures tier %ld, raytracing %d, 32-bit float filtering %d\n",
            (long)dev.argumentBuffersSupport + 1, (long)dev.readWriteTextureSupport, dev.supportsRaytracing, dev.supports32BitFloatFiltering);
    });
    PHASE("sparse_tile", { say("[properties] sparse tile %lu bytes\n", (unsigned long)dev.sparseTileSizeInBytes); });
    PHASE("shader_compile", {
        NSError *err = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&err];
        probe = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"probe"] error:&err] : nil;
        say("[shader] runtime compile: %s\n", probe ? "ok" : err.localizedDescription.UTF8String);
    });
    if (probe) {
        const float wr[4] = {200 / 255.f, 100 / 255.f, 50 / 255.f, 1}, wa[4] = {32 / 255.f, 96 / 255.f, 160 / 255.f, 224 / 255.f};
        const float w1[4] = {1, 0, 0, 1}, w3[4] = {1, 0, 0, 128 / 255.f}, w7[4] = {64 / 255.f, 128 / 255.f, 192 / 255.f, 254 / 255.f};
        say("[formats] 8x8 texture of a known colour: create, upload, sample on the GPU\n");
        PHASE("fmt_rgba8", { bad += decode_check("RGBA8", MTLPixelFormatRGBA8Unorm, 32, 256, blk_rgba8, wr); });
        PHASE("fmt_astc4x4", { ci_crash("fmt_astc4x4"); bad += decode_check("ASTC 4x4 LDR", MTLPixelFormatASTC_4x4_LDR, 32, 64, blk_astc, wa); });
        if (bc == 1) {
            PHASE("fmt_bc1", { bad += decode_check("BC1", MTLPixelFormatBC1_RGBA, 16, 32, blk_bc1, w1); });
            PHASE("fmt_bc3", { bad += decode_check("BC3", MTLPixelFormatBC3_RGBA, 32, 64, blk_bc3, w3); });
            PHASE("fmt_bc7", { bad += decode_check("BC7", MTLPixelFormatBC7_RGBAUnorm, 32, 64, blk_bc7, w7); });
        } else say("  BC1 / BC3 / BC7: UNSUPPORTED, skipped (the device does not report supportsBCTextureCompression; no BC texture is created)\n");
    }
    uint8_t *src = malloc(512 * 512 * 4); for (int i = 0; i < 512 * 512 * 4; i++) src[i] = (uint8_t)(i * 2654435761u >> 24);
    say("[texture memory] 4 textures of 512x512 each\n");
    PHASE("mem_rgba8_private", { tex_mem("RGBA8 private", MTLPixelFormatRGBA8Unorm, 512, 4, 32, MTLStorageModePrivate, src); });
    PHASE("mem_rgba8_shared", { tex_mem("RGBA8 shared", MTLPixelFormatRGBA8Unorm, 512, 4, 32, MTLStorageModeShared, src); });
    PHASE("mem_astc_private", { ci_crash("mem_astc_private"); tex_mem("ASTC4x4 private", MTLPixelFormatASTC_4x4_LDR, 512, 4, 8, MTLStorageModePrivate, src); });
    if (bc == 1) PHASE("mem_bc7_private", { tex_mem("BC7 private", MTLPixelFormatBC7_RGBAUnorm, 512, 4, 8, MTLStorageModePrivate, src); });
    else say("  BC7 memory: UNSUPPORTED, skipped\n");
    PHASE("lazy_vs_upfront", {
        if (room_for(16 * MB)) {
            Mem b = mem(); NSMutableArray *a = [NSMutableArray array]; double t0 = now_ms();
            for (int i = 0; i < 16; i++) { id<MTLTexture> t = make_tex(MTLPixelFormatRGBA8Unorm, 512, 512, MTLStorageModePrivate, src, 2048, MB); if (t) [a addObject:t]; }
            mrow("up front: 16 made", b); say("  %-30s %.1f ms in all\n", "", now_ms() - t0); [a removeAllObjects];
            b = mem(); double worst = 0;
            for (int i = 0; i < 2; i++) { double t1 = now_ms(); id<MTLTexture> t = make_tex(MTLPixelFormatRGBA8Unorm, 512, 512, MTLStorageModePrivate, src, 2048, MB);
                float g[4]; if (t && probe) sample(t, g); if (now_ms() - t1 > worst) worst = now_ms() - t1; [a addObject:t ? t : (id)NSNull.null]; }
            mrow("on first use: 2 made", b); say("  %-30s worst first use %.2f ms\n", "", worst);
        }
    });
    const int RW[] = {1280, 960, 768}, RH[] = {720, 540, 432};
    for (int r = 0; r < 3; r++) for (int ml = 0; ml < 2; ml++) {
        char name[48]; snprintf(name, sizeof name, "rt_%dx%d_%s", RW[r], RH[r], ml ? "memoryless" : "private");
        PHASE(name, {
            if (ml && ![dev supportsFamily:MTLGPUFamilyApple1]) { say("  %s: memoryless needs an Apple GPU family\n", name); }
            else {
                Mem b = mem();
                MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:RW[r] height:RH[r] mipmapped:NO];
                cd.storageMode = MTLStorageModePrivate; cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:RW[r] height:RH[r] mipmapped:NO];
                dd.storageMode = ml ? MTLStorageModeMemoryless : MTLStorageModePrivate; dd.usage = MTLTextureUsageRenderTarget;
                id<MTLTexture> c = new_tex(cd), d = new_tex(dd);
                if (!c || !d) say("  %s: texture refused\n", name);
                else {
                    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
                    p.colorAttachments[0].texture = c; p.colorAttachments[0].loadAction = MTLLoadActionClear; p.colorAttachments[0].storeAction = MTLStoreActionStore;
                    p.depthAttachment.texture = d; p.depthAttachment.loadAction = MTLLoadActionClear; p.depthAttachment.storeAction = MTLStoreActionDontCare;
                    id<MTLCommandBuffer> cb = [q commandBuffer]; [[cb renderCommandEncoderWithDescriptor:p] endEncoding]; [cb commit]; [cb waitUntilCompleted];
                    cb_ok(cb, name); mrow(name, b);
                    say("  %-30s colour %.1f + depth %.1f MB\n", "", c.allocatedSize / (double)MB, d.allocatedSize / (double)MB);
                }
            }
        });
    }
    PHASE("purgeable_textures", {
        if (room_for(16 * MB)) {
            Mem b = mem(); NSMutableArray *a = [NSMutableArray array];
            for (int i = 0; i < 4; i++) { id<MTLTexture> t = make_tex(MTLPixelFormatRGBA8Unorm, 1024, 1024, MTLStorageModePrivate, NULL, 0, 0); if (t) [a addObject:t]; }
            mrow("4 textures made", b);
            for (id<MTLTexture> t in a) [t setPurgeableState:MTLPurgeableStateVolatile];
            mrow("volatile", b);
            for (id<MTLTexture> t in a) [t setPurgeableState:MTLPurgeableStateEmpty];
            mrow("empty", b);
            int lost = 0; for (id<MTLTexture> t in a) lost += [t setPurgeableState:MTLPurgeableStateNonVolatile] == MTLPurgeableStateEmpty;
            say("  %d of %d report their contents discarded\n", lost, (int)a.count);
        }
    });
    PHASE("purgeable_buffer", {
        if (room_for(16 * MB)) {
            Mem b = mem(); id<MTLBuffer> big = [dev newBufferWithLength:16 * MB options:MTLResourceStorageModeShared];
            memset(big.contents, 0x5a, 16 * MB); mrow("16 MB buffer made", b);
            [big setPurgeableState:MTLPurgeableStateVolatile]; mrow("volatile", b);
            [big setPurgeableState:MTLPurgeableStateEmpty]; mrow("empty", b);
            say("  contents %s\n", [big setPurgeableState:MTLPurgeableStateNonVolatile] == MTLPurgeableStateEmpty ? "discarded" : "kept");
        }
    });
    free(src);
    if (getenv("E2DBG_NO_BC")) PHASE("desc_check_selftest", {   // CI: a BC descriptor must stop at the check, never reach Metal
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBC1_RGBA width:8 height:8 mipmapped:NO];
        say("  self-test: BC1 descriptor on a no-BC device -> %s\n", new_tex(d) ? "CREATED (check failed)" : "not created");
        d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatASTC_4x4_LDR width:6 height:8 mipmapped:NO];
        say("  self-test: ASTC 6x8 (not block aligned) -> %s\n", new_tex(d) ? "CREATED (check failed)" : "not created");
    });
    PHASE("release", { mrow("everything released (against start)", start); });
    say("\n[summary] BC1 / BC3 / BC7: %s; ASTC 4x4: %s\n", bc == 1 ? "supported and checked above" : "UNSUPPORTED (not created)",
        [dev supportsFamily:MTLGPUFamilyApple2] ? "supported and checked above" : "UNSUPPORTED (not created)");
    logf_("RUN end\n");
    say("\nE2-Debug RESULT: %s\n", bad ? "FORMAT DECODE ERRORS" : "finished every phase");
    save_report();
    }
    return bad;
}
