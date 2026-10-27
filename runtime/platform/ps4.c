// PS4 host layer (runtime/platform.h), for the OpenOrbis toolchain (ps4/CMakeLists.txt). x86-64 (AMD Jaguar).
//
// Guest memory: the PS4 has no overcommit and no lazily backed anonymous memory at a fixed address. The guest window
// is a sceKernelReserveVirtualRange reservation, and 2 MiB chunks of it get direct (physical) memory on their first
// access (the SIGSEGV / SIGBUS handler below) or when the runtime is about to let the kernel write there
// (plat_commit: file reads into guest buffers). The chunks are CPU and GPU readable / writable: the GPU address of a
// mapping is its CPU address, so the Vulkan backend can use guest memory directly, as on the PC.
// The game declares 3.2 GB of application memory (runtime/kernel.c) but only what it touches costs memory here.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../platform.h"

// Declared by hand, as in DolphinPS4 (Common/MemArenaPS4.cpp): several OpenOrbis prototypes are wrong.
size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(off_t search_start, off_t search_end, size_t length, size_t alignment,
                                      int32_t memory_type, off_t *physical_out);
int32_t sceKernelReleaseDirectMemory(off_t start, size_t length);
int32_t sceKernelMapDirectMemory(void **address, size_t length, int32_t protection, int32_t flags, off_t start,
                                 size_t alignment);
int32_t sceKernelReserveVirtualRange(void **address, size_t length, int32_t flags, size_t alignment);
int32_t sceKernelMunmap(void *address, size_t length);
int32_t sceKernelMprotect(const void *address, size_t length, int32_t protection);
uint32_t sceSysmoduleLoadModuleInternal(uint32_t id);
int32_t sceSystemServiceLoadExec(const char *path, const char *args[]);
int32_t sceSystemServiceHideSplashScreen(void);
void *scePthreadSelf(void);
int scePthreadSetprio(void *thread, int prio);
uint64_t sceKernelGetTscFrequency(void);
int32_t sceKernelAvailableDirectMemorySize(off_t search_start, off_t search_end, size_t alignment, off_t *physical_out,
                                           size_t *size_out);
void ac_ps4_get_memory(uint64_t *garlic_bytes, uint64_t *onion_bytes, uint64_t *buffers);  // (RADV, ac_ps4_drm.c)
void ps4_heap_usage(size_t *main_mb, size_t *overflow_mb, size_t *overflow_cap_mb);  // (ps4/ps4_runtime.cpp)
uint32_t nvn_texture_count(uint32_t *inits);                                        // (nvn/objects.c)

#define SCE_KERNEL_WB_ONION 0      // CPU cached, GPU coherent: the guest's memory is mostly CPU work
#define SCE_KERNEL_MAP_FIXED 0x10
#define SCE_KERNEL_PROT_CPU_READ 0x01
#define SCE_KERNEL_PROT_CPU_RW 0x03
#define SCE_KERNEL_PROT_GPU_RW 0x30

// ---- guest address space
#define CHUNK 0x200000ull  // 2 MiB: direct memory's natural size, and few enough faults
static uint64_t g_lo, g_hi;
static _Atomic(uint8_t) *g_state;  // per chunk: 0 reserved only, 1 being backed, 2 backed
static off_t *g_phys;              // per chunk: its direct memory, for plat_reset_guest
static _Atomic uint64_t g_backed;  // bytes, for the log
static _Atomic uint64_t g_stack_bytes;  // (host stacks: plat_alloc_stack)

static bool reserve(uint64_t addr, uint64_t size) {
    void *p = (void *)(uintptr_t)addr;
    int32_t r = sceKernelReserveVirtualRange(&p, size, SCE_KERNEL_MAP_FIXED, CHUNK);
    return r == 0 && p == (void *)(uintptr_t)addr;
}

// Back chunk i (idempotent, any thread, also from the fault handler: no locks, no malloc). False when the console
// is out of direct memory.
static bool back_chunk(uint64_t i) {
    uint8_t s = atomic_load(&g_state[i]);
    while (s != 2) {
        uint8_t expect = 0;
        if (s == 0 && atomic_compare_exchange_strong(&g_state[i], &expect, 1)) {
            off_t phys = 0;
            if (sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), CHUNK, CHUNK,
                                              SCE_KERNEL_WB_ONION, &phys) != 0) {
                atomic_store(&g_state[i], 0);
                return false;
            }
            void *p = (void *)(uintptr_t)(g_lo + i * CHUNK);
            if (sceKernelMapDirectMemory(&p, CHUNK, SCE_KERNEL_PROT_CPU_RW | SCE_KERNEL_PROT_GPU_RW,
                                         SCE_KERNEL_MAP_FIXED, phys, CHUNK) != 0) {
                sceKernelReleaseDirectMemory(phys, CHUNK);
                atomic_store(&g_state[i], 0);
                return false;
            }
            memset(p, 0, CHUNK);  // (direct memory comes as it was left)
            g_phys[i] = phys;
            atomic_fetch_add(&g_backed, CHUNK);
            atomic_store(&g_state[i], 2);
            return true;
        }
        s = atomic_load(&g_state[i]);  // another thread is backing it: wait for that
    }
    return true;
}

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    lo &= ~(CHUNK - 1);
    hi = (hi + CHUNK - 1) & ~(CHUNK - 1);
    uint64_t n = (hi - lo) / CHUNK;
    g_state = calloc(n, 1);
    g_phys = calloc(n, sizeof *g_phys);
    if (!g_state || !g_phys || !reserve(lo, hi - lo)) return false;
    g_lo = lo;
    g_hi = hi;
    return true;
}

// Every backed chunk goes back to the system and the window is reserved again (unmapping leaves a hole).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    (void)lo, (void)hi;
    bool ok = true;
    for (uint64_t i = 0; i < (g_hi - g_lo) / CHUNK; i++) {
        if (atomic_load(&g_state[i]) != 2) continue;
        void *p = (void *)(uintptr_t)(g_lo + i * CHUNK);
        sceKernelMunmap(p, CHUNK);
        sceKernelReleaseDirectMemory(g_phys[i], CHUNK);
        if (!reserve((uint64_t)(uintptr_t)p, CHUNK)) ok = false;
        atomic_store(&g_state[i], 0);
        atomic_fetch_sub(&g_backed, CHUNK);
    }
    return ok;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size || addr >= g_hi || addr + size <= g_lo) return;
    uint64_t a = addr < g_lo ? g_lo : addr, e = addr + size > g_hi ? g_hi : addr + size;
    for (uint64_t i = (a - g_lo) / CHUNK; i <= (e - 1 - g_lo) / CHUNK; i++)
        if (!back_chunk(i)) {
            fprintf(stderr, "[plat] out of direct memory backing %llx+%llx (%llu MB of guest memory backed)\n",
                    (unsigned long long)addr, (unsigned long long)size,
                    (unsigned long long)(atomic_load(&g_backed) >> 20));
            return;
        }
}

uint64_t plat_guest_backed_bytes(void) { return atomic_load(&g_backed); }

// Host stacks of guest threads (runtime/kernel.c thread_start) and of the host's own threads (pthread_create, below):
// direct memory mapped anywhere, CPU only, all of it backed (no alternate signal stack here: a fault on a thread's own
// stack couldn't be handled), with an unmapped 2 MiB guard below each (an overflow faults instead of writing over
// what lies under it). Never freed: the threads live as long as the process.
// Each is filled with a pattern, so the memory report can tell how deep the deepest one got (stack_used_max).
#define STACK_FILL 0x5357535753575357ull
#define MAX_STACKS 256
static struct { uint64_t lo, size; bool guest; } g_stacks[MAX_STACKS];
static _Atomic int g_nstacks;
static void *alloc_stack(size_t size, const char *what) {
    size = (size + CHUNK - 1) & ~(CHUNK - 1);
    void *base = NULL;
    if (sceKernelReserveVirtualRange(&base, size + CHUNK, 0, CHUNK) != 0) return NULL;
    off_t phys = 0;
    if (sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), size, CHUNK, SCE_KERNEL_WB_ONION,
                                      &phys) != 0) {
        sceKernelMunmap(base, size + CHUNK);
        return NULL;
    }
    void *p = (char *)base + CHUNK;  // (the guard: [base, p) stays reserved, unmapped)
    if (sceKernelMapDirectMemory(&p, size, SCE_KERNEL_PROT_CPU_RW, SCE_KERNEL_MAP_FIXED, phys, CHUNK) != 0) {
        sceKernelReleaseDirectMemory(phys, size);
        sceKernelMunmap(base, size + CHUNK);
        return NULL;
    }
    for (uint64_t *w = p, *e = (uint64_t *)((char *)p + size); w < e; w++) *w = STACK_FILL;
    int k = atomic_fetch_add(&g_nstacks, 1);
    if (k < MAX_STACKS) g_stacks[k].lo = (uint64_t)(uintptr_t)p, g_stacks[k].size = size, g_stacks[k].guest = what[0] == 'h';
    uint64_t total = atomic_fetch_add(&g_stack_bytes, size) + size;
    fprintf(stderr, "[plat] %s stack %llu MB at %p (%llu MB of stacks)\n", what, (unsigned long long)(size >> 20), p,
            (unsigned long long)(total >> 20));
    return p;
}
void *plat_alloc_stack(size_t size) { return alloc_stack(size, "host"); }

// The deepest a guest thread's stack (guest) or a host thread's got: the lowest 4 KiB page under its top whose first
// word lost the fill
static uint64_t stack_used_max(bool guest) {
    uint64_t best = 0;
    int n = atomic_load(&g_nstacks);
    for (int k = 0; k < n && k < MAX_STACKS; k++) {
        uint64_t lo = g_stacks[k].lo, size = g_stacks[k].size, off = 0;
        if (!lo || g_stacks[k].guest != guest) continue;
        while (off < size && *(volatile uint64_t *)(uintptr_t)(lo + off) == STACK_FILL) off += 4096;
        if (size - off > best) best = size - off;
    }
    return best;
}

// The host's own threads (the queue, the translator, shader and pipeline workers, RADV's and SDL's): a plain pthread
// stack comes from the process's small flexible memory, which the eboot itself fills (the PGO-instrumented one: a
// pipeline pre-warm worker never started, scePthreadCreate 0x80020023, and the pre-warm waited for it forever). Those
// created without a stack of their own get one in direct memory, 2 MiB or the size they asked for (SDL's), and keep
// their detach state (ps4/CMakeLists.txt: --wrap=pthread_create).
#define HOST_THREAD_STACK (2ull << 20)
int __real_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    size_t size = HOST_THREAD_STACK, asked = 0;
    int detach = PTHREAD_CREATE_JOINABLE;
    if (attr) {
        void *own = NULL;
        if (pthread_attr_getstack(attr, &own, &asked) == 0 && own)
            return __real_pthread_create(t, attr, fn, arg);  // (kernel.c's guest threads bring their own)
        if (pthread_attr_getstacksize(attr, &asked) == 0 && asked > size) size = asked;
        pthread_attr_getdetachstate(attr, &detach);
    }
    void *stack = alloc_stack(size, "thread");
    if (!stack) return __real_pthread_create(t, attr, fn, arg);
    size = (size + CHUNK - 1) & ~(CHUNK - 1);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, detach);
    pthread_attr_setstack(&a, stack, size);
    int r = __real_pthread_create(t, &a, fn, arg);
    pthread_attr_destroy(&a);
    return r;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? 0 : prot == PLAT_PROT_READ ? SCE_KERNEL_PROT_CPU_READ : SCE_KERNEL_PROT_CPU_RW;
    if (p) p |= SCE_KERNEL_PROT_GPU_RW;
    plat_commit(addr, size);
    return sceKernelMprotect((void *)(uintptr_t)addr, size, p) == 0 ? 0 : -1;
}

// ---- time
// (clock_gettime is served from the TSC: DolphinPS4's runtime, --wrap=clock_gettime in its CMake setup)
// The clock: the CPU's time stamp counter (rdtsc: invariant, the same on every core), from CLOCK_MONOTONIC's reading at
// the first call. clock_gettime is a system call here, microseconds each, and the game reads the clock all the time
// (the system tick, rt_ticks); SAVAGEWIND_VK_PROF's timings alone halved the frame rate.
// SAVAGEWIND_CLOCK_SYSCALL=1: clock_gettime every time (as before).
static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}
uint64_t plat_now_ns(void) {
    static uint64_t tsc0, ns0, mul;  // ns = ns0 + (tsc - tsc0) * mul >> 32
    static int mode = -1;            // 0 clock_gettime, 1 tsc
    if (__builtin_expect(mode < 0, 0)) {
        uint64_t f = sceKernelGetTscFrequency();
        ns0 = mono_ns();
        tsc0 = __builtin_ia32_rdtsc();
        mul = f ? (uint64_t)(((__uint128_t)1000000000u << 32) / f) : 0;
        int m = mul && !getenv("SAVAGEWIND_CLOCK_SYSCALL");
        if (m) {
            uint64_t a = mono_ns();
            for (int i = 0; i < 999; i++) (void)mono_ns();
            uint64_t b = mono_ns(), c0 = __builtin_ia32_rdtsc();
            for (int i = 0; i < 1000; i++) (void)__builtin_ia32_rdtsc();
            uint64_t c1 = __builtin_ia32_rdtsc();
            fprintf(stderr, "[plat] clock: TSC at %llu Hz (rdtsc %.0f ns, clock_gettime %.0f ns)\n", (unsigned long long)f,
                    (double)(((__uint128_t)(c1 - c0) * mul) >> 32) / 1000.0, (b - a) / 1001.0);
        }
        __atomic_store_n(&mode, m, __ATOMIC_RELEASE);
    }
    if (!mode) return mono_ns();
    return ns0 + (uint64_t)(((__uint128_t)(__builtin_ia32_rdtsc() - tsc0) * mul) >> 32);
}

void plat_sleep_until_ns(uint64_t t) {
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 200000) { __builtin_ia32_pause(); continue; }  // last 0.2 ms: spin (timer slack)
        struct timespec ts = {.tv_sec = 0, .tv_nsec = (long)(left - 100000 < 999999999 ? left - 100000 : 999999999)};
        nanosleep(&ts, NULL);
    }
}

// PS4 thread priorities: 256 (highest) - 767 (lowest), 700 by default.
void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    scePthreadSetprio(scePthreadSelf(), 300);
}

// ---- process
// stdout / stderr go to /data/savagewind/savagewind.log (the previous run's stays as savagewind.prev.log).
// freopen(stderr) leaves the file empty on the console: point the descriptors at it instead (dusklight-ps4).
static void log_to_file(void) {
    mkdir(PS4_DATA_DIR, 0777);
    rename(PS4_DATA_DIR "/savagewind.log", PS4_DATA_DIR "/savagewind.prev.log");
    int fd = open(PS4_DATA_DIR "/savagewind.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    dup2(fd, 2);
    dup2(fd, 1);
    if (fd > 2) close(fd);
    setvbuf(stderr, NULL, _IONBF, 0);
}

// Calling into a system library whose module isn't loaded kills the title: load them first (DolphinPS4, dusklight).
static void load_system_modules(void) {
    static const struct { const char *name; uint32_t id; } mods[] = {
        {"SystemService", 0x80000010}, {"UserService", 0x80000011}, {"VideoOut", 0x80000022},
        {"AudioOut", 0x80000001},      {"Pad", 0x80000024},
        {"Net", 0x8000001c},  // (the C library's resolver imports sceNetGetDnsInfo)
    };
    for (size_t i = 0; i < sizeof mods / sizeof mods[0]; i++) {
        uint32_t r = sceSysmoduleLoadModuleInternal(mods[i].id);
        if (r) fprintf(stderr, "[plat] system module %s: %#x\n", mods[i].name, r);
    }
}

// Hang watchdog: the log stays the same size for 10 s -> every guest thread's state to it (kernel_dump_threads), at
// most 5 times, 30 s apart. The console has no debugger to attach; this shows what each thread waits in.
void kernel_dump_threads(const char *why);
void kernel_log_core_waits(void);
// Where the console's direct memory went: the guest's, the host stacks', RADV's buffers and images, and what's left
// (the largest free 2 MiB-aligned block: a guest chunk needs one)
static void log_memory(const char *why) {
    uint64_t garlic = 0, onion = 0, bufs = 0;
    ac_ps4_get_memory(&garlic, &onion, &bufs);
    off_t at = 0;
    size_t big = 0;
    if (sceKernelAvailableDirectMemorySize(0, (off_t)sceKernelGetDirectMemorySize(), CHUNK, &at, &big) != 0) big = 0;
    // (the host heap: 512 MB of system flexible memory, then an overflow heap in direct memory; full = abort)
    size_t heap = 0, over = 0, over_cap = 0;
    ps4_heap_usage(&heap, &over, &over_cap);
    uint32_t tex_inits = 0, tex = nvn_texture_count(&tex_inits);
    fprintf(stderr, "[plat] memory%s: host heap %zu MB + overflow %zu of %zu MB (%u textures of %u inits), guest %llu MB, stacks %llu MB (%d; deepest used: guest threads %llu KB, host "
            "threads %llu KB), GPU %llu MB garlic + %llu MB onion (%llu buffers), largest free block %llu MB of %llu MB\n",
            why, heap, over, over_cap, tex, tex_inits, (unsigned long long)(atomic_load(&g_backed) >> 20), (unsigned long long)(atomic_load(&g_stack_bytes) >> 20),
            atomic_load(&g_nstacks), (unsigned long long)(stack_used_max(true) >> 10),
            (unsigned long long)(stack_used_max(false) >> 10), (unsigned long long)(garlic >> 20),
            (unsigned long long)(onion >> 20), (unsigned long long)bufs, (unsigned long long)(big >> 20),
            (unsigned long long)(sceKernelGetDirectMemorySize() >> 20));
}

static void *hang_watch(void *arg) {
    (void)arg;
    off_t last = -1;
    int still = 0, dumps = 0, wait = 10;
    for (;;) {
        sleep(1);
        struct stat st;
        if (fstat(2, &st) != 0) continue;
        if (st.st_size != last) {
            static unsigned secs;
            if (++secs % 30 == 0) { log_memory(""); kernel_log_core_waits(); fstat(2, &st); }  // (only while the log moves: hangs stay quiet)
            last = st.st_size; still = 0; wait = 10;
            continue;
        }
        if (++still < wait || dumps >= 5) continue;
        char why[64];
        snprintf(why, sizeof why, "no log for %d s (dump %d)", still, ++dumps);
        kernel_dump_threads(why);
        fstat(2, &st);
        last = st.st_size;
        still = 0;
        wait = 30;
    }
    return NULL;
}

// (profiling) SAVAGEWIND_PS4_SAMPLE=1: DolphinPS4's sampler (ps4/ps4_sampler.cpp) on the queue thread (the NVN ->
// Vulkan translation) and the first 31 guest threads: every 10 s, /data/savagewind/samples.log gets each one's
// busiest eboot addresses (symbolize with llvm-symbolizer --obj=<the build's savagewind.oelf>).
void ps4_sampler_start(void);
void ps4_sampler_register_thread(const char *name);
static int sampling(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_PS4_SAMPLE") && atoi(getenv("SAVAGEWIND_PS4_SAMPLE")) > 0;
    return on;
}
void ps4_profile_thread(const char *name) {
    if (!sampling()) return;
    ps4_sampler_start();  // (once)
    ps4_sampler_register_thread(strdup(name));  // (kept: the sampler holds the pointer)
}

// /data/savagewind/env.txt: KEY=VALUE lines (# comments) set as environment variables before anything reads them -
// the PC's SAVAGEWIND_* test and debug switches, which the console has no other way to get. Dumps (screenshots,
// SAVAGEWIND_DEBUG_FRAME's passes) go to build/ and build/frames/ under the data folder, as on the PC.
static void env_from_file(void) {
    FILE *f = fopen(PS4_DATA_DIR "/env.txt", "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        char *k = line;
        while (*k == ' ') k++;
        char *eq = strchr(k, '=');
        if (!*k || *k == '#' || !eq) continue;
        *eq = 0;
        setenv(k, eq + 1, 1);
        fprintf(stderr, "[plat] env.txt: %s=%s\n", k, eq + 1);
    }
    fclose(f);
    mkdir(PS4_DATA_DIR "/build", 0777);
    mkdir(PS4_DATA_DIR "/build/frames", 0777);
}

void plat_init(void) {
    log_to_file();
    env_from_file();
    (void)plat_now_ns();  // (the clock's setup, before any other thread reads it)
#ifdef SW_PS4_PGO_GENERATE
    { void sw_pgo_start(void); sw_pgo_start(); }  // (ps4/ps4_pgo_runtime.c: an instrumented build's profile writer)
#endif
    load_system_modules();
    fprintf(stderr, "[plat] PS4: %llu MB of direct memory\n", (unsigned long long)(sceKernelGetDirectMemorySize() >> 20));
    pthread_t t;
    if (pthread_create(&t, NULL, hang_watch, NULL) == 0) pthread_detach(t);
}

// The console's boot splash stays up until the first frame (runtime/nvn/vulkan: first present).
void plat_first_frame(void) {
    static bool done;
    if (!done) { done = true; sceSystemServiceHideSplashScreen(); }
}

bool plat_exe_path(char *buf, size_t size) {
    snprintf(buf, size, PS4_APP_DIR "/eboot.bin");
    return true;
}

// Restart the title (the start screen's "play", an in-game restart). Command lines are not passed on.
void plat_exec(char *const argv[]) {
    (void)argv;
    sceSystemServiceLoadExec(PS4_APP_DIR "/eboot.bin", NULL);
}

// Returning from main (or exit) kills the title with an error on the console: leave through the system instead.
void plat_terminate(void) {
    fflush(stderr);
    sceSystemServiceLoadExec("exit", NULL);
    _exit(0);
}

// ---- child processes: none on the console (the game code is generated and built on the PC)
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    (void)argv, (void)log_path, (void)append_log;
    return 0;
}
int plat_proc_wait(PlatProc p, bool block) { (void)p, (void)block; return 255; }
void plat_proc_kill(PlatProc p) { (void)p; }

bool plat_remove_tree(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return remove(dir) == 0 || errno == ENOENT;
    struct dirent *e;
    bool ok = true;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) ok &= plat_remove_tree(p);
        else if (remove(p) != 0) ok = false;
    }
    closedir(d);
    return rmdir(dir) == 0 && ok;
}

// ---- diagnostics
// No dladdr: frames are printed as eboot offsets (the eboot is loaded at 0x400000), for
// llvm-addr2line -fe <the eboot's ELF> 0x<offset> on the PC. Generated-code frames are named by the runtime.
const char *plat_symbol(uint64_t pc, uint64_t *offset) { (void)pc, (void)offset; return NULL; }

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

static void print_frame(int i, uint64_t pc) {
    uint64_t off = 0;
    const char *s = rt_host_fn_name(pc, &off);
    if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx (eboot+%llx)\n", i, (unsigned long long)pc, s,
                   (unsigned long long)off, (unsigned long long)(pc - 0x400000));
    else fprintf(stderr, "  #%-2d %016llx (eboot+%llx)\n", i, (unsigned long long)pc, (unsigned long long)(pc - 0x400000));
}

// Frame-pointer walk (the runtime and the game code are built with frame pointers: ps4/CMakeLists.txt).
static void walk_frames(uint64_t fp, int first) {
    for (int i = first; i < 64 && fp && !(fp & 7); i++) {
        uint64_t *f = (uint64_t *)(uintptr_t)fp;
        uint64_t next = f[0], ret = f[1];
        if (!ret) break;
        print_frame(i, ret);
        if (next <= fp) break;  // (stacks grow down: callers' frames are higher)
        fp = next;
    }
}

void plat_print_backtrace(void) { walk_frames((uint64_t)(uintptr_t)__builtin_frame_address(0), 0); }

// A fatal error: the details are in the log; leave through the system (an abort shows the console's crash report and
// keeps the title from closing cleanly).
void plat_fatal_notice(void) { plat_terminate(); }

// The registers the kernel passes to a signal handler: FreeBSD's amd64 mcontext, but 0x40 bytes into the context
// (measured on hardware by DolphinPS4: Core/MemTools.cpp).
typedef struct {
    uint64_t onstack, rdi, rsi, rdx, rcx, r8, r9, rax, rbx, rbp, r10, r11, r12, r13, r14, r15;
    uint32_t trapno;
    uint16_t fs, gs;
    uint64_t addr;
    uint32_t flags;
    uint16_t es, ds;
    uint64_t err, rip, cs, rflags, rsp, ss;
} Ps4MContext;
#define PS4_MCONTEXT(ctx) ((Ps4MContext *)((char *)(ctx) + 0x40))

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    Ps4MContext *mc = PS4_MCONTEXT(ctx);
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
    if ((sig == SIGSEGV || sig == SIGBUS) && fa >= g_lo && fa < g_hi && atomic_load(&g_state[(fa - g_lo) / CHUNK]) != 2 &&
        back_chunk((fa - g_lo) / CHUNK))
        return;  // first touch of a guest chunk: backed, retry the access
    // x86-64 has no link register: report the return address on top of the stack (valid at function entry)
    uint64_t pc = mc->rip, sp = mc->rsp, lr = 0;
    if (sig != SIGSEGV || (fa & ~0xFFFull) != (sp & ~0xFFFull)) lr = *(uint64_t *)(uintptr_t)sp;
    g_fault_is_write = (mc->err & 2) != 0;  // (page fault error code: bit 1 = write)
    if (rt_fault_watch(fa, pc)) return;
    static volatile int depth;
    if (depth++ > 0) _exit(1);  // (nested: just die)
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    if ((sig == SIGSEGV || sig == SIGBUS) && fa >= g_lo && fa < g_hi && atomic_load(&g_state[(fa - g_lo) / CHUNK]) != 2)
        log_memory(" (a guest chunk couldn't be backed: out of direct memory)");
    fprintf(stderr, "[plat] fault at %llx, pc %llx (eboot+%llx), %llu MB of guest memory backed\n",
            (unsigned long long)fa, (unsigned long long)pc, (unsigned long long)(pc - 0x400000),
            (unsigned long long)(atomic_load(&g_backed) >> 20));
    walk_frames(mc->rbp, 1);
    rt_fault_report(what, fa, pc, lr);
}

// (no alternate signal stack: not verified on the console; host stacks are 16 MB of direct memory, a fault on one's guard page ends
// the title without a report)
void plat_crash_handler_install(void) {
    static bool installed;  // (process-wide on the PS4, unlike the per-thread signal stack elsewhere)
    if (installed) return;
    installed = true;
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}
