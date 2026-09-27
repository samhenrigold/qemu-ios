// LightTouch AppSync: one dylib, injected into installd (install path) and
// SpringBoard (launch path) via DYLD_INSERT_LIBRARIES. Everything binds by
// symbol name or ObjC selector, so the same fat armv6+armv7 binary works on
// the iPod (3.1.3) and iPad (3.2.2) with no hand-found firmware offsets.
//
// installd  -> interpose libmis's two signature checks: MISValidateSignature
//              returns success; MISValidateSignatureAndCopyInfo returns success
//              AND fills the caller's info dict with the profile flag and a
//              signer certificate, which is what installd's verify_signer_identity
//              reads out of the result. Returning an empty dict is NOT enough
//              (installd then errors on the missing SignerCertificate); this is
//              the whole reason the dict is populated rather than left blank.
// SpringBoard -> swizzle -[SBApplication applicationSignatureState] to the
//              trusted value (2). ObjC selector, so no SpringBoard offset.
//
// The one embedded fact is a signer certificate (appsync_cert.h): any valid
// DER works, installd only reads its subject summary for logging. Not
// per-firmware.
//
// See README.md. Out of scope: kernel code-signing on later iOS (needs
// per-major-version work; this dylib does not touch the kernel).

#include "appsync_cert.h"

typedef unsigned char Boolean;

// minimal objc runtime decls (SDK's objc headers need compiler builtins that
// -nostdinc strips; declaring what we use avoids the whole include chain).
typedef struct objc_class  *Class;
typedef struct objc_object *id;
typedef struct objc_selector *SEL;
typedef struct objc_method *Method;
typedef id (*IMP)(id, SEL, ...);
extern Class  objc_getClass(const char *);
extern SEL    sel_registerName(const char *);
extern Method class_getInstanceMethod(Class, SEL);
extern IMP    method_setImplementation(Method, IMP);
extern Boolean class_addMethod(Class, SEL, IMP, const char *);

// ---- minimal CoreFoundation / Security decls (no SDK headers needed) -------
typedef const struct __CFString      *CFStringRef;
typedef const struct __CFDictionary  *CFDictionaryRef;
typedef struct __CFDictionary        *CFMutableDictionaryRef;
typedef const struct __CFAllocator   *CFAllocatorRef;
typedef const struct __CFData        *CFDataRef;
typedef const void                   *CFTypeRef;
typedef signed long                   CFIndex;

extern const CFAllocatorRef kCFAllocatorDefault;
extern const void *kCFTypeDictionaryKeyCallBacks;
extern const void *kCFTypeDictionaryValueCallBacks;
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

// ---- libmis hooks ----------------------------------------------------------
// Signatures match how installd calls them (2 args / 3 args). Args are ignored;
// we always report success. Extra trailing args, if any exist on some version,
// are harmless under the C calling convention for a hook that reads none.

int MISValidateSignature(void *a, void *b);           // original (interposed below)
int MISValidateSignatureAndCopyInfo(void *path, void *options, CFDictionaryRef *info);

static int as_MISValidateSignature(void *a, void *b) {
    (void)a; (void)b;
    return 0;   // errSecSuccess
}

static int as_MISValidateSignatureAndCopyInfo(void *path, void *options,
                                              CFDictionaryRef *info) {
    (void)path; (void)options;
    if (info) {
        CFMutableDictionaryRef d = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0,
            kCFTypeDictionaryKeyCallBacks, kCFTypeDictionaryValueCallBacks);
        // profile flag: installd checks it's present/true
        CFDictionarySetValue(d, kMISValidationInfoValidatedByProfile,
                             kCFBooleanTrue);
        // signer cert: installd 3.2.2 stores this value and later calls
        // SecCertificateCreateWithData() on it, i.e. it expects raw DER CFData,
        // NOT a SecCertificateRef. Handing it a SecCertificate made installd call
        // -[... length] on it and crash (unrecognized selector). So store CFData.
        CFDataRef der = CFDataCreate(kCFAllocatorDefault,
                                     kAppSyncCertDER, (CFIndex)kAppSyncCertDERLen);
        if (der) {
            CFDictionarySetValue(d, kMISValidationInfoSignerCertificate, der);
            CFRelease((CFTypeRef)der);
        }
        *info = (CFDictionaryRef)d;   // ownership passes to caller (Copy semantics)
    }
    return 0;
}

// dyld interposition: replace/original pairs in __DATA,__interpose.
typedef struct { const void *replacement; const void *original; } interpose_t;
__attribute__((used)) static const interpose_t as_interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)as_MISValidateSignature,        (const void *)MISValidateSignature },
    { (const void *)as_MISValidateSignatureAndCopyInfo, (const void *)MISValidateSignatureAndCopyInfo },
};

// ---- SpringBoard hook ------------------------------------------------------
// -[SBApplication applicationSignatureState] gates launch of installed apps.
// The trusted value is 2 (matches the iPod byte patch: movs r0,#2; bx lr).
// In installd the SBApplication class is absent, so this is a no-op there.
static int as_applicationSignatureState(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return 2;
}

extern const char *getprogname(void);
extern int strcmp(const char *, const char *);

__attribute__((constructor))
static void as_init(void) {
    // Only touch the ObjC runtime in SpringBoard. In installd (a CoreFoundation
    // daemon) calling objc_getClass from a dyld constructor can re-enter and
    // wedge process init, so gate on the program name and do nothing there.
    const char *p = getprogname();
    if (!p || strcmp(p, "SpringBoard") != 0) return;
    Class c = objc_getClass("SBApplication");
    if (!c) return;
    SEL sel = sel_registerName("applicationSignatureState");
    Method m = class_getInstanceMethod(c, sel);
    if (m) {
        method_setImplementation(m, (IMP)as_applicationSignatureState);
    } else {
        // Add it if the method ever moves; still selector-bound, no offset.
        class_addMethod(c, sel, (IMP)as_applicationSignatureState, "i@:");
    }
}
