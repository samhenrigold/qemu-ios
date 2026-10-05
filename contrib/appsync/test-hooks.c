// Native behavioral checks with real CoreFoundation objects and mock originals.
// No guest interposition or Security trust-store dependency in this test.
#define APPSYNC_TEST 1
#define dlsym test_symbol
#define getprogname test_program
#include "appsync.c"
#undef dlsym
#undef getprogname
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
extern long write(int, const void *, unsigned long);
extern int unlink(const char *);

extern CFTypeRef CFDictionaryGetValue(CFDictionaryRef, const void *);
const CFStringRef kMISValidationInfoValidatedByProfile =
    (CFStringRef)__builtin___CFStringMakeConstantString("ValidatedByProfile");
const CFStringRef kMISValidationInfoSignerCertificate =
    (CFStringRef)__builtin___CFStringMakeConstantString("SignerCertificate");
static const char *program = "installd";
static CFDictionaryRef original_info;
static SecCertificateRef original_certificate;
static CFStringRef original_summary;
static int calls, result = -42, ios6;
static void *expected_path = (void *)1;
static const CFStringRef signing_id_key =
    (CFStringRef)__builtin___CFStringMakeConstantString("SigningID");
static const CFStringRef entitlements_key =
    (CFStringRef)__builtin___CFStringMakeConstantString("Entitlements");
const char *test_program(void) { return program; }
static int verify(void *p, void *o) { assert(p == expected_path && o == (void *)2); calls++; return result; }
static int verify_info(void *p, void *o, CFDictionaryRef *i) {
    verify(p, o);
    if (i && original_info) *i = (CFDictionaryRef)CFRetain(original_info);
    return result;
}
static SecCertificateRef certificate(CFAllocatorRef a, CFDataRef d) {
    (void)a; (void)d; calls++; return original_certificate;
}
static CFStringRef summary(SecCertificateRef c) {
    (void)c; calls++; return original_summary;
}
void *test_symbol(void *handle, const char *name) {
    if (handle == RTLD_DEFAULT) {
        if (!ios6) return 0;
        if (!strcmp(name, "kMISValidationInfoSigningID")) return (void *)&signing_id_key;
        if (!strcmp(name, "kMISValidationInfoEntitlements")) return (void *)&entitlements_key;
        assert(0);
    }
    assert(handle == RTLD_NEXT);
    if (!strcmp(name, "MISValidateSignature")) return verify;
    if (!strcmp(name, "MISValidateSignatureAndCopyInfo")) return verify_info;
    if (!strcmp(name, "SecCertificateCreateWithData")) return certificate;
    if (!strcmp(name, "SecCertificateCopySubjectSummary")) return summary;
    assert(0); return 0;
}
int main(void) {
    CFDictionaryRef info = (CFDictionaryRef)1;
    assert(as_MISValidateSignatureAndCopyInfo((void *)1, (void *)2, &info) == 0);
    assert(calls == 1 && info);
    CFDataRef der = (CFDataRef)CFDictionaryGetValue(info, kMISValidationInfoSignerCertificate);
    assert(is_appsync_certificate(der));
    assert(CFDictionaryGetValue(info, kMISValidationInfoValidatedByProfile) == kCFBooleanTrue);
    SecCertificateRef standin = as_SecCertificateCreateWithData(0, der);
    assert((CFTypeRef)standin == (CFTypeRef)der);
    CFStringRef text = as_SecCertificateCopySubjectSummary(standin);
    assert(text); CFRelease(text); CFRelease((CFTypeRef)standin);

    // Malformed/unrelated data must retain Security's rejection.
    CFDataRef bad = CFDataCreate(0, (const unsigned char *)"bad", 3);
    assert(!as_SecCertificateCreateWithData(0, bad));
    original_summary = CFStringCreateWithCString(0, "Original", 0x0600);
    assert(as_SecCertificateCopySubjectSummary((SecCertificateRef)original_summary) == original_summary);
    original_certificate = (SecCertificateRef)original_summary;
    assert(as_SecCertificateCreateWithData(0, der) == original_certificate);
    original_certificate = 0;

    // Preserve the complete original dictionary, including unknown future keys,
    // even if the verifier returns a failure alongside usable metadata.
    CFStringRef extra = CFStringCreateWithCString(0, "Apple metadata", 0x0600);
    CFDictionarySetValue((CFMutableDictionaryRef)info,
        __builtin___CFStringMakeConstantString("unknown-key"), extra);
    CFRelease(extra);
    original_info = info;
    CFDictionaryRef preserved = 0;
    assert(as_MISValidateSignatureAndCopyInfo((void *)1, (void *)2, &preserved) == 0);
    assert(preserved == original_info);
    assert(CFDictionaryGetValue(preserved, __builtin___CFStringMakeConstantString("unknown-key")) == extra); CFRelease(preserved);
    assert(as_MISValidateSignatureAndCopyInfo((void *)1, (void *)2, 0) == 0);
    assert(as_MISValidateSignature((void *)1, (void *)2) == 0);
    program = "mobile_installation_proxy";
    assert(as_MISValidateSignature((void *)1, (void *)2) == 0);
    program = "SpringBoard";
    assert(as_MISValidateSignature((void *)1, (void *)2) == result);
    assert(!as_SecCertificateCreateWithData(0, der));
    original_info = 0; preserved = (CFDictionaryRef)1;
    assert(as_MISValidateSignatureAndCopyInfo((void *)1, (void *)2, &preserved) == result);
    assert(!preserved);
    CFRelease(bad); CFRelease(info); CFRelease(original_summary);

    // iOS 6: identifier and entitlements come from a 32-bit Mach-O's signature.
    static const char ents[] = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
        "<key>get-task-allow</key><true/></dict></plist>";
    unsigned char m[512] = {0};
    unsigned *w = (unsigned *)m;
    w[0] = 0xfeedface; w[4] = 1; w[5] = 16;           // magic, ncmds, sizeofcmds
    w[7] = 0x1d; w[8] = 16; w[9] = 64; w[10] = 400;  // LC_CODE_SIGNATURE at 64
    unsigned char *sb = m + 64;
    #define BE(p, v) ((p)[0] = (unsigned char)((v) >> 24), (p)[1] = (unsigned char)((v) >> 16), (p)[2] = (unsigned char)((v) >> 8), (p)[3] = (unsigned char)(v))
    BE(sb, 0xfade0cc0); BE(sb + 4, 400); BE(sb + 8, 2);
    BE(sb + 12, 0); BE(sb + 16, 28); BE(sb + 20, 5); BE(sb + 24, 96);
    BE(sb + 28, 0xfade0c02); BE(sb + 32, 60); BE(sb + 48, 40);
    memcpy(sb + 68, "com.example.harness", 20);
    BE(sb + 96, 0xfade7171); BE(sb + 100, 8 + (unsigned)strlen(ents));
    memcpy(sb + 104, ents, strlen(ents));
    char file[] = "/tmp/appsync-macho.XXXXXX";
    int fd = mkstemp(file);
    assert(fd >= 0 && write(fd, m, sizeof m) == sizeof m); close(fd);
    ios6 = 1; original_info = 0; program = "installd";
    expected_path = (void *)CFStringCreateWithCString(0, file, 0x08000100);
    CFDictionaryRef six = 0;
    assert(as_MISValidateSignatureAndCopyInfo(expected_path, (void *)2, &six) == 0 && six);
    CFStringRef ident = (CFStringRef)CFDictionaryGetValue(six, signing_id_key);
    char got[64];
    assert(ident && CFStringGetCString(ident, got, sizeof got, 0x08000100));
    assert(!strcmp(got, "com.example.harness"));
    CFDictionaryRef e = (CFDictionaryRef)CFDictionaryGetValue(six, entitlements_key);
    assert(e && CFDictionaryGetValue(e, __builtin___CFStringMakeConstantString("get-task-allow")) == kCFBooleanTrue);
    CFRelease(six);
    // Unsigned: entitlements fall back to an empty dictionary.
    w[7] = 0; fd = open(file, 1); assert(write(fd, m, sizeof m) == sizeof m); close(fd);
    assert(as_MISValidateSignatureAndCopyInfo(expected_path, (void *)2, &six) == 0 && six);
    assert(!CFDictionaryGetValue(six, signing_id_key) && CFDictionaryGetValue(six, entitlements_key));
    CFRelease(six); CFRelease(expected_path); unlink(file);
    puts("PASS: original signing info, iOS 6 signature info, certificate scope, Copy ownership, and process scope");
}
