// Guest CPU state and helpers used by recompiled code.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct __attribute__((aligned(16))) Cpu {
    uint64_t x[32];          // x0-x30 (x[31] unused; sp is separate)
    uint64_t sp;             // 256
    uint64_t pc;             // 264: target for indirect dispatch
    uint64_t nzcv;           // 272: flags in bits 31..28
    uint64_t tpidr;          // 280
    uint64_t tpidrro;        // 288
    uint64_t excl_addr;      // 296
    uint64_t excl_val;       // 304
    void *thread;            // 312: runtime thread object
    uint64_t fpcr;           // 320: guest FPCR/FPSR (portable FP/SIMD lowering only; the asm path uses the host's)
    uint64_t fpsr;           // 328
    int32_t core_held;       // 336: KThread's core_held and prio (kernel.c), for the game code's dispatch (RT_CALL)
    int32_t prio;            // 340
    uint8_t _pad[512 - 344];
    __uint128_t v[32];       // 512
} Cpu;

_Static_assert(offsetof(Cpu, sp) == 256, "sp");
_Static_assert(offsetof(Cpu, pc) == 264, "pc");
_Static_assert(offsetof(Cpu, nzcv) == 272, "nzcv");
_Static_assert(offsetof(Cpu, fpcr) == 320, "fpcr");
_Static_assert(offsetof(Cpu, fpsr) == 328, "fpsr");
_Static_assert(offsetof(Cpu, v) == 512, "v");

typedef void (*GuestFn)(Cpu *);

// ---- runtime services (runtime/*.c)
void rt_dispatch(Cpu *c);                 // call function at c->pc
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc);
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) __attribute__((noreturn));
void rt_udf(Cpu *c, uint64_t pc) __attribute__((noreturn));
uint64_t rt_ticks(void);

#if defined(__aarch64__)
static inline uint64_t rt_get_fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static inline uint64_t rt_get_fpsr(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }
static inline void rt_set_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
static inline void rt_set_fpsr(uint64_t v) { __asm__ volatile("msr fpsr, %0" :: "r"(v)); }
#else
// Host FP state is not the guest's here: code lifted with --simd=c keeps FPCR/FPSR in Cpu (c->fpcr, zeroed
// with the Cpu), so the host-side reset in thread start-up has nothing to do.
static inline uint64_t rt_get_fpcr(void) { return 0; }
static inline uint64_t rt_get_fpsr(void) { return 0; }
static inline void rt_set_fpcr(uint64_t v) { (void)v; }
static inline void rt_set_fpsr(uint64_t v) { (void)v; }
#endif

// ---- memory: guest addresses are host addresses
typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef uint8_t __attribute__((may_alias)) u8a;

// Function-entry profiler (runtime/fnprof.c, SAVAGEWIND_FNPROF): one predictable branch per call when off.
extern volatile int g_fnprof;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0);
// SW_NO_FNPROF (the PS4, whose sampler takes its place): not even that.
#ifdef SW_NO_FNPROF
#define FN_ENTER(a) ((void)0)
#else
#define FN_ENTER(a) do { if (__builtin_expect(g_fnprof, 0)) fnprof_hit(a, c->x[30], c->x[0]); } while (0)
#endif

// GUEST_VOL: volatile in code generated with registers in locals (tools/promote.py). There a loop polling memory
// (ldr; cbz back: a spin wait on another thread's store) has no other store left, and the C compiler hoisted the load
// out of it (the game hung at start); before, each pass also stored a register to *c, which the load might alias.
// The guest's loads and stores are its compiler's already: emitting each one as written costs little.
#ifndef GUEST_VOL
#define GUEST_VOL
#endif
#define LD8(a)  (*(const GUEST_VOL u8a *)(uintptr_t)(a))
#define LD16(a) (*(const GUEST_VOL u16u *)(uintptr_t)(a))
#define LD32(a) (*(const GUEST_VOL u32u *)(uintptr_t)(a))
#define LD64(a) (*(const GUEST_VOL u64u *)(uintptr_t)(a))
#define ST8(a, v)  (*(GUEST_VOL u8a *)(uintptr_t)(a) = (uint8_t)(v))
#define ST16(a, v) (*(GUEST_VOL u16u *)(uintptr_t)(a) = (uint16_t)(v))
#define ST32(a, v) (*(GUEST_VOL u32u *)(uintptr_t)(a) = (uint32_t)(v))
#define ST64(a, v) (*(GUEST_VOL u64u *)(uintptr_t)(a) = (uint64_t)(v))

// ---- exclusive monitor (emulated with compare-and-swap)
#define EXCL(N, T)                                                                         \
    static inline uint64_t ldx##N(Cpu *c, uint64_t a) {                                    \
        T v = __atomic_load_n((T *)(uintptr_t)a, __ATOMIC_ACQUIRE);                        \
        c->excl_addr = a;                                                                  \
        c->excl_val = v;                                                                   \
        return v;                                                                          \
    }                                                                                      \
    static inline uint32_t stx##N(Cpu *c, uint64_t a, uint64_t v) {                        \
        if (c->excl_addr != a) return 1;                                                   \
        c->excl_addr = ~0ull;                                                              \
        T expected = (T)c->excl_val;                                                       \
        return !__atomic_compare_exchange_n((T *)(uintptr_t)a, &expected, (T)v, 0,         \
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);           \
    }                                                                                      \
    static inline uint64_t lda##N(uint64_t a) {                                            \
        return __atomic_load_n((T *)(uintptr_t)a, __ATOMIC_ACQUIRE);                       \
    }                                                                                      \
    static inline void stl##N(uint64_t a, uint64_t v) {                                    \
        __atomic_store_n((T *)(uintptr_t)a, (T)v, __ATOMIC_RELEASE);                       \
    }
EXCL(8, uint8_t)
EXCL(16, uint16_t)
EXCL(32, uint32_t)
EXCL(64, uint64_t)

// ---- indirect calls and jumps (to c->pc): RT_CALL and RT_TAIL. SW_INLINE_DISPATCH (the PS4, where the game code
// is in the executable and can use the runtime's thread-local cache): each call site looks the target up in the
// thread's cache (runtime/dispatch.c) and calls it itself; rt_dispatch on a miss, or with a better thread waiting
// for this one's core (its preemption point). One indirect branch per site instead of the one in rt_dispatch that
// every indirect call of the game went through, which the CPU could hardly ever predict.
void rt_dispatch(Cpu *c);
typedef struct { uint64_t addr; void (*fn)(Cpu *); } RtDcacheEnt;
#define RT_DCACHE 4096
#ifdef SW_INLINE_DISPATCH
extern __thread RtDcacheEnt rt_dcache[RT_DCACHE] __attribute__((tls_model("initial-exec")));  // (simd_softfma.h)
extern volatile unsigned char g_core_preempt[5];  // (kernel.c)
#define RT_DC_HIT(c, e)                                                                                         \
    (__builtin_expect((e)->addr == (c)->pc, 1) && __builtin_expect(!g_core_preempt[(c)->core_held + 1], 1))
#define RT_CALL(c)                                                                                              \
    do {                                                                                                        \
        RtDcacheEnt *e_ = &rt_dcache[((c)->pc >> 2) & (RT_DCACHE - 1)];                                         \
        if (RT_DC_HIT(c, e_)) e_->fn(c);                                                                        \
        else rt_dispatch(c);                                                                                    \
    } while (0)
#define RT_TAIL(c)                                                                                              \
    do {                                                                                                        \
        RtDcacheEnt *e_ = &rt_dcache[((c)->pc >> 2) & (RT_DCACHE - 1)];                                         \
        if (RT_DC_HIT(c, e_)) __attribute__((musttail)) return e_->fn(c);                                       \
        __attribute__((musttail)) return rt_dispatch(c);                                                        \
    } while (0)
#else
#define RT_CALL(c) rt_dispatch(c)
#define RT_TAIL(c) do { __attribute__((musttail)) return rt_dispatch(c); } while (0)
#endif

// ---- flags (packed NZCV in bits 31..28)
#define FLAG_N(c) (((c)->nzcv >> 31) & 1)
#define FLAG_Z(c) (((c)->nzcv >> 30) & 1)
#define FLAG_C(c) (((c)->nzcv >> 29) & 1)
#define FLAG_V(c) (((c)->nzcv >> 28) & 1)
#define COND_EQ(c) (FLAG_Z(c))
#define COND_NE(c) (!FLAG_Z(c))
#define COND_CS(c) (FLAG_C(c))
#define COND_CC(c) (!FLAG_C(c))
#define COND_MI(c) (FLAG_N(c))
#define COND_PL(c) (!FLAG_N(c))
#define COND_VS(c) (FLAG_V(c))
#define COND_VC(c) (!FLAG_V(c))
#define COND_HI(c) (FLAG_C(c) && !FLAG_Z(c))
#define COND_LS(c) (!(FLAG_C(c) && !FLAG_Z(c)))
#define COND_GE(c) (FLAG_N(c) == FLAG_V(c))
#define COND_LT(c) (FLAG_N(c) != FLAG_V(c))
#define COND_GT(c) (!FLAG_Z(c) && FLAG_N(c) == FLAG_V(c))
#define COND_LE(c) (!(!FLAG_Z(c) && FLAG_N(c) == FLAG_V(c)))

#define MKFLAGS(n, z, cy, v) \
    (((uint64_t)(n) << 31) | ((uint64_t)(z) << 30) | ((uint64_t)(cy) << 29) | ((uint64_t)(v) << 28))

// (on a pointer to the flags: Cpu's own, or a promoted function's local copy, tools/promote.py)
static inline uint64_t adds64_nz(uint64_t *nz, uint64_t a, uint64_t b) {
    uint64_t r = a + b;
    *nz = MKFLAGS(r >> 63, r == 0, r < a, ((a ^ r) & (b ^ r)) >> 63);
    return r;
}
static inline uint64_t subs64_nz(uint64_t *nz, uint64_t a, uint64_t b) {
    uint64_t r = a - b;
    *nz = MKFLAGS(r >> 63, r == 0, a >= b, ((a ^ b) & (a ^ r)) >> 63);
    return r;
}
static inline uint32_t adds32_nz(uint64_t *nz, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    *nz = MKFLAGS(r >> 31, r == 0, r < a, ((a ^ r) & (b ^ r)) >> 31);
    return r;
}
static inline uint32_t subs32_nz(uint64_t *nz, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    *nz = MKFLAGS(r >> 31, r == 0, a >= b, ((a ^ b) & (a ^ r)) >> 31);
    return r;
}
static inline uint64_t logic64_nz(uint64_t *nz, uint64_t r) { *nz = MKFLAGS(r >> 63, r == 0, 0, 0); return r; }
static inline uint32_t logic32_nz(uint64_t *nz, uint32_t r) { *nz = MKFLAGS(r >> 31, r == 0, 0, 0); return r; }
#define adds64(c, a, b) adds64_nz(&(c)->nzcv, a, b)
#define subs64(c, a, b) subs64_nz(&(c)->nzcv, a, b)
#define adds32(c, a, b) adds32_nz(&(c)->nzcv, a, b)
#define subs32(c, a, b) subs32_nz(&(c)->nzcv, a, b)
#define logic64(c, r) logic64_nz(&(c)->nzcv, r)
#define logic32(c, r) logic32_nz(&(c)->nzcv, r)

#define ADC_IMPL(N, S)                                                                     \
    static inline uint##N##_t adc##N##_nz(uint64_t nz, uint##N##_t a, uint##N##_t b) {     \
        return a + b + (uint##N##_t)((nz >> 29) & 1);                                      \
    }                                                                                      \
    static inline uint##N##_t sbc##N##_nz(uint64_t nz, uint##N##_t a, uint##N##_t b) {     \
        return a - b - 1 + (uint##N##_t)((nz >> 29) & 1);                                  \
    }                                                                                      \
    static inline uint##N##_t adcs##N##_nz(uint64_t *nz, uint##N##_t a, uint##N##_t b) {   \
        uint##N##_t cin = (*nz >> 29) & 1, r = a + b + cin;                                \
        int carry = cin ? r <= a : r < a;                                                  \
        *nz = MKFLAGS(r >> (N - 1), r == 0, carry, ((a ^ r) & (b ^ r)) >> (N - 1));        \
        return r;                                                                          \
    }                                                                                      \
    static inline uint##N##_t sbcs##N##_nz(uint64_t *nz, uint##N##_t a, uint##N##_t b) {   \
        return adcs##N##_nz(nz, a, (uint##N##_t)~b);                                       \
    }
ADC_IMPL(64, )
ADC_IMPL(32, )
#define adc64(c, a, b) adc64_nz((c)->nzcv, a, b)
#define sbc64(c, a, b) sbc64_nz((c)->nzcv, a, b)
#define adcs64(c, a, b) adcs64_nz(&(c)->nzcv, a, b)
#define sbcs64(c, a, b) sbcs64_nz(&(c)->nzcv, a, b)
#define adc32(c, a, b) adc32_nz((c)->nzcv, a, b)
#define sbc32(c, a, b) sbc32_nz((c)->nzcv, a, b)
#define adcs32(c, a, b) adcs32_nz(&(c)->nzcv, a, b)
#define sbcs32(c, a, b) sbcs32_nz(&(c)->nzcv, a, b)

// ---- bit ops
#define ROR64(v, r) ((uint64_t)(v) >> ((r) & 63) | (uint64_t)(v) << ((64 - ((r) & 63)) & 63))
#define ROR32(v, r) ((uint32_t)(v) >> ((r) & 31) | (uint32_t)(v) << ((32 - ((r) & 31)) & 31))
static inline uint64_t clz64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
static inline uint32_t clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
static inline uint64_t cls64(uint64_t v) { return clz64(v ^ (uint64_t)((int64_t)v >> 1)) - 1; }
static inline uint32_t cls32(uint32_t v) { return clz32(v ^ (uint32_t)((int32_t)v >> 1)) - 1; }
static inline uint64_t rbit64(uint64_t v) { return __builtin_bitreverse64(v); }
static inline uint32_t rbit32(uint32_t v) { return __builtin_bitreverse32(v); }
static inline uint32_t rev16_32(uint32_t v) { return ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu); }
static inline uint64_t rev16_64(uint64_t v) {
    return ((v & 0x00ff00ff00ff00ffull) << 8) | ((v >> 8) & 0x00ff00ff00ff00ffull);
}
static inline uint64_t rev32_64(uint64_t v) {
    return ((uint64_t)__builtin_bswap32((uint32_t)v)) | ((uint64_t)__builtin_bswap32((uint32_t)(v >> 32)) << 32);
}
static inline uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint64_t sdiv64(uint64_t a, uint64_t b) {
    if (!b) return 0;
    if ((int64_t)a == INT64_MIN && (int64_t)b == -1) return a;
    return (uint64_t)((int64_t)a / (int64_t)b);
}
static inline uint32_t sdiv32(uint32_t a, uint32_t b) {
    if (!b) return 0;
    if ((int32_t)a == INT32_MIN && (int32_t)b == -1) return a;
    return (uint32_t)((int32_t)a / (int32_t)b);
}

// Portable FP/SIMD helpers for code lifted with --simd=c (default off AArch64; gen.py includes simd.h itself
// in that mode, so AArch64 hosts can build it too).
#if !defined(__cplusplus) && !defined(__aarch64__)
#include "simd.h"
#endif
