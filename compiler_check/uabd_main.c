#include <stdint.h>
#include <stdio.h>
void f_O0(uint64_t *, const uint64_t *, const uint64_t *);
void f_O2(uint64_t *, const uint64_t *, const uint64_t *);
int main(void) {
    uint64_t s = 0x9E3779B97F4A7C15ull, bad0 = 0, bad2 = 0;
    for (int t = 0; t < 100000; t++) {
        uint64_t a[2], b[2], r0[2], r2[2];
        for (int k = 0; k < 2; k++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; a[k] = s; s ^= s << 13; s ^= s >> 7; s ^= s << 17; b[k] = s; }
        f_O0(r0, a, b); f_O2(r2, a, b);
        for (int j = 0; j < 16; j++) {
            unsigned x = ((const uint8_t *)a)[j], y = ((const uint8_t *)b)[j], ref = x > y ? x - y : y - x;
            bad0 += ((const uint8_t *)r0)[j] != ref;
            bad2 += ((const uint8_t *)r2)[j] != ref;
        }
    }
    printf("UABD body: -O0 wrong lanes %llu, -O2 wrong lanes %llu (of 1600000)\n", (unsigned long long)bad0, (unsigned long long)bad2);
    return (bad0 || bad2) ? 1 : 0;
}
