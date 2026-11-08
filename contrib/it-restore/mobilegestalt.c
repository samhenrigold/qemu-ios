/* Optional FirmwareKit plan. The temporary full-System worker generates
 * native opaque cache bytes; policy keys come only from the preparation plan. */
#include "mobilegestalt.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <semaphore.h>
#include <stdlib.h>

/* Stable 32-bit CF C ABI, loaded like the other legacy guest tools. This
 * keeps the build usable with a libSystem-only SDK. CF owns plist parsing. */
typedef const void *CFTypeRef, *CFPropertyListRef, *CFDictionaryRef, *CFDataRef,
    *CFStringRef, *CFUUIDRef;
typedef void *CFMutableDictionaryRef, *CFMutableDataRef;
typedef long CFIndex;
typedef unsigned long CFTypeID;
#define kCFPropertyListMutableContainersAndLeaves 2
#define kCFPropertyListBinaryFormat_v1_0 200
#define CF_API_LIST(X)                                                         \
    X(CFMutableDataRef, CFDataCreateMutable, (void *, CFIndex))                \
    X(void, CFDataSetLength, (CFMutableDataRef, CFIndex))                      \
    X(unsigned char *, CFDataGetMutableBytePtr, (CFMutableDataRef))            \
    X(const unsigned char *, CFDataGetBytePtr, (CFDataRef))                    \
    X(CFIndex, CFDataGetLength, (CFDataRef))                                   \
    X(CFPropertyListRef, CFPropertyListCreateWithData,                         \
      (void *, CFDataRef, unsigned long, void *, void *))                      \
    X(CFDataRef, CFPropertyListCreateData,                                     \
      (void *, CFPropertyListRef, long, unsigned long, void *))                \
    X(void, CFRelease, (CFTypeRef))                                            \
    X(CFTypeRef, CFRetain, (CFTypeRef))                                        \
    X(CFTypeID, CFGetTypeID, (CFTypeRef))                                      \
    X(CFTypeID, CFDictionaryGetTypeID, (void))                                 \
    X(CFTypeID, CFStringGetTypeID, (void))                                     \
    X(CFTypeID, CFBooleanGetTypeID, (void))                                    \
    X(CFTypeID, CFDataGetTypeID, (void))                                       \
    X(unsigned char, CFEqual, (CFTypeRef, CFTypeRef))                          \
    X(CFTypeRef, CFDictionaryGetValue, (CFDictionaryRef, CFTypeRef))           \
    X(CFIndex, CFDictionaryGetCount, (CFDictionaryRef))                        \
    X(void, CFDictionaryGetKeysAndValues,                                      \
      (CFDictionaryRef, const void **, const void **))                         \
    X(void, CFDictionarySetValue,                                              \
      (CFMutableDictionaryRef, CFTypeRef, CFTypeRef))                          \
    X(unsigned char, CFStringGetCString,                                       \
      (CFStringRef, char *, CFIndex, unsigned int))                            \
    X(CFStringRef, CFStringCreateWithCString,                                  \
      (void *, const char *, unsigned int))
#define DECLARE(result, name, args) static result(*name) args;
CF_API_LIST(DECLARE)
#undef DECLARE
/* A bounded number of constant keys, kept alive for this short-lived tool. */
#define CFSTR(s) CFStringCreateWithCString(NULL, (s), 0x08000100)
static int load_cf(void)
{
    void *cf = dlopen(
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
        RTLD_NOW);
    if (!cf) {
        fprintf(stderr, "A6NATIVEGESTALT CoreFoundation load failed: %s\n",
                dlerror());
        return -1;
    }
#define LOAD(result, name, args)                                               \
    name = dlsym(cf, #name);                                                   \
    if (!name) {                                                               \
        fprintf(stderr, "A6NATIVEGESTALT missing %s: %s\n", #name, dlerror()); \
        return -1;                                                             \
    }
    CF_API_LIST(LOAD)
#undef LOAD
    return 0;
}

#ifdef IT_MG_FULL_SYSTEM
#define PLAN "/private/var/tmp/it-mobilegestalt/plan.plist"
#define DATA_ROOT "/private/var"
#else
#define PLAN "/usr/local/share/qemu/activation-plan.plist"
#define DATA_ROOT "/mnt2"
#endif

/* iOS 10's native container lookup has a filesystem fast path guarded by
 * containermanagerd's readiness semaphore. During this short-lived restore
 * worker no container service runs. Prepare only the known Data container,
 * use that native path, and remove any semaphore we created before returning.
 * The original MobileGestalt library still generates every cache byte. */
static int owns_container_semaphore;
static int release_container_bootstrap(FILE *log)
{
    if (!owns_container_semaphore)
        return 0;
    if (sem_unlink("containermanagerd.fb_check")) {
        fprintf(
            log,
            "A6NATIVEGESTALT temporary container semaphore cleanup failed errno=%d\n",
            errno);
        return -1;
    }
    owns_container_semaphore = 0;
    fprintf(log, "A6NATIVEGESTALT temporary container semaphore removed\n");
    return 0;
}
static void cleanup_container_at_exit(void)
{
    release_container_bootstrap(stderr);
}
static int prepare_container_bootstrap(FILE *log)
{
    const char *group =
        "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache";
    const char *paths[] = {
        "/private/var/containers",
        "/private/var/containers/Shared",
        "/private/var/containers/Shared/SystemGroup",
        "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache",
        "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library",
        "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches"};
    void *lib =
        dlopen("/usr/lib/system/libsystem_containermanager.dylib", RTLD_NOW);
    char *(*path_for)(const char *, const char *, unsigned long long *) =
        lib ? dlsym(lib, "container_system_group_path_for_identifier") : NULL;
    if (!path_for) {
        fprintf(log, "A6NATIVEGESTALT native container API missing\n");
        return -1;
    }
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        struct stat st;
        if ((mkdir(paths[i], 0755) && errno != EEXIST) ||
            lstat(paths[i], &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0) {
            fprintf(
                log,
                "A6NATIVEGESTALT invalid native Data container directory errno=%d\n",
                errno);
            return -1;
        }
    }
    sem_t *sem =
        sem_open("containermanagerd.fb_check", O_CREAT | O_EXCL, 0600, 0);
    if (sem == SEM_FAILED) {
        if (errno != EEXIST)
            return -1;
        /* An existing native service owns its semaphore; never unlink it. */
        sem = sem_open("containermanagerd.fb_check", 0);
        if (sem == SEM_FAILED)
            return -1;
    } else {
        owns_container_semaphore = 1;
        if (atexit(cleanup_container_at_exit)) {
            sem_close(sem);
            release_container_bootstrap(log);
            return -1;
        }
    }
    if (sem_close(sem)) {
        release_container_bootstrap(log);
        return -1;
    }
    unsigned long long error = 0;
    char *native =
        path_for(NULL, "systemgroup.com.apple.mobilegestaltcache", &error);
    int matching = native && !strcmp(native, group) && !error;
    fprintf(log, "A6NATIVEGESTALT native container lookup %s error=%llu\n",
            matching ? "PASS" : "FAIL", error);
    free(native);
    return matching ? 0 : -1;
}

static CFDictionaryRef plan;
static CFDataRef original_data;
static CFStringRef original_uuid;
static int system_group_cache(void)
{
    return plan &&
           CFEqual(CFDictionaryGetValue(plan, CFSTR("Build")), CFSTR("14G61"));
}
static const char *cache_path(void)
{
    return system_group_cache() ? DATA_ROOT
               "/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist"
                                : DATA_ROOT
               "/mobile/Library/Caches/com.apple.MobileGestalt.plist";
}
#define CACHE cache_path()

static CFPropertyListRef read_plist(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n <= 0 || n > 4 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return NULL;
    }
    CFMutableDataRef data = CFDataCreateMutable(NULL, n);
    if (!data) {
        fclose(f);
        return NULL;
    }
    CFDataSetLength(data, n);
    int ok = fread(CFDataGetMutableBytePtr(data), 1, n, f) == (size_t)n;
    fclose(f);
    CFPropertyListRef result =
        ok ? CFPropertyListCreateWithData(
                 NULL, data, kCFPropertyListMutableContainersAndLeaves, NULL,
                 NULL)
           : NULL;
    CFRelease(data);
    return result;
}

static int valid_cache(CFDictionaryRef cache, int verify_overrides)
{
    if (!cache || CFGetTypeID(cache) != CFDictionaryGetTypeID())
        return 0;
    CFTypeRef version = CFDictionaryGetValue(cache, CFSTR("CacheVersion"));
    CFTypeRef uuid = CFDictionaryGetValue(cache, CFSTR("CacheUUID"));
    CFTypeRef opaque = CFDictionaryGetValue(cache, CFSTR("CacheData"));
    CFTypeRef extra = CFDictionaryGetValue(cache, CFSTR("CacheExtra"));
    if (!version ||
        !CFEqual(version, CFDictionaryGetValue(plan, CFSTR("Build"))) ||
        !uuid || CFGetTypeID(uuid) != CFStringGetTypeID() || !opaque ||
        CFGetTypeID(opaque) != CFDataGetTypeID() ||
        CFDataGetLength(opaque) == 0 || !extra ||
        CFGetTypeID(extra) != CFDictionaryGetTypeID())
        return 0;
    /* CFUUIDCreateFromString accepts malformed input on some host releases. */
    char uuid_text[37];
    if (!CFStringGetCString(uuid, uuid_text, sizeof uuid_text, 0x08000100))
        return 0;
    for (unsigned i = 0; i < 36; i++) {
        char c = uuid_text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return 0;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F')))
            return 0;
    }
    if (original_data &&
        (!CFEqual(opaque, original_data) || !CFEqual(uuid, original_uuid)))
        return 0;
    if (verify_overrides) {
        CFDictionaryRef overrides =
            CFDictionaryGetValue(plan, CFSTR("Overrides"));
        CFIndex count = CFDictionaryGetCount(overrides);
        const void *keys[count], *values[count];
        CFDictionaryGetKeysAndValues(overrides, keys, values);
        for (CFIndex i = 0; i < count; i++) {
            CFTypeRef actual = CFDictionaryGetValue(extra, keys[i]);
            if (!actual || !CFEqual(actual, values[i]))
                return 0;
        }
    }
    return 1;
}

static int verify_cache_at(FILE *log, const char *path, const char *stage)
{
    if (!plan)
        return 0;
    CFDictionaryRef cache = read_plist(path);
    int ok = valid_cache(cache, 1);
    if (cache)
        CFRelease(cache);
    fprintf(log, "A6NATIVEGESTALT %s verification %s\n", stage,
            ok ? "PASS" : "FAIL");
    fflush(log);
    return ok ? 0 : -1;
}

int it_verify_mobilegestalt(FILE *log)
{
    return verify_cache_at(log, CACHE, "remount");
}

#ifndef IT_MG_PARENT
static int load_plan(const char *path, FILE *log)
{
    struct stat st;
    if (stat(path, &st))
        return errno == ENOENT ? 0 : -1;
    fprintf(log, "A6NATIVEGESTALT loading plan (%lld bytes)\n",
            (long long)st.st_size);
    fflush(log);
    if (load_cf())
        return -1;
    plan = read_plist(path);
    if (!plan || CFGetTypeID(plan) != CFDictionaryGetTypeID()) {
        fprintf(log, "A6NATIVEGESTALT invalid plan plist\n");
        return -1;
    }
    CFTypeRef build = CFDictionaryGetValue(plan, CFSTR("Build"));
    CFDictionaryRef overrides = CFDictionaryGetValue(plan, CFSTR("Overrides"));
    if (!build || CFGetTypeID(build) != CFStringGetTypeID() || !overrides ||
        CFGetTypeID(overrides) != CFDictionaryGetTypeID()) {
        fprintf(log, "A6NATIVEGESTALT invalid build/override types\n");
        return -1;
    }
    CFIndex count = CFDictionaryGetCount(overrides);
    if (count < 1 || count > 32)
        return -1;
    const void *keys[count], *values[count];
    CFDictionaryGetKeysAndValues(overrides, keys, values);
    for (CFIndex i = 0; i < count; i++)
        if (CFGetTypeID(keys[i]) != CFStringGetTypeID() ||
            CFGetTypeID(values[i]) != CFBooleanGetTypeID())
            return -1;

    return 1;
}

/* Both initialization and post-migration sealing use the same guarded plan
 * application. Preserve the native cache's identity and opaque payload. */
static int apply_plan_at(FILE *log, int preserve_metadata, const char *path)
{
    struct stat metadata;
    if (preserve_metadata &&
        (lstat(path, &metadata) || !S_ISREG(metadata.st_mode)))
        return -1;
    CFDictionaryRef overrides = CFDictionaryGetValue(plan, CFSTR("Overrides"));
    CFIndex count = CFDictionaryGetCount(overrides);
    const void *keys[count], *values[count];
    CFDictionaryGetKeysAndValues(overrides, keys, values);
    CFMutableDictionaryRef cache = (CFMutableDictionaryRef)read_plist(path);
    if (!valid_cache(cache, 0)) {
        struct stat result;
        int found = !stat(CACHE, &result);
        fprintf(
            log,
            "A6NATIVEGESTALT generated cache invalid: file=%d bytes=%lld errno=%d\n",
            found, found ? (long long)result.st_size : -1LL, errno);
        if (cache && CFGetTypeID(cache) == CFDictionaryGetTypeID()) {
            const char *names[] = {"CacheVersion", "CacheUUID", "CacheData",
                                   "CacheExtra"};
            for (unsigned i = 0; i < 4; i++) {
                CFTypeRef value = CFDictionaryGetValue(cache, CFSTR(names[i]));
                char printable[128] = "";
                if (value && CFGetTypeID(value) == CFStringGetTypeID())
                    CFStringGetCString(value, printable, sizeof printable,
                                       0x08000100);
                fprintf(
                    log,
                    "A6NATIVEGESTALT field %s type=%lu text=%s dataBytes=%ld\n",
                    names[i], value ? CFGetTypeID(value) : 0, printable,
                    value && CFGetTypeID(value) == CFDataGetTypeID()
                        ? (long)CFDataGetLength(value)
                        : -1L);
            }
        }
        if (cache)
            CFRelease(cache);
        return -1;
    }
    original_data = CFRetain(CFDictionaryGetValue(cache, CFSTR("CacheData")));
    original_uuid = CFRetain(CFDictionaryGetValue(cache, CFSTR("CacheUUID")));
    CFMutableDictionaryRef extra = (CFMutableDictionaryRef)CFDictionaryGetValue(
        cache, CFSTR("CacheExtra"));
    for (CFIndex i = 0; i < count; i++)
        CFDictionarySetValue(extra, keys[i], values[i]);
    CFDataRef encoded = CFPropertyListCreateData(
        NULL, cache, kCFPropertyListBinaryFormat_v1_0, 0, NULL);
    CFRelease(cache);
    if (!encoded)
        return -1;
    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s.restore-tmp", path) >= (int)sizeof tmp) {
        CFRelease(encoded);
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600), failed = fd < 0;
    CFIndex offset = 0, n = CFDataGetLength(encoded);
    while (!failed && offset < n) {
        ssize_t written =
            write(fd, CFDataGetBytePtr(encoded) + offset, n - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            failed = 1;
        else
            offset += written;
    }
    if (fd >= 0) {
        unsigned owner = system_group_cache() ? 0 : 501;
        unsigned uid = preserve_metadata ? metadata.st_uid : owner;
        unsigned gid = preserve_metadata ? metadata.st_gid : owner;
        /* The modern SystemGroup cache is root-owned, but its native reader
         * MobileGestaltHelper runs as mobile. A root-only temporary-file mode
         * makes that reader reject and regenerate our verified cache. Legacy
         * mobile-owned caches keep their private mode; replay keeps native metadata. */
        unsigned mode = preserve_metadata      ? metadata.st_mode & 0777
                        : system_group_cache() ? 0644
                                               : 0600;
        if (fchown(fd, uid, gid) || fchmod(fd, mode) || fsync(fd))
            failed = 1;
        if (close(fd))
            failed = 1;
    }
    CFRelease(encoded);
    if (!failed && rename(tmp, path))
        failed = 1;
    if (failed) {
        unlink(tmp);
        return -1;
    }
    fprintf(
        log,
        "A6NATIVEGESTALT preserved %ld original opaque bytes; applied %ld planned booleans\n",
        (long)CFDataGetLength(original_data), (long)count);
    fflush(log);
    return verify_cache_at(log, path, "readback");
}

int it_reapply_mobilegestalt(FILE *log, const char *path)
{
    if (!path || load_plan(path, log) != 1)
        return -1;
    return apply_plan_at(log, 1, CACHE);
}

int it_prepare_mobilegestalt(FILE *log)
{
    int loaded = load_plan(PLAN, log);
    if (loaded <= 0)
        return loaded;
    CFTypeRef build = CFDictionaryGetValue(plan, CFSTR("Build"));
    void *lib = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW);
    void (*server)(void) = lib ? dlsym(lib, "_MGSetServer") : NULL;
    void (*write_cache)(void) = lib ? dlsym(lib, "_MGWriteCache") : NULL;
    CFTypeRef (*answer)(CFStringRef, void *) =
        lib ? dlsym(lib, "MGCopyAnswer") : NULL;
    if (!server || !write_cache || !answer) {
        fprintf(log, "A6NATIVEGESTALT native symbols missing: %s\n", dlerror());
        return -1;
    }
    fprintf(log,
            "A6NATIVEGESTALT generating cache with original System library\n");
    fflush(log);
    server();
    CFTypeRef native_build = answer(CFSTR("BuildVersion"), NULL);
    int matching = native_build && CFEqual(native_build, build);
    if (native_build)
        CFRelease(native_build);
    if (!matching) {
        fprintf(log, "A6NATIVEGESTALT original library build mismatch\n");
        return -1;
    }
    if (system_group_cache() && prepare_container_bootstrap(log))
        return -1;
    write_cache();
    if (release_container_bootstrap(log))
        return -1;
    return apply_plan_at(log, 0, CACHE);
}

#endif /* !IT_MG_PARENT */
