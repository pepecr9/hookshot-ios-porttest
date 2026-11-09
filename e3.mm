// E3: BC1 / BC3 / BC7 -> ASTC 4x4 on the device, for GPUs without BC (Apple A15). Synthetic textures only.
// The BC data is made in CI (etcpak) from synthetic images and shipped in the app (e3_data/). On the device:
// BC -> RGBA8 with bcdec (CPU) -> ASTC 4x4 with ARM astcenc (CPU; presets fastest / fast / medium, 1 thread and all) ->
// Metal ASTC texture -> every texel read back through the GPU sampler -> PSNR against the original and the BC decode.
// The alternative without re-encoding (upload the RGBA8 decode) is measured too. No BC texture is ever created in Metal.
// Measures time per stage, megapixels a second, temporary memory (footprint deltas), final GPU memory (allocatedSize).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdarg.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#include "astcenc.h"
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

#define MB (1ull << 20)
#define AVAIL_FLOOR (900 * MB)
static const int N = 512;   // image side: 0.26 MPix, small enough for any budget

static char *o_buf, *o_p; static size_t o_left;
static void say(const char *f, ...) {
    va_list a; va_start(a, f); int n = vsnprintf(o_p, o_left, f, a); va_end(a);
    if (n > 0 && (size_t)n < o_left) { o_p += n; o_left -= n; }
}
static double now_ms(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6; }
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
static NSString *outpath;
static void save(void) { if (outpath) [[NSString stringWithUTF8String:o_buf] writeToFile:outpath atomically:YES encoding:NSUTF8StringEncoding error:nil]; }

static double psnr(const uint8_t *a, const uint8_t *b, int channels) {   // RGBA8 images, first `channels` channels
    double s = 0; size_t n = (size_t)N * N;
    for (size_t i = 0; i < n; i++) for (int c = 0; c < channels; c++) { double d = (double)a[4 * i + c] - b[4 * i + c]; s += d * d; }
    double m = s / (n * channels);
    return m == 0 ? 99.0 : 10 * log10(255.0 * 255.0 / m);
}
static NSData *load(NSString *dir, NSString *name) { return [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:name]]; }

// ---- Metal: upload, then read every texel back through the sampler ----
static id<MTLDevice> dev; static id<MTLCommandQueue> q; static id<MTLComputePipelineState> readback;
static NSString *const kSrc = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "kernel void readback(texture2d<float> t [[texture(0)]], device uchar4 *o [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
    "  if (g.x >= t.get_width() || g.y >= t.get_height()) return;\n"
    "  constexpr sampler s(filter::nearest, coord::pixel);\n"
    "  float4 c = t.sample(s, float2(g) + 0.5, level(0));\n"
    "  o[g.y * t.get_width() + g.x] = uchar4(round(saturate(c) * 255.0));\n}\n";
// texture (ASTC_4x4_LDR or RGBA8Unorm only: both documented for Apple GPUs) -> RGBA8 copy as the GPU samples it; returns allocatedSize
static NSUInteger gpu_roundtrip(MTLPixelFormat f, const void *data, NSUInteger rowBytes, NSUInteger bytes, uint8_t *out, double *ms) {
    if (f != MTLPixelFormatASTC_4x4_LDR && f != MTLPixelFormatRGBA8Unorm) return 0;   // never anything else (see E2's crash)
    double t0 = now_ms();
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:N height:N mipmapped:NO];
    d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [dev newTextureWithDescriptor:d];
    id<MTLBuffer> src = [dev newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> dst = [dev newBufferWithLength:(NSUInteger)N * N * 4 options:MTLResourceStorageModeShared];
    if (!t || !src || !dst) return 0;
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromBuffer:src sourceOffset:0 sourceBytesPerRow:rowBytes sourceBytesPerImage:bytes sourceSize:MTLSizeMake(N, N, 1)
            toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [b endEncoding];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:readback]; [e setTexture:t atIndex:0]; [e setBuffer:dst offset:0 atIndex:0];
    [e dispatchThreadgroups:MTLSizeMake(N / 16, N / 16, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    *ms = now_ms() - t0;
    if (cb.status != MTLCommandBufferStatusCompleted) { say("    GPU command failed: %s\n", cb.error.localizedDescription.UTF8String); return 0; }
    memcpy(out, dst.contents, (size_t)N * N * 4);
    return t.allocatedSize;
}

// ---- ASTC encode with astcenc, `threads` workers ----
static int astc_encode(const uint8_t *rgba, float preset, unsigned threads, uint8_t *out, size_t outlen, double *ms, double *ctx_mb) {
    astcenc_config cfg;
    if (astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, preset, 0, &cfg) != ASTCENC_SUCCESS) return 0;
    double f0 = foot_mb();
    astcenc_context *ctx = NULL;
    if (astcenc_context_alloc(&cfg, threads, &ctx) != ASTCENC_SUCCESS) return 0;
    *ctx_mb = foot_mb() - f0;
    astcenc_image img; void *slice = (void *)rgba;
    img.dim_x = N; img.dim_y = N; img.dim_z = 1; img.data_type = ASTCENC_TYPE_U8; img.data = &slice;
    const astcenc_swizzle sw = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    astcenc_image *pimg = &img; __block int ok = 1; double t0 = now_ms();   // (a block captures img as const)
    dispatch_apply(threads, DISPATCH_APPLY_AUTO, ^(size_t i) {
        if (astcenc_compress_image(ctx, pimg, &sw, out, outlen, (unsigned)i) != ASTCENC_SUCCESS) ok = 0;
    });
    *ms = now_ms() - t0;
    astcenc_context_free(ctx);
    return ok;
}

extern "C" int e3_all(char *buf, unsigned long cap, int unused_states, int unused_touch) {
    (void)unused_states; (void)unused_touch;
    o_buf = o_p = buf; o_left = cap; buf[0] = 0;
    int bad = 0;
    @autoreleasepool {
#if TARGET_OS_IPHONE
    NSString *dir = [[NSBundle mainBundle].resourcePath stringByAppendingPathComponent:@"e3_data"];
    outpath = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:@"e3_astc.txt"];
#else
    NSString *dir = getenv("E3_DATA") ? @(getenv("E3_DATA")) : @"e3_data";
    outpath = @"e3_astc_partial.txt";
#endif
    unsigned cores = (unsigned)NSProcessInfo.processInfo.activeProcessorCount;
    say("E3 BC -> ASTC 4x4 on the device, %s, %u cores, %dx%d synthetic images, no BC texture is created in Metal\n",
        TARGET_OS_IPHONE ? "iOS" : "macOS", cores, N, N);
    dev = MTLCreateSystemDefaultDevice();
    if (!dev) { say("no Metal device\n"); return 1; }
    int hasbc = [dev respondsToSelector:@selector(supportsBCTextureCompression)] && dev.supportsBCTextureCompression;
    int hasastc = [dev supportsFamily:MTLGPUFamilyApple2];
    say("GPU %s: BC %s (not used either way), ASTC %s; footprint %.1f MB, avail %.0f MB\n", dev.name.UTF8String,
        hasbc ? "supported" : "UNSUPPORTED", hasastc ? "supported" : "UNSUPPORTED", foot_mb(), avail() / (double)MB);
    if (!hasastc) { say("no ASTC on this GPU: nothing to validate\n"); save(); return 1; }
    q = [dev newCommandQueue];
    NSError *err = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&err];
    readback = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"readback"] error:&err] : nil;
    if (!readback) { say("shader compile failed: %s\n", err.localizedDescription.UTF8String); save(); return 1; }
    if (avail() && avail() < AVAIL_FLOOR + 64 * MB) { say("only %.0f MB of room: not starting\n", avail() / (double)MB); save(); return 1; }

    const size_t px = (size_t)N * N, astc_len = px;   // ASTC 4x4: 16 bytes a 4x4 block = 1 byte a pixel
    uint8_t *rgba = (uint8_t *)malloc(px * 4), *astc = (uint8_t *)malloc(astc_len), *back = (uint8_t *)malloc(px * 4);
    say("buffers: decode %.2f MB, ASTC %.2f MB, readback %.2f MB\n", px * 4 / (double)MB, astc_len / (double)MB, px * 4 / (double)MB);
    struct { const char *name; float preset; } P[] = {{"fastest", ASTCENC_PRE_FASTEST}, {"fast", ASTCENC_PRE_FAST}, {"medium", ASTCENC_PRE_MEDIUM}};
    const char *imgs[] = {"smooth", "detail"}, *fmts[] = {"bc1", "bc3", "bc7"};
    double fast1_mpix[2][3] = {{0}};

    for (int im = 0; im < 2; im++) {
        NSData *orig = load(dir, [NSString stringWithFormat:@"%s.rgba", imgs[im]]);
        if (orig.length != px * 4) { say("missing %s.rgba in %s\n", imgs[im], dir.UTF8String); bad++; continue; }
        const uint8_t *O = (const uint8_t *)orig.bytes;
        // the RGBA8 original through the GPU: the reference path and the "no compression" memory
        double gms; NSUInteger gsz = gpu_roundtrip(MTLPixelFormatRGBA8Unorm, O, N * 4, px * 4, back, &gms);
        say("\n== %s: RGBA8 upload + GPU readback %.1f ms, PSNR %.2f dB (must be 99 = exact), GPU %.2f MB\n", imgs[im], gms, psnr(O, back, 4), gsz / (double)MB);
        if (psnr(O, back, 4) < 99) { say("   RGBA8 readback is not exact: the readback path is wrong\n"); bad++; }
        for (int fm = 0; fm < 3; fm++) {
            NSData *bc = load(dir, [NSString stringWithFormat:@"%s.%s", imgs[im], fmts[fm]]);
            size_t blk = fm == 0 ? 8 : 16, want = (size_t)(N / 4) * (N / 4) * blk;
            if (bc.length != want) { say("  %s: missing or wrong size (%lu, want %zu)\n", fmts[fm], (unsigned long)bc.length, want); bad++; continue; }
            int ch = fm == 0 ? 3 : 4;   // BC1 data here is opaque: colour only
            double f0 = foot_mb(), t0 = now_ms();
            const uint8_t *B = (const uint8_t *)bc.bytes;
            for (int by = 0; by < N / 4; by++) for (int bx = 0; bx < N / 4; bx++) {
                const uint8_t *s = B + ((size_t)by * (N / 4) + bx) * blk; uint8_t *d = rgba + ((size_t)by * 4 * N + bx * 4) * 4;
                if (fm == 0) bcdec_bc1(s, d, N * 4); else if (fm == 1) bcdec_bc3(s, d, N * 4); else bcdec_bc7(s, d, N * 4);
            }
            double tdec = now_ms() - t0, p_bc = psnr(O, rgba, ch);
            say("  %s (%zu KB): CPU decode %.2f ms (%.0f MPix/s), PSNR vs original %.2f dB\n", fmts[fm], want / 1024, tdec, px / 1e6 / (tdec / 1e3), p_bc);
            // alternative A: keep the decode as RGBA8 (no re-encode)
            gsz = gpu_roundtrip(MTLPixelFormatRGBA8Unorm, rgba, N * 4, px * 4, back, &gms);
            say("    A. RGBA8 (no re-encode)  GPU %.2f MB (x%.0f the BC size), GPU sampled PSNR %.2f dB, upload+readback %.1f ms\n",
                gsz / (double)MB, (double)gsz / want, psnr(O, back, ch), gms);
            // alternative B: re-encode to ASTC 4x4
            for (int p = 0; p < 3; p++) for (int th = 0; th < 2; th++) {
                unsigned threads = th ? cores : 1;
                if (p == 2 && th == 0) continue;   // medium on one thread: slow and adds nothing
                double tenc, ctx_mb;
                if (!astc_encode(rgba, P[p].preset, threads, astc, astc_len, &tenc, &ctx_mb)) { say("    astcenc failed (%s)\n", P[p].name); bad++; continue; }
                double peak = foot_mb() - f0;
                gsz = gpu_roundtrip(MTLPixelFormatASTC_4x4_LDR, astc, (N / 4) * 16, astc_len, back, &gms);
                double pg = psnr(O, back, ch), pb = psnr(rgba, back, ch);
                if (!gsz) { bad++; continue; }
                if (p == 1 && th == 0) fast1_mpix[im][fm] = px / 1e6 / (tenc / 1e3);
                say("    B. ASTC %-7s %2u thr  encode %7.1f ms (%6.2f MPix/s), context +%.1f MB, temp footprint +%.1f MB | GPU %.2f MB | sampled PSNR %.2f vs original, %.2f vs BC decode\n",
                    P[p].name, threads, tenc, px / 1e6 / (tenc / 1e3), ctx_mb, peak, gsz / (double)MB, pg, pb);
                if (pg < p_bc - 3.0) { say("      quality drop over 3 dB against the BC source\n"); }
            }
            save();
        }
    }
    free(rgba); free(astc); free(back);

    // load-time viability: seconds to convert a texture set at the 1-thread "fast" rate measured here
    say("\n[viability] time to convert at the measured 1-thread 'fast' rate (all cores divide it by up to their count):\n");
    for (int fm = 0; fm < 3; fm++) {
        double r = (fast1_mpix[0][fm] + fast1_mpix[1][fm]) / 2;
        if (r <= 0) continue;
        say("  %s: %.2f MPix/s -> 64 MPix (a scene, ~64 MB as BC7) %.0f s; 1000 MPix (~1 GB of BC7) %.0f s\n", fmts[fm], r, 64 / r, 1000 / r);
    }
    say("\nfootprint at the end %.1f MB, avail %.0f MB\n", foot_mb(), avail() / (double)MB);
    say("E3 RESULT: %s\n", bad ? "ERRORS (see above)" : "every ASTC texture was created, sampled on the GPU and compared");
    save();
    }
    return bad;
}
