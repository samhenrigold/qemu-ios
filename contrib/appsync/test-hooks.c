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

extern CFTypeRef CFDictionaryGetValue(CFDictionaryRef, const void *);
const CFStringRef kMISValidationInfoValidatedByProfile =
    (CFStringRef)__builtin___CFStringMakeConstantString("ValidatedByProfile");
const CFStringRef kMISValidationInfoSignerCertificate =
    (CFStringRef)__builtin___CFStringMakeConstantString("SignerCertificate");
static const char *program = "installd";
static CFDictionaryRef original_info;
static SecCertificateRef original_certificate;
static CFStringRef original_summary;
static int calls, result = -42;
const char *test_program(void) { return program; }
static int verify(void *p, void *o) { assert(p == (void *)1 && o == (void *)2); calls++; return result; }
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
    puts("PASS: original signing info, certificate scope, Copy ownership, and process scope");
}
