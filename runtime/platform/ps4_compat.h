// What the PS4's libc (OpenOrbis musl) lacks (included via runtime/platform.h on the PS4 only).
#pragma once
#include <stdint.h>
#include <string.h>

// Guest RNG requests (svcGetInfo random entropy) and nothing security-related: the TSC mixed by splitmix64.
static inline uint32_t arc4random(void) {
    static __thread uint64_t s;
    if (!s) s = __builtin_ia32_rdtsc() | 1;
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)(z ^ (z >> 31));
}
static inline uint32_t arc4random_uniform(uint32_t n) {
    if (n < 2) return 0;
    uint32_t min = -n % n, r;  // reject the low values that would bias the modulo
    do r = arc4random(); while (r < min);
    return r % n;
}

// Where the PS4 build keeps everything it writes (/app0, the package, is read-only), and the player's RomFS image.
#define PS4_DATA_DIR "/data/savagewind"
#define PS4_APP_DIR "/app0"
