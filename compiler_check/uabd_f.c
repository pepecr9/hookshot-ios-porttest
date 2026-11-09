/* The exact body Hookshot's lifter emits for UABD v1.16b (gen/lifted.c), as a function. Compiled at -O0 and -O2. */
#include <stdint.h>
#include <string.h>
void NAME(uint64_t *d, const uint64_t *a2, const uint64_t *b2) {
    uint8_t _a[16], _b[16], _r[16];
    memcpy(_a, a2, 16); memcpy(_b, b2, 16);
    for (int _j = 0; _j < 16; ++_j) { int64_t _x = (int64_t)_a[_j] - (int64_t)_b[_j]; uint64_t _abs = (uint64_t)(_x < 0 ? -_x : _x); _r[_j] = (uint8_t)(_abs); }
    memcpy(d, _r, 16);
}
