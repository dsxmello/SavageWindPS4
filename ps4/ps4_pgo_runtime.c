// Savage Wind PS4 - PGO instrumented builds only (ps4/build.sh PGO=generate), as TriAevum PS4's ps4_pgo_runtime.c.
//
// The host clang's profile runtime (libclang_rt.profile-x86_64.a) is built against glibc; its few glibc-only entry
// points map onto the SDK libc here. The runtime writes on exit, which a PS4 app never reaches: a thread rewrites
// /data/savagewind/pgo/savagewind.profraw every 30 seconds (the counters are cumulative, so the newest file holds the
// whole session; the previous one stays as .prev until the new one is complete).

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PGO_DIR "/data/savagewind/pgo"
#define PGO_FILE PGO_DIR "/savagewind.profraw"

int __llvm_profile_write_file(void);
void __llvm_profile_set_filename(const char *name);

// The runtime records build IDs from the program headers it finds through the linker's __ehdr_start. On the PS4 the
// fself doesn't map the ELF header there (TriAevum 1.19: SIGBUS in __llvm_write_binary_ids on the first write); this
// definition takes precedence over the linker's and declares no program headers, so no IDs are written.
struct SwElfHeader {
    unsigned char ident[16];
    unsigned short type, machine;
    unsigned int version;
    unsigned long long entry, phoff, shoff;
    unsigned int flags;
    unsigned short ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
__attribute__((visibility("hidden"))) const struct SwElfHeader __ehdr_start = {
    {0x7F, 'E', 'L', 'F', 2, 1, 1}, 2, 62, 1, 0, 0, 0, 0, 64, 56, 0, 64, 0, 0};

int __fprintf_chk(FILE *stream, int flag, const char *format, ...) {
    (void)flag;
    va_list ap;
    va_start(ap, format);
    int n = vfprintf(stream, format, ap);
    va_end(ap);
    return n;
}

int __snprintf_chk(char *buf, size_t size, int flag, size_t bufsize, const char *format, ...) {
    (void)flag, (void)bufsize;
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(buf, size, format, ap);
    va_end(ap);
    return n;
}

// (clang 23's runtime also formats through the va_list forms)
int __vfprintf_chk(FILE *stream, int flag, const char *format, va_list ap) {
    (void)flag;
    return vfprintf(stream, format, ap);
}

int __vsnprintf_chk(char *buf, size_t size, int flag, size_t bufsize, const char *format, va_list ap) {
    (void)flag, (void)bufsize;
    return vsnprintf(buf, size, format, ap);
}

void *__memcpy_chk(void *dst, const void *src, size_t size, size_t dstsize) {
    (void)dstsize;
    return memcpy(dst, src, size);
}

void *__memset_chk(void *dst, int value, size_t size, size_t dstsize) {
    (void)dstsize;
    return memset(dst, value, size);
}

char *__strncpy_chk(char *dst, const char *src, size_t size, size_t dstsize) {
    (void)dstsize;
    return strncpy(dst, src, size);
}

long __isoc23_strtol(const char *text, char **end, int base) { return strtol(text, end, base); }

int prctl(int option, ...) {
    (void)option;
    errno = EINVAL;
    return -1;
}

struct utsname;
int uname(struct utsname *name) {
    (void)name;
    errno = ENOSYS;
    return -1;
}

static void *write_periodically(void *arg) {
    (void)arg;
    const struct timespec period = {30, 0};
    for (unsigned n = 1;; n++) {
        nanosleep(&period, NULL);
        rename(PGO_FILE, PGO_FILE ".prev");  // (a crash while writing leaves the last complete one)
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int r = __llvm_profile_write_file();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        fprintf(stderr, "[pgo] profile %u written (%d, %.1f s)\n", n, r,
                (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    }
    return NULL;
}

void sw_pgo_start(void) {
    mkdir(PGO_DIR, 0777);
    __llvm_profile_set_filename(PGO_FILE);
    pthread_t t;
    if (pthread_create(&t, NULL, write_periodically, NULL) == 0) {
        pthread_detach(t);
        fprintf(stderr, "[pgo] instrumented build: %s every 30 s\n", PGO_FILE);
    }
}
