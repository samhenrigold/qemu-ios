// LightTouch AppSync: process-local installation hooks for iOS 2.x-5.x.
// Injected only into installd or mobile_installation_proxy. Symbol-bound dyld
// interposition preserves original signing information and valid certificates;
// the legacy fallback supplies the two fields required by verify_signer_identity.
// See README.md for prior art and the emulator kernel execution policy.

#include "appsync_cert.h"

typedef unsigned char Boolean;

// ---- minimal CoreFoundation / Security decls (no SDK headers needed) -------
typedef const struct __CFString      *CFStringRef;
typedef const struct __CFDictionary  *CFDictionaryRef;
typedef struct __CFDictionary        *CFMutableDictionaryRef;
typedef const struct __CFAllocator   *CFAllocatorRef;
typedef const struct __CFData        *CFDataRef;
typedef const void                   *CFTypeRef;
typedef signed long                   CFIndex;

extern const CFAllocatorRef kCFAllocatorDefault;
// These exports are callback STRUCTS, not pointers. Passing the version word
// as a pointer disabled value retention and left SignerCertificate dangling.
typedef struct {
    CFIndex version;
    const void *retain, *release, *copyDescription, *equal, *hash;
} CFDictionaryKeyCallBacks;
typedef struct {
    CFIndex version;
    const void *retain, *release, *copyDescription, *equal;
} CFDictionaryValueCallBacks;
extern const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
extern const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
extern const void *const kCFBooleanTrue;   // CFBooleanRef singleton (a pointer value)
extern CFDataRef   CFDataCreate(CFAllocatorRef, const unsigned char *, CFIndex);
extern CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef, CFIndex,
                                    const void *, const void *);
extern void        CFDictionarySetValue(CFMutableDictionaryRef, const void *, const void *);
extern void        CFRelease(CFTypeRef);

// libmis exports these keys; installd imports them, so they resolve at load.
extern const CFStringRef kMISValidationInfoValidatedByProfile;
extern const CFStringRef kMISValidationInfoSignerCertificate;

// SecCertificateRef from DER; installd stores it under the signer key.
typedef struct __SecCertificate *SecCertificateRef;
extern SecCertificateRef SecCertificateCreateWithData(CFAllocatorRef, CFDataRef);
extern CFStringRef SecCertificateCopySubjectSummary(SecCertificateRef);
extern CFTypeRef CFRetain(CFTypeRef);
extern unsigned long CFGetTypeID(CFTypeRef);
extern unsigned long CFDataGetTypeID(void);
extern CFIndex CFDataGetLength(CFDataRef);
extern const unsigned char *CFDataGetBytePtr(CFDataRef);
extern int memcmp(const void *, const void *, unsigned long);
extern CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char *, unsigned int);
extern void *dlsym(void *, const char *);
#ifndef RTLD_NEXT
#define RTLD_NEXT ((void *)-1L)
#endif

extern const char *getprogname(void);
extern int strcmp(const char *, const char *);
static int in_installd(void) {
    const char *p = getprogname();
    return p && (!strcmp(p, "installd") || !strcmp(p, "mobile_installation_proxy"));
}

// Resolve the original rather than replacing Apple's signing information. This
// follows AppSync Unified's original-first behavior; the legacy fallback below
// remains necessary for 2.x-5.x installation services on fresh emulator stores.
int MISValidateSignature(void *, void *);
int MISValidateSignatureAndCopyInfo(void *, void *, CFDictionaryRef *);

static int as_MISValidateSignature(void *path, void *options) {
    // The old two-argument API returns no metadata. Keep the proven 2.x path:
    // its original verifier contributes nothing to the installation result.
    if (in_installd()) return 0;
    int (*real)(void *, void *) = dlsym(RTLD_NEXT, "MISValidateSignature");
    return real ? real(path, options) : -1;
}

static int as_MISValidateSignatureAndCopyInfo(void *path, void *options,
                                             CFDictionaryRef *info) {
    int (*real)(void *, void *, CFDictionaryRef *) =
        dlsym(RTLD_NEXT, "MISValidateSignatureAndCopyInfo");
    // Copy APIs may leave the out parameter untouched on failure.
    if (info) *info = 0;
    int result = real ? real(path, options, info) : -1;
    if (!in_installd()) return result;
    if (info && !*info) {
        CFMutableDictionaryRef d = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (!d) return -1;
        CFDataRef der = CFDataCreate(kCFAllocatorDefault,
                                    kAppSyncCertDER, (CFIndex)kAppSyncCertDERLen);
        if (!der) { CFRelease((CFTypeRef)d); return -1; }
        CFDictionarySetValue(d, kMISValidationInfoValidatedByProfile, kCFBooleanTrue);
        CFDictionarySetValue(d, kMISValidationInfoSignerCertificate, der);
        CFRelease((CFTypeRef)der);
        *info = d; // Copy ownership passes to the caller.
    }
    return 0;
}

static int is_appsync_certificate(CFTypeRef value) {
    return value && CFGetTypeID(value) == CFDataGetTypeID() &&
        CFDataGetLength((CFDataRef)value) == (CFIndex)kAppSyncCertDERLen &&
        !memcmp(CFDataGetBytePtr((CFDataRef)value), kAppSyncCertDER, kAppSyncCertDERLen);
}

// Keep real certificates and errors intact. Only our synthetic signing-info
// certificate may use CFData as a stand-in when legacy Security rejects it.
static SecCertificateRef as_SecCertificateCreateWithData(CFAllocatorRef a, CFDataRef d) {
    SecCertificateRef (*real)(CFAllocatorRef, CFDataRef) =
        dlsym(RTLD_NEXT, "SecCertificateCreateWithData");
    SecCertificateRef result = real ? real(a, d) : 0;
    if (!result && in_installd() && is_appsync_certificate((CFTypeRef)d))
        result = (SecCertificateRef)CFRetain((CFTypeRef)d);
    return result;
}

static CFStringRef as_SecCertificateCopySubjectSummary(SecCertificateRef c) {
    if (in_installd() && is_appsync_certificate((CFTypeRef)c))
        return CFStringCreateWithCString(kCFAllocatorDefault, "AppSync", 0x0600);
    CFStringRef (*real)(SecCertificateRef) =
        dlsym(RTLD_NEXT, "SecCertificateCopySubjectSummary");
    return real ? real(c) : 0;
}

// dyld interposition: replace/original pairs in __DATA,__interpose.
#ifndef APPSYNC_TEST
typedef struct { const void *replacement; const void *original; } interpose_t;
__attribute__((used)) static const interpose_t as_interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)as_MISValidateSignature,        (const void *)MISValidateSignature },
    { (const void *)as_MISValidateSignatureAndCopyInfo, (const void *)MISValidateSignatureAndCopyInfo },
    { (const void *)as_SecCertificateCreateWithData,     (const void *)SecCertificateCreateWithData },
    { (const void *)as_SecCertificateCopySubjectSummary, (const void *)SecCertificateCopySubjectSummary },
};

#endif
