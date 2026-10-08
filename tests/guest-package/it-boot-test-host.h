/* it_boot's host side for it-boot-test.sh: a fake qc() and system, included by it_boot.c as IT_BOOT_TEST.
 * The fake host is a directory: `offer` is what QC_PKG_OFFER serves and each payload is served by index
 * from serve/<the path its offer line names>, as the real host does. `silent` makes every call fail,
 * `corrupt` names an index whose bytes get flipped, `fail_at` an "index offset" where reads start failing,
 * `slow` and `tick_step` advance the monotonic clock per read, `nosha` and `readonly` stand for a
 * libSystem without CommonCrypto and 7.x's read-only root. Reports and launchctl calls are appended
 * to files for the checks. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static char fake_dir[1024];
static void fake_path(char *out, const char *name) { snprintf(out, 1024, "%s/%s", fake_dir, name); }
static int fake_exists(const char *name) { char p[1024]; fake_path(p, name); return access(p, F_OK) == 0; }
static const char *root_dir(void) { static char p[1024]; fake_path(p, "root"); return p; }
static const char *sys_root(void) { static char p[1024]; fake_path(p, "sys"); return p; }
static int sha_disabled(void) { return fake_exists("nosha"); }
static int root_readonly(void) { return fake_exists("readonly"); }
static void os_build(char *out, size_t n) {
    char p[1024]; fake_path(p, "build"); FILE *f = fopen(p, "r");
    snprintf(out, n, "7E18");
    if (f) { if (!fgets(out, (int)n, f)) out[0] = 0; out[strcspn(out, "\n")] = 0; fclose(f); }
}
static int launchctl(const char *verb, const char *arg) {
    char p[1024]; fake_path(p, "launchctl"); FILE *f = fopen(p, "a");
    fprintf(f, "%s %s\n", verb, arg); fclose(f); return 0;
}
static uint64_t fake_ticks;
static uint64_t fake_mach_absolute_time(void) { return fake_ticks; }
static kern_return_t fake_mach_timebase_info(mach_timebase_info_t info) {
    info->numer = 1; info->denom = 1; return 0;
}
static time_t fake_time(time_t *out) {
    time_t value = fake_ticks ? 2000000000 : 1000000000;
    if (out) *out = value;
    return value;
}
#define mach_absolute_time fake_mach_absolute_time
#define mach_timebase_info fake_mach_timebase_info
#define time fake_time
static char offer_buf[65536];
static long offer_len = -1;
static long fake_load(char *buf, size_t cap, const char *name) {
    char p[1024]; fake_path(p, name); FILE *f = fopen(p, "rb");
    if (!f) return -1;
    long n = (long)fread(buf, 1, cap, f); fclose(f); buf[n] = 0; return n;
}
/* index -> the path its offer line names, the way hw/arm/guest-package.c maps it */
static int fake_index_path(unsigned idx, char *out) {
    char copy[65536]; memcpy(copy, offer_buf, (size_t)offer_len); copy[offer_len] = 0;
    char *save = NULL;
    for (char *l = strtok_r(copy, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        char kind[16], path[512]; unsigned i;
        if (sscanf(l, "%15s %u %511s", kind, &i, path) == 3 && i == idx &&
            (!strcmp(kind, "file") || !strcmp(kind, "job") || !strcmp(kind, "hook"))) {
            snprintf(out, 1024, "%s/serve/%s", fake_dir, path); return 0;
        }
    }
    return -1;
}
static int64_t qc(uint32_t op, void *buf, uint32_t off, uint32_t len, uint64_t token) {
    assert(len <= 1024);
    if (fake_exists("silent")) return -1;
    if (op == 0x170) {
        if (off == 0) offer_len = fake_load(offer_buf, sizeof(offer_buf) - 1, "offer");
        if (offer_len < 0) return -1;
        if (!len) return offer_len;
        if (off >= offer_len) return 0;
        long n = offer_len - off < (long)len ? offer_len - off : (long)len;
        memcpy(buf, offer_buf + off, (size_t)n); return n;
    }
    if (op == 0x171) {
        if (len) {
            char step[64];
            uint64_t elapsed = fake_load(step, sizeof(step) - 1, "tick_step") > 0 ? strtoull(step, NULL, 10) : 1;
            fake_ticks += fake_exists("slow") ? UINT64_C(6000000000) : elapsed;
        }
        char p[1024]; static char data[1 << 20];
        if (offer_len < 0 || fake_index_path((unsigned)token, p)) return -1;
        FILE *f = fopen(p, "rb"); if (!f) return -1;
        long size = (long)fread(data, 1, sizeof(data), f); fclose(f);
        if (!len) return size;
        char spec[64]; unsigned fi, fo;
        if (fake_load(spec, sizeof(spec) - 1, "fail_at") > 0 && sscanf(spec, "%u %u", &fi, &fo) == 2 &&
            fi == token && off >= fo) return -1;
        if (off >= size) return 0;
        long n = size - off < (long)len ? size - off : (long)len;
        memcpy(buf, data + off, (size_t)n);
        if (fake_load(spec, sizeof(spec) - 1, "corrupt") > 0 && (unsigned)atoi(spec) == token && off == 0)
            ((char *)buf)[0] ^= 0x55;
        return n;
    }
    if (op == 0x172) {
        char p[1024]; fake_path(p, "reports"); FILE *f = fopen(p, "a");
        fprintf(f, "%llu %d %.*s\n", (unsigned long long)token, (int32_t)off, (int)len, (char *)buf);
        fclose(f); return 0;
    }
    return -1;
}
