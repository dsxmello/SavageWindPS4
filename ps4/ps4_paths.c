// Relative paths on the PS4. The runtime works in its data root and opens most files by relative names
// (runtime/paths.h), but the PS4 kernel answers EINVAL to any path not starting with '/', even after a chdir, and the
// libc's getcwd is a stub that always fails (TriAevum PS4 found both on the console). So every path-taking call the
// runtime makes is wrapped at link time (--wrap, ps4/CMakeLists.txt) and resolves relative paths against the working
// directory kept here; fopen and opendir reach the kernel through open. realpath (musl's needs /proc) is replaced too.
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../runtime/platform/ps4_compat.h"

static char g_cwd[PATH_MAX] = PS4_DATA_DIR;

// path as the kernel wants it: absolute ones as they are, relative ones under the working directory, "." and ".."
// components folded. NULL (errno set) when it doesn't fit.
static const char *absolute(const char *path, char *buf) {
    if (!path || !*path) return path;
    char tmp[PATH_MAX];
    if (path[0] == '/') {
        if ((size_t)snprintf(tmp, sizeof tmp, "%s", path) >= sizeof tmp) goto toolong;
    } else if ((size_t)snprintf(tmp, sizeof tmp, "%s/%s", g_cwd, path) >= sizeof tmp)
        goto toolong;
    // fold the components into buf
    size_t n = 0;
    for (char *s = tmp, *e; *s; s = e) {
        while (*s == '/') s++;
        if (!*s) break;
        for (e = s; *e && *e != '/'; e++) {}
        size_t len = (size_t)(e - s);
        if (len == 1 && s[0] == '.') continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (n > 0 && buf[n - 1] != '/') n--;
            if (n > 0) n--;
            continue;
        }
        if (n + 1 + len + 1 > PATH_MAX) goto toolong;
        buf[n++] = '/';
        memcpy(buf + n, s, len);
        n += len;
    }
    if (!n) buf[n++] = '/';
    buf[n] = 0;
    return buf;
toolong:
    errno = ENAMETOOLONG;
    return NULL;
}

#define RESOLVE(var, path)                    \
    char var##_buf[PATH_MAX];                 \
    const char *var = absolute(path, var##_buf); \
    if (path && !var) return -1

int __real_open(const char *, int, ...);
int __real_stat(const char *, struct stat *);
int __real_lstat(const char *, struct stat *);
int __real_mkdir(const char *, mode_t);
int __real_rmdir(const char *);
int __real_unlink(const char *);
int __real_rename(const char *, const char *);
int __real_access(const char *, int);
int __real_truncate(const char *, off_t);

int __wrap_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    RESOLVE(p, path);
    return __real_open(p, flags, mode);
}
int __wrap_stat(const char *path, struct stat *st) { RESOLVE(p, path); return __real_stat(p, st); }
int __wrap_lstat(const char *path, struct stat *st) { RESOLVE(p, path); return __real_lstat(p, st); }
int __wrap_mkdir(const char *path, mode_t mode) { RESOLVE(p, path); return __real_mkdir(p, mode); }
int __wrap_rmdir(const char *path) { RESOLVE(p, path); return __real_rmdir(p); }
int __wrap_unlink(const char *path) { RESOLVE(p, path); return __real_unlink(p); }
int __wrap_access(const char *path, int mode) { RESOLVE(p, path); return __real_access(p, mode); }
int __wrap_truncate(const char *path, off_t len) { RESOLVE(p, path); return __real_truncate(p, len); }
int __wrap_rename(const char *from, const char *to) {
    RESOLVE(a, from);
    RESOLVE(b, to);
    return __real_rename(a, b);
}

// The working directory: only remembered (the kernel's own is no use), for the calls above.
int __wrap_chdir(const char *path) {
    char buf[PATH_MAX];
    const char *p = absolute(path, buf);
    if (!p) return -1;
    struct stat st;
    if (__real_stat(p, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    snprintf(g_cwd, sizeof g_cwd, "%s", p);
    return 0;
}

char *__wrap_getcwd(char *buf, size_t size) {
    size_t len = strlen(g_cwd) + 1;
    if (!buf) {
        if (size && size < len) { errno = ERANGE; return NULL; }
        return strdup(g_cwd);
    }
    if (size < len) { errno = size ? ERANGE : EINVAL; return NULL; }
    memcpy(buf, g_cwd, len);
    return buf;
}

// No symbolic links to resolve on the console's file systems: the path made absolute and folded, if it exists.
char *__wrap_realpath(const char *path, char *resolved) {
    char buf[PATH_MAX];
    const char *p = absolute(path, buf);
    if (!p) return NULL;
    struct stat st;
    if (__real_stat(p, &st) != 0) return NULL;
    if (!resolved) return strdup(p);
    memcpy(resolved, p, strlen(p) + 1);
    return resolved;
}
