// What the PS4's libc (OpenOrbis musl) lacks (included via runtime/platform.h on the PS4 only).
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

// pthread_cond_wait that also returns after ms: on the PS4 a wake-up can get lost while a signal (the sampler's SIGPROF
// every ms, SAVAGEWIND_PS4_SAMPLE=1) interrupts the wait - TriAevum PS4 recovered ~20 a second that way. Every caller
// waits in a loop that rechecks its condition, as pthread_cond_wait's spurious wake-ups require anyway.
static inline void plat_cond_wait_ms(pthread_cond_t *cv, pthread_mutex_t *m, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(cv, m, &ts);
}

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
