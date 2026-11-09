// E2: Metal capabilities and texture memory on the GPU at hand (Apple A15 target), with synthetic data only.
// 1 device and families  2 BC / ASTC formats, each checked by decoding a known block on the GPU
// 3 memory of RGBA8 vs ASTC 4x4 (vs BC7 when supported), private vs shared  4 textures made up front vs on first use
// 5 render targets at several resolutions, depth in memory vs memoryless  6 purgeable textures and buffers  7 release.
// Memory: phys_footprint and its graphics / purgeable ledgers (task_vm_info), currentAllocatedSize,
// recommendedMaxWorkingSetSize, allocatedSize. A fixed budget bounds what is alive at once (GPU_BUDGET).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

#define MB (1ull << 20)
#define GPU_BUDGET (192 * MB)       // most Metal memory alive at once
#define AVAIL_FLOOR (800 * MB)      // iOS: never go under this much room to the app's limit

static char *o_p; static size_t o_left;
static void say(const char *f, ...) {
    va_list a; va_start(a, f);
    int n = vsnprintf(o_p, o_left, f, a);
    va_end(a);
    if (n > 0 && (size_t)n < o_left) { o_p += n; o_left -= n; }
}
static double now_ms(void) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}
static uint64_t avail(void) {
#if TARGET_OS_IPHONE
    return os_proc_available_memory();
#else
    return 0;
#endif
}
typedef struct { double foot, gfx, pvol, pnonvol, alloc; } Mem;
static id<MTLDevice> dev;
static Mem mem(void) {
    task_vm_info_data_t v; mach_msg_type_number_t n = TASK_VM_INFO_COUNT; Mem m = {0};
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&v, &n) == KERN_SUCCESS) {
        m.foot = v.phys_footprint / (double)MB;
        if (n >= TASK_VM_INFO_REV3_COUNT) {
            m.gfx = v.ledger_tag_graphics_footprint / (double)MB;
            m.pvol = v.ledger_purgeable_volatile / (double)MB;
            m.pnonvol = v.ledger_purgeable_nonvolatile / (double)MB;
        }
    }
    m.alloc = dev ? dev.currentAllocatedSize / (double)MB : 0;
    return m;
}
static void mrow(const char *what, Mem b) {
    Mem m = mem();
    say("  %-34s foot %+7.1f gfx %+7.1f purg.vol %+6.1f purg.nonvol %+7.1f | MTL allocated %+7.1f MB | avail %5.0f\n",
        what, m.foot - b.foot, m.gfx - b.gfx, m.pvol - b.pvol, m.pnonvol - b.pnonvol, m.alloc - b.alloc, avail() / (double)MB);
}
// may `bytes` more be allocated? (budget and, on iOS, room to the limit)
static int room_for(uint64_t bytes, const char *what) {
    uint64_t a = avail();
    if (bytes > GPU_BUDGET || (a && a < bytes + AVAIL_FLOOR)) {
        say("  SKIP %s: %llu MB over the budget (%llu MB) or the room (%llu MB, floor %llu)\n", what, (unsigned long long)(bytes / MB),
            (unsigned long long)(GPU_BUDGET / MB), (unsigned long long)(a / MB), (unsigned long long)(AVAIL_FLOOR / MB));
        return 0;
    }
    return 1;
}

static id<MTLCommandQueue> q;
static id<MTLComputePipelineState> probe;
static NSString *const kSrc = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "kernel void probe(texture2d<float> t [[texture(0)]], device float4 *o [[buffer(0)]]) {\n"
    "  constexpr sampler s(filter::nearest, coord::normalized);\n"
    "  o[0] = t.sample(s, float2(0.3, 0.3), level(0));\n}\n";

// a private texture filled from a shared staging buffer by a blit; nil when the format is refused
static id<MTLTexture> make_tex(MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLStorageMode sm, const void *data, NSUInteger rowBytes, NSUInteger bytes, id<MTLBuffer> staging) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.storageMode = sm; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [dev newTextureWithDescriptor:d];
    if (!t || !data) return t;
    if (sm == MTLStorageModeShared) { [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:data bytesPerRow:rowBytes]; return t; }
    id<MTLBuffer> src = staging;
    if (!src) src = [dev newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
    else memcpy(src.contents, data, bytes);
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromBuffer:src sourceOffset:0 sourceBytesPerRow:rowBytes sourceBytesPerImage:bytes sourceSize:MTLSizeMake(w, h, 1)
            toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return cb.status == MTLCommandBufferStatusCompleted ? t : nil;
}
static int sample(id<MTLTexture> t, float out[4]) {
    id<MTLBuffer> o = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:probe]; [e setTexture:t atIndex:0]; [e setBuffer:o offset:0 atIndex:0];
    [e dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) return 0;
    memcpy(out, o.contents, 16); return 1;
}

// ---- known 4x4 blocks (solid colours) ----
static void setbits(uint64_t b[2], int pos, int n, uint64_t v) { for (int i = 0; i < n; i++) if (v >> i & 1) b[(pos + i) >> 6] |= 1ull << ((pos + i) & 63); }
static void blk_bc1(uint8_t *o) { uint16_t c = 0xF800; memset(o, 0, 8); o[0] = o[2] = c & 255; o[1] = o[3] = c >> 8; }            // red, index 0
static void blk_bc3(uint8_t *o) { memset(o, 0, 16); o[0] = o[1] = 128; blk_bc1(o + 8); }                                          // red, alpha 128
static void blk_bc7(uint8_t *o) {                                                                                                  // mode 6: (64,128,192,254)
    uint64_t b[2] = {0, 0}; setbits(b, 6, 1, 1);
    const int c7[4] = {32, 64, 96, 127};
    for (int ch = 0; ch < 4; ch++) { setbits(b, 7 + ch * 14, 7, c7[ch]); setbits(b, 14 + ch * 14, 7, c7[ch]); }
    memcpy(o, b, 16);
}
static void blk_astc(uint8_t *o) {                                                                                                 // void extent: (32,96,160,224)
    uint64_t b[2] = {0xFFFFFFFFFFFFFDFCull, 0}; const uint16_t c[4] = {32 * 257, 96 * 257, 160 * 257, 224 * 257};
    for (int i = 0; i < 4; i++) b[1] |= (uint64_t)c[i] << (16 * i);
    memcpy(o, b, 16);
}

static int fmt_checks(void) {
    struct { const char *name; MTLPixelFormat f; int bpb; void (*blk)(uint8_t *); float want[4]; } F[] = {
        {"BC1 RGBA", MTLPixelFormatBC1_RGBA, 8, blk_bc1, {1, 0, 0, 1}},
        {"BC3 RGBA", MTLPixelFormatBC3_RGBA, 16, blk_bc3, {1, 0, 0, 128 / 255.f}},
        {"BC7 RGBA unorm", MTLPixelFormatBC7_RGBAUnorm, 16, blk_bc7, {64 / 255.f, 128 / 255.f, 192 / 255.f, 254 / 255.f}},
        {"ASTC 4x4 LDR", MTLPixelFormatASTC_4x4_LDR, 16, blk_astc, {32 / 255.f, 96 / 255.f, 160 / 255.f, 224 / 255.f}},
    };
    const char *other[] = {"BC2", "BC4 R", "BC5 RG", "BC6H RGB float"};
    MTLPixelFormat of[] = {MTLPixelFormatBC2_RGBA, MTLPixelFormatBC4_RUnorm, MTLPixelFormatBC5_RGUnorm, MTLPixelFormatBC6H_RGBFloat};
    int bad = 0;
    say("\n[2] compressed formats: create 8x8 (2x2 blocks of one known colour), upload, sample on the GPU\n");
    for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++) {
        uint8_t data[4 * 16]; for (int k = 0; k < 4; k++) F[i].blk(data + k * F[i].bpb);
        id<MTLTexture> t = nil;
        @try { t = make_tex(F[i].f, 8, 8, MTLStorageModePrivate, data, 2 * F[i].bpb, 4 * F[i].bpb, nil); }
        @catch (NSException *x) { say("  %-16s refused with an exception: %s\n", F[i].name, x.reason.UTF8String); continue; }
        if (!t) { say("  %-16s texture refused (nil)\n", F[i].name); continue; }
        float g[4] = {0}; int ok = sample(t, g), match = ok;
        for (int c = 0; c < 4 && ok; c++) if (fabsf(g[c] - F[i].want[c]) > 2.5f / 255) match = 0;
        say("  %-16s created; sampled %.3f %.3f %.3f %.3f, expected %.3f %.3f %.3f %.3f: %s\n", F[i].name, g[0], g[1], g[2], g[3],
            F[i].want[0], F[i].want[1], F[i].want[2], F[i].want[3], !ok ? "GPU command failed" : match ? "decodes correctly" : "WRONG");
        if (ok && !match) bad++;
    }
    for (int i = 0; i < 4; i++) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:of[i] width:8 height:8 mipmapped:NO];
        d.storageMode = MTLStorageModePrivate; id<MTLTexture> t = nil;
        @try { t = [dev newTextureWithDescriptor:d]; } @catch (NSException *x) { t = nil; }
        say("  %-16s %s (creation only)\n", other[i], t ? "created" : "refused");
    }
    return bad;
}

static void info(void) {
    say("\n[1] device: %s\n", dev.name.UTF8String);
    struct { const char *n; long v; } fam[] = {{"Apple1", 1001}, {"Apple2", 1002}, {"Apple3", 1003}, {"Apple4", 1004}, {"Apple5", 1005},
        {"Apple6", 1006}, {"Apple7", 1007}, {"Apple8", 1008}, {"Apple9", 1009}, {"Apple10", 1010}, {"Mac2", 2002},
        {"Common1", 3001}, {"Common2", 3002}, {"Common3", 3003}, {"Metal3", 5001}, {"Metal4", 5002}};
    say("  families:");
    for (unsigned i = 0; i < sizeof fam / sizeof fam[0]; i++) if ([dev supportsFamily:(MTLGPUFamily)fam[i].v]) say(" %s", fam[i].n);
    say("\n");
    int bc = [dev respondsToSelector:@selector(supportsBCTextureCompression)] ? (int)dev.supportsBCTextureCompression : -1;
    say("  supportsBCTextureCompression %s\n", bc < 0 ? "(no such API)" : bc ? "YES" : "NO");
    say("  unified memory %s, recommendedMaxWorkingSetSize %.0f MB, maxBufferLength %.0f MB, currentAllocatedSize %.1f MB\n",
        dev.hasUnifiedMemory ? "yes" : "no", dev.recommendedMaxWorkingSetSize / (double)MB, dev.maxBufferLength / (double)MB, dev.currentAllocatedSize / (double)MB);
    MTLSize tg = dev.maxThreadsPerThreadgroup;
    say("  threadgroup max %lux%lux%lu, threadgroup memory %lu KB, argument buffers tier %ld, read-write textures tier %ld\n",
        (unsigned long)tg.width, (unsigned long)tg.height, (unsigned long)tg.depth, (unsigned long)(dev.maxThreadgroupMemoryLength / 1024),
        (long)dev.argumentBuffersSupport + 1, (long)dev.readWriteTextureSupport);
    say("  raytracing %s, function pointers %s, 32-bit float filtering %s, 32-bit MSAA %s, query texture LOD %s, sparse tile %lu bytes\n",
        dev.supportsRaytracing ? "yes" : "no", dev.supportsFunctionPointers ? "yes" : "no", dev.supports32BitFloatFiltering ? "yes" : "no",
        dev.supports32BitMSAA ? "yes" : "no", dev.supportsQueryTextureLOD ? "yes" : "no", (unsigned long)dev.sparseTileSizeInBytes);
    say("  mesh shaders (Apple7+ or Mac2): %s; memoryless render targets (Apple families): %s\n",
        [dev supportsFamily:MTLGPUFamilyApple7] || [dev supportsFamily:MTLGPUFamilyMac2] ? "yes" : "no", [dev supportsFamily:MTLGPUFamilyApple1] ? "yes" : "no");
}

static uint8_t *noise(size_t n) { uint8_t *p = malloc(n); uint32_t s = 12345; for (size_t i = 0; i < n; i++) { s = s * 1664525 + 1013904223; p[i] = s >> 24; } return p; }

// [3] N textures of WxW in one format; returns 0 when skipped
static void tex_set(const char *label, MTLPixelFormat f, NSUInteger W, int N, double bpp, MTLStorageMode sm, id<MTLBuffer> staging, uint8_t *src) {
    uint64_t each = (uint64_t)(W * W * bpp / 8), all = each * N;
    if (!room_for(all, label)) return;
    NSUInteger rowBytes = f == MTLPixelFormatRGBA8Unorm ? W * 4 : (W / 4) * 16;
    @autoreleasepool {
        Mem b = mem(); NSMutableArray *a = [NSMutableArray array]; double t0 = now_ms(); NSUInteger alloc = 0;
        for (int i = 0; i < N; i++) {
            id<MTLTexture> t = make_tex(f, W, W, sm, src, rowBytes, each, staging);
            if (!t) { say("  %s: texture %d refused\n", label, i); break; }
            [a addObject:t]; alloc += t.allocatedSize;
        }
        char l[96]; snprintf(l, sizeof l, "%s %dx %lu^2 (%.0f MB data)", label, (int)a.count, (unsigned long)W, all / (double)MB);
        mrow(l, b);
        say("  %-34s allocatedSize %.1f MB in all, %.2f ms per texture\n", "", alloc / (double)MB, (now_ms() - t0) / (a.count ? a.count : 1));
        [a removeAllObjects];
    }
}

static int cmpd(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }

int e2_all(char *buf, unsigned long cap, int unused_states, int unused_touch) {
    (void)unused_states; (void)unused_touch;
    o_p = buf; o_left = cap; buf[0] = 0;
    int bad = 0;
    @autoreleasepool {
    say("E2 Metal capabilities and texture memory, %s, synthetic data only, budget %llu MB alive at once\n",
        TARGET_OS_IPHONE ? "iOS" : "macOS", (unsigned long long)(GPU_BUDGET / MB));
    dev = MTLCreateSystemDefaultDevice();
    if (!dev) { say("no Metal device: nothing to measure\n"); return 0; }
    q = [dev newCommandQueue];
    NSError *err = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&err];
    probe = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"probe"] error:&err] : nil;
    if (!probe) { say("shader compile failed: %s\n", err.localizedDescription.UTF8String); return 1; }
    Mem start = mem();
    say("start: footprint %.1f MB, graphics %.1f MB, MTL allocated %.1f MB, avail %.0f MB\n", start.foot, start.gfx, start.alloc, avail() / (double)MB);
    info();
    bad += fmt_checks();

    // [3] RGBA8 vs ASTC 4x4 vs BC7, 16 textures of 1024^2, noise content (memory does not depend on content)
    say("\n[3] texture memory by format and storage (16 textures of 1024x1024, deltas against before the set)\n");
    uint8_t *src = noise(1024 * 1024 * 4);
    id<MTLBuffer> staging = [dev newBufferWithLength:1024 * 1024 * 4 options:MTLResourceStorageModeShared];
    tex_set("RGBA8 private", MTLPixelFormatRGBA8Unorm, 1024, 16, 32, MTLStorageModePrivate, staging, src);
    tex_set("RGBA8 shared", MTLPixelFormatRGBA8Unorm, 1024, 16, 32, MTLStorageModeShared, staging, src);
    tex_set("ASTC4x4 private", MTLPixelFormatASTC_4x4_LDR, 1024, 16, 8, MTLStorageModePrivate, staging, src);
    if ([dev respondsToSelector:@selector(supportsBCTextureCompression)] && dev.supportsBCTextureCompression)
        tex_set("BC7 private", MTLPixelFormatBC7_RGBAUnorm, 1024, 16, 8, MTLStorageModePrivate, staging, src);
    else say("  BC7: not supported here, skipped\n");
    mrow("after the sets were released", start);

    // [4] made up front vs on first use: 96 textures of 512^2 RGBA8 (1 MB each); a "scene" reads 10 of them
    say("\n[4] 96 textures of 512x512 RGBA8: all made up front vs only the 10 a scene reads, on first use\n");
    if (room_for(96 * MB, "up-front set")) @autoreleasepool {
        Mem b = mem(); double t0 = now_ms(); NSMutableArray *a = [NSMutableArray array];
        for (int i = 0; i < 96; i++) { id<MTLTexture> t = make_tex(MTLPixelFormatRGBA8Unorm, 512, 512, MTLStorageModePrivate, src, 2048, MB, staging); if (t) [a addObject:t]; }
        double tu = now_ms() - t0; mrow("up front: 96 made", b);
        say("  %-34s %.0f ms to make and upload all (%.2f ms each)\n", "", tu, tu / 96);
        [a removeAllObjects];
    }
    @autoreleasepool {
        Mem b = mem(); NSMutableDictionary *made = [NSMutableDictionary dictionary]; double lat[10]; int nl = 0; uint32_t s = 777;
        while (nl < 10) {
            s = s * 1664525 + 1013904223; NSNumber *k = @(s % 96);
            if (made[k]) continue;
            double t0 = now_ms();
            id<MTLTexture> t = make_tex(MTLPixelFormatRGBA8Unorm, 512, 512, MTLStorageModePrivate, src, 2048, MB, staging);
            float g[4]; if (t) sample(t, g);   // the first read waits for the texture
            lat[nl++] = now_ms() - t0; if (t) made[k] = t;
        }
        qsort(lat, 10, sizeof lat[0], cmpd);
        mrow("on first use: 10 made", b);
        say("  %-34s first-use latency (make + upload + first read) p50 %.2f ms, max %.2f ms\n", "", lat[5], lat[9]);
        [made removeAllObjects];
    }

    // [5] render targets: RGBA16F colour + Depth32F, cleared once so their memory is really there
    say("\n[5] render targets: RGBA16Float colour + Depth32Float depth, one clear pass each\n");
    const int RW[] = {1920, 1280, 960, 768}, RH[] = {1080, 720, 540, 432};
    int memless = [dev supportsFamily:MTLGPUFamilyApple1];
    for (int r = 0; r < 4; r++) for (int ml = 0; ml <= memless; ml++) @autoreleasepool {
        uint64_t need = (uint64_t)RW[r] * RH[r] * (8 + (ml ? 0 : 4));
        if (!room_for(need, "render target")) continue;
        Mem b = mem();
        MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:RW[r] height:RH[r] mipmapped:NO];
        cd.storageMode = MTLStorageModePrivate; cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:RW[r] height:RH[r] mipmapped:NO];
        dd.storageMode = ml ? MTLStorageModeMemoryless : MTLStorageModePrivate; dd.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> c = [dev newTextureWithDescriptor:cd], d = [dev newTextureWithDescriptor:dd];
        if (!c || !d) { say("  %dx%d: refused\n", RW[r], RH[r]); continue; }
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = c; p.colorAttachments[0].loadAction = MTLLoadActionClear; p.colorAttachments[0].storeAction = MTLStoreActionStore;
        p.depthAttachment.texture = d; p.depthAttachment.loadAction = MTLLoadActionClear; p.depthAttachment.storeAction = MTLStoreActionDontCare;
        id<MTLCommandBuffer> cb = [q commandBuffer]; [[cb renderCommandEncoderWithDescriptor:p] endEncoding]; [cb commit]; [cb waitUntilCompleted];
        char l[96]; snprintf(l, sizeof l, "%dx%d depth %s", RW[r], RH[r], ml ? "memoryless" : "private");
        mrow(l, b);
        say("  %-34s allocatedSize colour %.1f + depth %.1f MB (pass %s)\n", "", c.allocatedSize / (double)MB, d.allocatedSize / (double)MB,
            cb.status == MTLCommandBufferStatusCompleted ? "ok" : "FAILED");
    }

    // [6] purgeable: 64 MB of textures and 64 MB of buffer, volatile then empty
    say("\n[6] purgeable resources: 16 RGBA8 1024^2 private textures (64 MB) and one 64 MB shared buffer\n");
    if (room_for(128 * MB, "purgeable set")) @autoreleasepool {
        Mem b = mem(); NSMutableArray *a = [NSMutableArray array];
        for (int i = 0; i < 16; i++) { id<MTLTexture> t = make_tex(MTLPixelFormatRGBA8Unorm, 1024, 1024, MTLStorageModePrivate, src, 4096, 4 * MB, staging); if (t) [a addObject:t]; }
        id<MTLBuffer> big = [dev newBufferWithLength:64 * MB options:MTLResourceStorageModeShared];
        memset(big.contents, 0x5a, 64 * MB);
        mrow("made, non-volatile", b);
        for (id<MTLTexture> t in a) [t setPurgeableState:MTLPurgeableStateVolatile];
        [big setPurgeableState:MTLPurgeableStateVolatile];
        mrow("set volatile", b);
        for (id<MTLTexture> t in a) [t setPurgeableState:MTLPurgeableStateEmpty];
        [big setPurgeableState:MTLPurgeableStateEmpty];
        mrow("set empty", b);
        int lost = 0;
        for (id<MTLTexture> t in a) lost += [t setPurgeableState:MTLPurgeableStateNonVolatile] == MTLPurgeableStateEmpty;
        int blost = [big setPurgeableState:MTLPurgeableStateNonVolatile] == MTLPurgeableStateEmpty;
        say("  back to non-volatile: %d of %d textures and %s report their contents discarded (must be uploaded again)\n", lost, (int)a.count, blost ? "the buffer" : "not the buffer");
        mrow("non-volatile again (no re-upload)", b);
        [a removeAllObjects]; big = nil;
    }
    staging = nil; free(src);

    say("\n[7] release\n");
    mrow("everything released (against start)", start);
    say("\nE2 RESULT: %s\n", bad ? "FORMAT DECODE ERRORS" : "measurements complete, decode checks passed");
    }
    return bad;
}
