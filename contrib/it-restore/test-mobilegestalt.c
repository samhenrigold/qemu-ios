/* Host-side validation tests. Synthetic plist data only; no Apple fixtures.
 * xcrun clang -fsanitize=address,undefined test-mobilegestalt.c -o /tmp/test-mg
 * ASAN_OPTIONS=detect_leaks=0 /tmp/test-mg
 */
#include <unistd.h>
#include <sys/stat.h>
/* Record guest ownership policy without requiring a root host test process. */
static uid_t requested_uid;
static gid_t requested_gid;
static int guest_chown(int, uid_t, gid_t);
#define fchown guest_chown
#include "mobilegestalt.c"
#undef fchown
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static int guest_chown(int fd, uid_t uid, gid_t gid)
{
    (void)fd;
    requested_uid = uid;
    requested_gid = gid;
    return 0;
}

static CFDictionaryRef synthetic(const char *xml)
{
    char name[] = "/tmp/it-restore-plist.XXXXXX";
    int fd = mkstemp(name);
    assert(fd >= 0);
    assert(write(fd, xml, strlen(xml)) == (ssize_t)strlen(xml));
    assert(close(fd) == 0);
    CFDictionaryRef result = read_plist(name);
    assert(unlink(name) == 0);
    assert(result);
    return result;
}

int main(void)
{
    assert(load_cf() == 0);
    plan = synthetic(
        "<plist version=\"1.0\"><dict><key>Build</key><string>test-build</string>"
        "<key>Overrides</key><dict><key>test-policy</key><true/></dict></dict></plist>");
    CFMutableDictionaryRef cache = (CFMutableDictionaryRef)synthetic(
        "<plist version=\"1.0\"><dict><key>CacheVersion</key><string>test-build</string>"
        "<key>CacheUUID</key><string>01234567-89AB-CDEF-0123-456789ABCDEF</string>"
        "<key>CacheData</key><data>AQID</data><key>CacheExtra</key><dict/>"
        "</dict></plist>");
    assert(valid_cache(NULL, 0) == 0);
    assert(valid_cache(cache, 0) == 1);
    assert(valid_cache(cache, 1) == 0); /* unapplied plan */
    CFDictionarySetValue(
        (CFMutableDictionaryRef)CFDictionaryGetValue(cache,
                                                     CFSTR("CacheExtra")),
        CFSTR("test-policy"),
        CFDictionaryGetValue(CFDictionaryGetValue(plan, CFSTR("Overrides")),
                             CFSTR("test-policy")));
    assert(valid_cache(cache, 1) == 1);
    CFDictionarySetValue(cache, CFSTR("CacheVersion"),
                         CFSTR("different-build"));
    assert(valid_cache(cache, 1) == 0);
    CFDictionarySetValue(cache, CFSTR("CacheVersion"), CFSTR("test-build"));
    CFDictionarySetValue(cache, CFSTR("CacheUUID"), CFSTR("not-a-uuid"));
    assert(valid_cache(cache, 1) == 0);
    CFDictionarySetValue(cache, CFSTR("CacheUUID"),
                         CFSTR("01234567-89AB-CDEF-0123-456789ABCDEF"));
    original_uuid = CFRetain(CFDictionaryGetValue(cache, CFSTR("CacheUUID")));
    original_data = CFRetain(CFDictionaryGetValue(cache, CFSTR("CacheData")));
    assert(valid_cache(cache, 1) == 1);
    CFMutableDataRef other = CFDataCreateMutable(NULL, 1);
    CFDataSetLength(other, 1);
    CFDataGetMutableBytePtr(other)[0] = 99;
    CFDictionarySetValue(cache, CFSTR("CacheData"), other);
    assert(valid_cache(cache, 1) == 0); /* opaque native bytes changed */
    CFDataSetLength(other, 0);
    assert(valid_cache(cache, 0) == 0);
    CFRelease(other);
    CFRelease(cache);
    CFRelease(original_data);
    CFRelease(original_uuid);
    original_data = NULL;
    original_uuid = NULL;
    /* Post-migration reapplication must preserve the native writer's file
     * metadata, unrelated answers, UUID and opaque bytes. */
    cache = (CFMutableDictionaryRef)synthetic(
        "<plist version=\"1.0\"><dict><key>CacheVersion</key><string>test-build</string>"
        "<key>CacheUUID</key><string>01234567-89AB-CDEF-0123-456789ABCDEF</string>"
        "<key>CacheData</key><data>AQID</data><key>CacheExtra</key><dict>"
        "<key>unrelated-native-answer</key><string>keep</string></dict></dict></plist>");
    char target[] = "/tmp/it-seal-cache.XXXXXX";
    int fd = mkstemp(target);
    assert(fd >= 0);
    CFDataRef bytes = CFPropertyListCreateData(
        NULL, cache, kCFPropertyListBinaryFormat_v1_0, 0, NULL);
    assert(bytes && write(fd, CFDataGetBytePtr(bytes),
                          CFDataGetLength(bytes)) == CFDataGetLength(bytes));
    assert(close(fd) == 0 && chmod(target, 0640) == 0);
    struct stat before, after;
    assert(stat(target, &before) == 0);
    assert(apply_plan_at(stderr, 1, target) == 0);
    assert(stat(target, &after) == 0 && before.st_uid == after.st_uid &&
           before.st_gid == after.st_gid);
    assert(requested_uid == before.st_uid && requested_gid == before.st_gid);
    assert((after.st_mode & 0777) == 0640);
    CFDictionaryRef result = read_plist(target);
    assert(valid_cache(result, 1));
    assert(CFEqual(CFDictionaryGetValue(result, CFSTR("CacheData")),
                   CFDictionaryGetValue(cache, CFSTR("CacheData"))));
    assert(CFEqual(CFDictionaryGetValue(result, CFSTR("CacheUUID")),
                   CFDictionaryGetValue(cache, CFSTR("CacheUUID"))));
    assert(CFEqual(
        CFDictionaryGetValue(CFDictionaryGetValue(result, CFSTR("CacheExtra")),
                             CFSTR("unrelated-native-answer")),
        CFSTR("keep")));
    assert(unlink(target) == 0);
    CFRelease(original_data);
    CFRelease(original_uuid);
    original_data = NULL;
    original_uuid = NULL;
    /* A native mobile reader must be able to read a new root-owned iOS 10
     * cache. Exercise the atomic writer under a restrictive inherited umask;
     * legacy caches must remain private to their mobile owner. */
    const char *builds[] = {"14G61", "12A365"};
    for (unsigned i = 0; i < 2; i++) {
        CFDictionarySetValue((CFMutableDictionaryRef)plan, CFSTR("Build"),
                             CFSTR(builds[i]));
        CFDictionarySetValue(cache, CFSTR("CacheVersion"), CFSTR(builds[i]));
        CFDataRef input = CFPropertyListCreateData(
            NULL, cache, kCFPropertyListBinaryFormat_v1_0, 0, NULL);
        fd = open(target, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0);
        assert(write(fd, CFDataGetBytePtr(input), CFDataGetLength(input)) ==
               CFDataGetLength(input));
        assert(close(fd) == 0);
        CFRelease(input);
        mode_t mask = umask(0777);
        assert(apply_plan_at(stderr, 0, target) == 0);
        umask(mask);
        assert(stat(target, &after) == 0);
        assert(requested_uid == (i == 0 ? 0 : 501) &&
               requested_gid == requested_uid);
        assert((after.st_mode & 0777) == (i == 0 ? 0644 : 0600));
        CFDictionaryRef written = read_plist(target);
        assert(valid_cache(written, 1));
        CFRelease(written);
        assert(unlink(target) == 0);
        CFRelease(original_data);
        CFRelease(original_uuid);
        original_data = NULL;
        original_uuid = NULL;
    }
    CFRelease(result);
    CFRelease(bytes);
    CFRelease(cache);
    CFRelease(plan);
    puts("MobileGestalt cache validation: PASS (synthetic data)");
    return 0;
}
