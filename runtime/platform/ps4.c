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

// Host stacks of guest threads (runtime/kernel.c thread_start): direct memory mapped anywhere, CPU only, all of it
// backed (no alternate signal stack here: a fault on a thread's own stack couldn't be handled). Never freed: the
// game's threads live as long as the process.
static _Atomic uint64_t g_stack_bytes;
void *plat_alloc_stack(size_t size) {
    size = (size + CHUNK - 1) & ~(CHUNK - 1);
    off_t phys = 0;
    if (sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), size, CHUNK, SCE_KERNEL_WB_ONION,
                                      &phys) != 0)
        return NULL;
    void *p = NULL;
    if (sceKernelMapDirectMemory(&p, size, SCE_KERNEL_PROT_CPU_RW, 0, phys, CHUNK) != 0) {
        sceKernelReleaseDirectMemory(phys, size);
        return NULL;
    }
    uint64_t total = atomic_fetch_add(&g_stack_bytes, size) + size;
    fprintf(stderr, "[plat] host stack %llu MB at %p (%llu MB of stacks)\n", (unsigned long long)(size >> 20), p,
            (unsigned long long)(total >> 20));
    return p;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? 0 : prot == PLAT_PROT_READ ? SCE_KERNEL_PROT_CPU_READ : SCE_KERNEL_PROT_CPU_RW;
    if (p) p |= SCE_KERNEL_PROT_GPU_RW;
    plat_commit(addr, size);
    return sceKernelMprotect((void *)(uintptr_t)addr, size, p) == 0 ? 0 : -1;
}

// ---- time
// (clock_gettime is served from the TSC: DolphinPS4's runtime, --wrap=clock_gettime in its CMake setup)
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
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
static void *hang_watch(void *arg) {
    (void)arg;
    off_t last = -1;
    int still = 0, dumps = 0, wait = 10;
    for (;;) {
        sleep(1);
        struct stat st;
        if (fstat(2, &st) != 0) continue;
        if (st.st_size != last) { last = st.st_size; still = 0; wait = 10; continue; }
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

void plat_init(void) {
    log_to_file();
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
