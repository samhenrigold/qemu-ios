/* AppSync reads an installing app's own code signature: a short or wrapping length in it is the app's to give, so
 * the parser takes only blobs that lie inside the signature (ASan sees any read past it).
 * SLICE contrib/appsync/appsync.c define HEADER_BYTES
 * SLICE contrib/appsync/appsync.c fn be32 appsync_arm32_slice executable_signature_info
 * CFLAGS -Wall -Wextra -Werror -Wno-unused-parameter
 */
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef const char *CFStringRef;
typedef const void *CFTypeRef;
typedef const void *CFDataRef;
#define kCFAllocatorDefault 0
static unsigned datas;
static CFDataRef CFDataCreate(int a, const unsigned char *p, long n)
{
    assert(n >= 0 && n < 4096);   /* the test's signature is 64 bytes: a wrapped len - 8 is not */
    datas++;
    return 0;
}
static CFTypeRef CFPropertyListCreateFromXMLData(int a, CFDataRef d, int o, int e) { return 0; }
static void CFRelease(CFTypeRef p) {}
static unsigned long CFGetTypeID(CFTypeRef p) { return 0; }
static unsigned long CFDictionaryGetTypeID(void) { return 1; }
static CFStringRef CFStringCreateWithCString(int a, const char *s, unsigned e) { return strdup(s); }
static int CFStringGetCString(CFStringRef s, char *buf, long n, unsigned e)
{
    snprintf(buf, n, "%s", s);
    return 1;
}
static void put(unsigned char *p, unsigned v)   /* big-endian, as the signature is */
{
    for (int i = 0; i < 4; i++) p[i] = v >> (24 - 8 * i);
}
static void le(unsigned char *p, unsigned v) { memcpy(p, &v, 4); }
#include "slice.h"

/* A 32-bit Mach-O whose LC_CODE_SIGNATURE (at 28) points at a 64-byte superblob at 256 holding one entitlements
 * blob at `o` with length `len`. */
static void run(const char *path, unsigned o, unsigned len)
{
    unsigned char f[320] = {0};
    put(f, 0xcefaedfe);
    le(f + 16, 1);
    le(f + 28, 0x1d);
    le(f + 32, 16);
    le(f + 36, 256);
    le(f + 40, 64);
    put(f + 256, 0xfade0cc0);
    put(f + 256 + 4, 64);
    put(f + 256 + 8, 1);
    put(f + 256 + 16, o);
    if (o + 8 <= 64) {
        put(f + 256 + o, 0xfade7171);
        put(f + 256 + o + 4, len);
    }
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    assert(fd >= 0 && write(fd, f, sizeof f) == (long)sizeof f);
    close(fd);
    CFStringRef ident = 0;
    CFTypeRef ents = 0;
    executable_signature_info(path, &ident, &ents);
}

int main(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/appsync-sig-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    close(mkstemp(path));
    run(path, 24, 16);
    assert(datas == 1);   /* a well-formed 16-byte entitlements blob is read */
    for (unsigned len = 0; len < 8; len++) run(path, 24, len);   /* shorter than its own header */
    run(path, 0xfffffff0, 16);                                   /* an offset that wraps o + 24 */
    assert(datas == 1);
    unlink(path);
    puts("PASS: AppSync reads only signature blobs that lie inside the signature");
}
