/*
 * Force-included by cc6 for LEGACY_LINK=1 code: the calls whose ABI changed between 1.x and 2.0.
 *
 * The SDKs this toolchain compiles against (2.0+) are __DARWIN_ONLY_64_BIT_INO_T: `stat` fills the
 * 64-bit-inode struct stat and `readdir` returns the 64-bit struct dirent. 1.x libSystem's `stat` and
 * `readdir` are the older 32-bit-inode ones (st_size reads 0 through the new layout; d_name is at 8,
 * not 21); its `stat64` family is the new layout. Only 1.x exports stat64 (2.0+ has no separate one),
 * so its presence is what selects the old-ABI path, at run time: one binary for 1.x, 2.x and 3.x+.
 */
#ifndef ARMV6_LEGACY_H
#define ARMV6_LEGACY_H
#include <dirent.h>
#include <dlfcn.h>
#include <string.h>
#include <sys/stat.h>

struct legacy_dirent32 {           /* 1.x's struct dirent */
    __uint32_t d_ino;
    __uint16_t d_reclen;
    __uint8_t d_type;
    __uint8_t d_namlen;
    char d_name[256];
};

/* 1.x's stat64/lstat64/fstat64; NULL where stat itself is the 64-bit-inode one */
static inline int legacy_stat(const char *p, struct stat *st)
{
    int (*f)(const char *, struct stat *) = dlsym(RTLD_DEFAULT, "stat64");
    return f ? f(p, st) : stat(p, st);
}

static inline int legacy_lstat(const char *p, struct stat *st)
{
    int (*f)(const char *, struct stat *) = dlsym(RTLD_DEFAULT, "lstat64");
    return f ? f(p, st) : lstat(p, st);
}

static inline int legacy_fstat(int fd, struct stat *st)
{
    int (*f)(int, struct stat *) = dlsym(RTLD_DEFAULT, "fstat64");
    return f ? f(fd, st) : fstat(fd, st);
}

static inline struct dirent *legacy_readdir(DIR *d)
{
    static struct dirent out;
    struct dirent *de = readdir(d);
    if (!de || !dlsym(RTLD_DEFAULT, "stat64")) {
        return de;
    }
    const struct legacy_dirent32 *old = (const void *)de;
    memset(&out, 0, sizeof(out));
    out.d_ino = old->d_ino;
    out.d_reclen = sizeof(out);
    out.d_type = old->d_type;
    out.d_namlen = old->d_namlen;
    memcpy(out.d_name, old->d_name, old->d_namlen);
    return &out;
}

/* function-like, so `struct stat` itself is untouched */
#define stat(p, st) legacy_stat(p, st)
#define lstat(p, st) legacy_lstat(p, st)
#define fstat(fd, st) legacy_fstat(fd, st)
#define readdir(d) legacy_readdir(d)
#endif
