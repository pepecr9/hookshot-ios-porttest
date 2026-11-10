// E4-A: what the signing changes, measured. Reservation-only tests (PROT_NONE / vm_allocate, never touched) at
// 4, 6, 8, 12 and 18 GB, the total in 1 GB pieces, the task's address range and memory counters, and the entitlements
// the installed provisioning profile grants (read from the app's embedded.mobileprovision, when there is one).
// The same unsigned IPA signed two ways (with and without the memory capabilities) gives the A/B comparison.
#import <Foundation/Foundation.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

#define GB (1ull << 30)
#define MB (1ull << 20)
static char *o_p; static size_t o_left;
static void say(const char *f, ...) { va_list a; va_start(a, f); int n = vsnprintf(o_p, o_left, f, a); va_end(a); if (n > 0 && (size_t)n < o_left) { o_p += n; o_left -= n; } }
static uint64_t avail(void) {
#if TARGET_OS_IPHONE
    return os_proc_available_memory();
#else
    return 0;
#endif
}
static void counters(const char *when) {
    task_vm_info_data_t v; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&v, &n) != KERN_SUCCESS) { say("[%s] task_info failed\n", when); return; }
    say("[%s] footprint %.1f MB, resident %.1f MB, compressed %.1f MB, avail %.0f MB", when, v.phys_footprint / (double)MB,
        v.resident_size / (double)MB, v.compressed / (double)MB, avail() / (double)MB);
    if (n >= TASK_VM_INFO_REV4_COUNT) say(", limit_bytes_remaining %.0f MB", v.limit_bytes_remaining / (double)MB);
    if (n >= TASK_VM_INFO_REV2_COUNT) say("\n[%s] task address range %#llx - %#llx (%.1f GB)", when, (unsigned long long)v.min_address,
        (unsigned long long)v.max_address, (v.max_address - v.min_address) / (double)GB);
    say("\n");
}
static void profile(void) {
    NSString *p = [[NSBundle mainBundle] pathForResource:@"embedded" ofType:@"mobileprovision"];
    NSData *d = p ? [NSData dataWithContentsOfFile:p] : nil;
    if (!d) { say("[profile] no embedded.mobileprovision (unsigned, ad hoc or a signer that does not embed one)\n"); return; }
    NSRange a = [d rangeOfData:[@"<?xml" dataUsingEncoding:NSUTF8StringEncoding] options:0 range:NSMakeRange(0, d.length)];
    NSRange b = [d rangeOfData:[@"</plist>" dataUsingEncoding:NSUTF8StringEncoding] options:0 range:NSMakeRange(0, d.length)];
    if (a.location == NSNotFound || b.location == NSNotFound) { say("[profile] present, no plist found in it\n"); return; }
    NSData *x = [d subdataWithRange:NSMakeRange(a.location, b.location + b.length - a.location)];
    NSDictionary *pl = [NSPropertyListSerialization propertyListWithData:x options:0 format:NULL error:nil];
    NSDictionary *e = pl[@"Entitlements"];
    say("[profile] team %s, expires %s\n", [[pl[@"TeamName"] description] UTF8String], [[pl[@"ExpirationDate"] description] UTF8String]);
    for (NSString *k in @[@"com.apple.developer.kernel.increased-memory-limit", @"com.apple.developer.kernel.extended-virtual-addressing", @"get-task-allow"])
        say("[profile] %s: %s\n", k.UTF8String, e[k] ? [[e[k] description] UTF8String] : "absent");
}

// the entitlements the executable's own code signature carries (LC_CODE_SIGNATURE -> SuperBlob -> slot 5, an XML plist):
// what the signer actually put in the binary, to compare with what the provisioning profile allows
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void signed_entitlements(void) {
    NSString *exe = [NSBundle mainBundle].executablePath;
    NSData *d = exe ? [NSData dataWithContentsOfFile:exe options:NSDataReadingMappedIfSafe error:nil] : nil;
    const uint8_t *b = d.bytes; size_t n = d.length;
    if (!b || n < sizeof(struct mach_header_64) || ((const struct mach_header_64 *)b)->magic != MH_MAGIC_64) { say("[signature] executable not readable as a thin 64-bit Mach-O\n"); return; }
    const struct mach_header_64 *h = (const struct mach_header_64 *)b; size_t off = sizeof *h; uint32_t csoff = 0, cssize = 0;
    for (uint32_t i = 0; i < h->ncmds && off + sizeof(struct load_command) <= n; i++) {
        const struct load_command *lc = (const struct load_command *)(b + off);
        if (lc->cmd == LC_CODE_SIGNATURE) { const struct linkedit_data_command *c = (const void *)lc; csoff = c->dataoff; cssize = c->datasize; }
        off += lc->cmdsize;
    }
    if (!csoff || (size_t)csoff + cssize > n || cssize < 12 || be32(b + csoff) != 0xfade0cc0) { say("[signature] no code signature in the executable\n"); return; }
    uint32_t count = be32(b + csoff + 8);
    for (uint32_t i = 0; i < count && 12 + 8 * (size_t)i + 8 <= cssize; i++) {
        uint32_t type = be32(b + csoff + 12 + 8 * i), bo = be32(b + csoff + 12 + 8 * i + 4);
        if (type != 5 || (size_t)bo + 8 > cssize) continue;
        const uint8_t *eb = b + csoff + bo; uint32_t len = be32(eb + 4);
        if (be32(eb) != 0xfade7171 || len < 8 || (size_t)bo + len > cssize) break;
        NSDictionary *e = [NSPropertyListSerialization propertyListWithData:[NSData dataWithBytes:eb + 8 length:len - 8] options:0 format:NULL error:nil];
        say("[signature] %lu entitlements in the executable's signature\n", (unsigned long)e.count);
        for (NSString *k in @[@"com.apple.developer.kernel.increased-memory-limit", @"com.apple.developer.kernel.extended-virtual-addressing", @"get-task-allow", @"application-identifier", @"com.example.e4a-ci-check"])
            if (e[k] || ![k hasPrefix:@"com.example"]) say("[signature] %s: %s\n", k.UTF8String, e[k] ? [[e[k] description] UTF8String] : "absent");
        return;
    }
    say("[signature] signed, with no entitlements blob (ad hoc or linker signature)\n");
}

int e4a_all(char *buf, unsigned long cap, int unused_states, int unused_touch) {
    (void)unused_states; (void)unused_touch;
    o_p = buf; o_left = cap; buf[0] = 0;
    @autoreleasepool {
    say("E4-A reservation-only address-space test, %s; nothing below is touched (PROT_NONE / vm_allocate only)\n", TARGET_OS_IPHONE ? "iOS" : "macOS");
    signed_entitlements();
    profile();
    counters("start");
    const uint64_t sizes[] = {4, 6, 8, 12, 18};
    for (unsigned i = 0; i < 5; i++) {
        uint64_t n = sizes[i] * GB;
        void *p = mmap(NULL, n, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0); int e = p == MAP_FAILED ? errno : 0;
        if (p != MAP_FAILED) munmap(p, n);
        vm_address_t va = 0; kern_return_t kr = vm_allocate(mach_task_self(), &va, (vm_size_t)n, VM_FLAGS_ANYWHERE);
        if (kr == KERN_SUCCESS) vm_deallocate(mach_task_self(), va, (vm_size_t)n);
        say("[reserve] %2llu GB: mmap %s", (unsigned long long)sizes[i], e ? "FAILED" : "ok");
        if (e) say(" (errno %d %s)", e, strerror(e));
        say(" | vm_allocate %s", kr == KERN_SUCCESS ? "ok" : "FAILED");
        if (kr != KERN_SUCCESS) say(" (kern %d %s)", kr, mach_error_string(kr));
        say("\n");
    }
    uint64_t lo = 0, hi = 256;   // largest single mmap in 256 MB steps, up to 64 GB
    while (lo < hi) { uint64_t m = (lo + hi + 1) / 2; void *p = mmap(NULL, m * 256 * MB, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0); if (p != MAP_FAILED) { munmap(p, m * 256 * MB); lo = m; } else hi = m - 1; }
    say("[reserve] largest single reservation: %.2f GB\n", lo * 0.25);
    static void *r[64]; int k = 0;
    while (k < 64 && (r[k] = mmap(NULL, GB, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0)) != MAP_FAILED) k++;
    say("[reserve] separate 1 GB reservations held at once: %d%s\n", k, k == 64 ? " (the test's cap)" : "");
    while (k) munmap(r[--k], GB);
    counters("end");
    say("E4-A RESULT: measured (compare this file between the two signings: [signature] is what the binary carries, [profile] what the profile allows)\n");
    }
    return 0;
}
