// E6-probe: which MetalFX effects this device and OS support, asked through MetalFX's own +supportsDevice: class methods.
// Classes are looked up by name, so an OS without one reports "class absent" instead of failing to launch.
// Nothing is created: only the support queries (frame interpolation needs many inputs; that comes once support is known).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <objc/message.h>
#include <stdarg.h>
#include <TargetConditionals.h>

static char *o_p; static size_t o_left;
static void say(const char *f, ...) { va_list a; va_start(a, f); int n = vsnprintf(o_p, o_left, f, a); va_end(a); if (n > 0 && (size_t)n < o_left) { o_p += n; o_left -= n; } }

int e6probe_all(char *buf, unsigned long cap, int unused_states, int unused_touch) {
    (void)unused_states; (void)unused_touch;
    o_p = buf; o_left = cap; buf[0] = 0;
    @autoreleasepool {
    NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
    say("E6-probe MetalFX support, %s %ld.%ld.%ld\n", TARGET_OS_IPHONE ? "iOS" : "macOS", (long)v.majorVersion, (long)v.minorVersion, (long)v.patchVersion);
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { say("no Metal device\n"); return 0; }
    say("GPU %s; Apple7 %d, Apple8 %d, Apple9 %d, Metal3 %d\n", dev.name.UTF8String, [dev supportsFamily:MTLGPUFamilyApple7],
        [dev supportsFamily:MTLGPUFamilyApple8], [dev supportsFamily:MTLGPUFamilyApple9], [dev supportsFamily:MTLGPUFamilyMetal3]);
    const char *names[] = {"MTLFXSpatialScalerDescriptor", "MTLFXTemporalScalerDescriptor", "MTLFXTemporalDenoisedScalerDescriptor", "MTLFXFrameInterpolatorDescriptor"};
    int interp = -1;
    for (int i = 0; i < 4; i++) {
        Class c = NSClassFromString(@(names[i]));
        if (!c) { say("  %-40s class absent on this OS\n", names[i]); continue; }
        SEL s = @selector(supportsDevice:);
        if (![c respondsToSelector:s]) { say("  %-40s present, no +supportsDevice:\n", names[i]); continue; }
        BOOL ok = ((BOOL (*)(id, SEL, id))objc_msgSend)(c, s, dev);
        say("  %-40s supportsDevice: %s\n", names[i], ok ? "YES" : "NO");
        if (i == 3) interp = ok;
    }
    // the fork's own gate (kid-with-sword-remake nvn_mtl.m fg_on): a Metal 4 queue (GPU family Metal4) and +supportsMetal4FX:
    int metal4 = [dev supportsFamily:(MTLGPUFamily)5002], m4fx = -1;   // 5002 = MTLGPUFamilyMetal4 in the iOS 26 SDK
    Class fi = NSClassFromString(@"MTLFXFrameInterpolatorDescriptor");
    SEL s4 = NSSelectorFromString(@"supportsMetal4FX:");
    if (fi && [fi respondsToSelector:s4]) m4fx = ((BOOL (*)(id, SEL, id))objc_msgSend)(fi, s4, dev);
    say("  GPU family Metal4: %s; MTLFXFrameInterpolatorDescriptor +supportsMetal4FX: %s\n", metal4 ? "YES" : "NO",
        m4fx < 0 ? "(no such method)" : m4fx ? "YES" : "NO");
    say("E6-probe RESULT: frame interpolation (supportsDevice:) %s; the fork's Metal 4 path %s\n",
        interp == 1 ? "SUPPORTED" : interp == 0 ? "NOT supported" : "unavailable",
        metal4 && m4fx == 1 ? "AVAILABLE" : "NOT available (the fork turns frame generation off here)");
    }
    return 0;
}
