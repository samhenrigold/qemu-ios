// Light Touch AppSync: process-local installation hooks for iOS 2.x-6.x.
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
extern void *memchr(const void *, int, unsigned long);
extern CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char *, unsigned int);
extern void *dlsym(void *, const char *);
#ifndef RTLD_NEXT
#define RTLD_NEXT ((void *)-1L)
#endif
#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *)-2L)
#endif

// iOS 6 installd also requires the code-signing identifier and entitlements.
// Those keys are absent before iOS 6, so they are resolved at runtime.
extern CFIndex CFStringGetLength(CFStringRef);
extern Boolean CFStringGetCString(CFStringRef, char *, CFIndex, unsigned int);
extern CFTypeRef CFPropertyListCreateFromXMLData(CFAllocatorRef, CFDataRef, unsigned long, CFStringRef *);
extern unsigned long CFDictionaryGetTypeID(void);
extern int open(const char *, int, ...);
extern long pread(int, void *, unsigned long, long long);
extern int close(int);
extern void *malloc(unsigned long);
extern void free(void *);

extern const char *getprogname(void);
extern int strcmp(const char *, const char *);
static int in_installd(void) {
    const char *p = getprogname();
    // amfid runs the same signature check on exec (iOS 10): it must get the
    // synthesized info dictionary too, so the kernel accepts the ad-hoc cdhash.
    return p && (!strcmp(p, "installd") || !strcmp(p, "mobile_installation_proxy") ||
                 !strcmp(p, "amfid"));
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

static unsigned be32(const unsigned char *p) {
    return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* Fat apps may put ARM64 first (Pages 2.0 does). This emulator's installd
 * needs the supported 32-bit ARM slice's signing metadata. */
static long appsync_arm32_slice(const unsigned char *header, unsigned size) {
    if (size < 8) return -1;
    if (be32(header) != 0xcafebabe) return 0;
    unsigned count = be32(header + 4);
    if (!count || count > (size - 8) / 20) return -1;
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *arch = header + 8 + 20 * i;
        if (be32(arch) == 12) {
            unsigned offset = be32(arch + 8);
            return offset && offset <= 0x7fffffff ? (long)offset : -1;
        }
    }
    return -1;
}

// Read the first Mach-O slice's identifier and entitlements from its embedded
// signature. A missing or malformed signature leaves both outputs untouched.
// The load commands are read whole: Chrome 21's take 4556 bytes, and with the
// first 4 KiB only its LC_CODE_SIGNATURE (the last) was never found, so 6.x
// installd refused it (code_info_identifier=n, InstallMapUpdateFailed).
#define HEADER_BYTES 65536
extern CFTypeRef CFDictionaryGetValue(CFDictionaryRef, const void *);
extern unsigned long CFStringGetTypeID(void);
extern int snprintf(char *, unsigned long, const char *, ...);
static void executable_signature_info(CFStringRef, CFStringRef *, CFTypeRef *);

// The cdhash of the binary last parsed by executable_signature_info: the SHA-1
// (or SHA-256, truncated to 20) of its best CS_CodeDirectory, as cdhash.m does.
extern void CC_SHA1(const void *, unsigned, unsigned char *);
extern void CC_SHA256(const void *, unsigned, unsigned char *);
static unsigned char as_cdhash[20];
static int as_have_cdhash;

/* Modern installation services pass a bundle directory. Resolve only its
 * declared executable; the embedded signature remains the metadata source. */
static int bundle_signature_info(const char *bundle, CFStringRef *ident, CFTypeRef *ents) {
    char file[1400], executable[256];
    int length = snprintf(file, sizeof file, "%s/Info.plist", bundle);
    if (length < 0 || (unsigned)length >= sizeof file) return 0;
    int fd = open(file, 0);
    if (fd < 0) return 0;
    unsigned char *bytes = malloc(HEADER_BYTES);
    long n = bytes ? pread(fd, bytes, HEADER_BYTES, 0) : -1;
    close(fd);
    if (n <= 0 || n == HEADER_BYTES) { free(bytes); return 0; }
    CFDataRef data = CFDataCreate(0, bytes, n);
    free(bytes);
    CFTypeRef plist = data ? CFPropertyListCreateFromXMLData(0, data, 0, 0) : 0;
    if (data) CFRelease((CFTypeRef)data);
    if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
        if (plist) CFRelease(plist);
        return 0;
    }
    CFStringRef key = CFStringCreateWithCString(0, "CFBundleExecutable", 0x08000100);
    CFTypeRef value = CFDictionaryGetValue((CFDictionaryRef)plist, key);
    CFRelease((CFTypeRef)key);
    if (!value || CFGetTypeID(value) != CFStringGetTypeID() ||
        !CFStringGetCString((CFStringRef)value, executable, sizeof executable, 0x08000100) ||
        !executable[0] || !strcmp(executable, ".") || !strcmp(executable, "..")) {
        CFRelease(plist);
        return 0;
    }
    for (unsigned i = 0; executable[i]; i++) {
        if (executable[i] == '/') { CFRelease(plist); return 0; }
    }
    length = snprintf(file, sizeof file, "%s/%s", bundle, executable);
    if (length > 0 && (unsigned)length < sizeof file) {
        CFStringRef path = CFStringCreateWithCString(0, file, 0x08000100);
        executable_signature_info(path, ident, ents);
        CFRelease((CFTypeRef)path);
    }
    CFRelease(plist);
    return 1;
}

static void executable_signature_info(CFStringRef path, CFStringRef *ident, CFTypeRef *ents) {
    char file[1024];
    if (!path || !CFStringGetCString(path, file, sizeof file, 0x08000100)) return;
    int fd = open(file, 0);
    if (fd < 0) return;
    unsigned char *h = malloc(HEADER_BYTES);
    if (!h) { close(fd); return; }
    long n = pread(fd, h, HEADER_BYTES, 0);
    long base = n > 0 ? appsync_arm32_slice(h, n) : -1;
    if (base < 0) { free(h); close(fd); return; }
    if (base) {
        n = pread(fd, h, HEADER_BYTES, base);
    }
    unsigned sigoff = 0, siglen = 0;
    if (n >= 28 && be32(h) == 0xcefaedfe) {   // little-endian 32-bit Mach-O
        unsigned ncmds = *(unsigned *)(h + 16), off = 28;
        for (unsigned i = 0; i < ncmds && off + 16 <= (unsigned)n; i++) {
            unsigned *lc = (unsigned *)(h + off);
            if (lc[0] == 0x1d) { sigoff = lc[2]; siglen = lc[3]; break; }
            if (lc[1] < 8 || lc[1] > (unsigned)n - off) break;   /* no wrap past the header read */
            off += lc[1];
        }
    }
    unsigned char *b = siglen >= 12 && siglen < (1u << 24) ? malloc(siglen) : 0;
    if (b && pread(fd, b, siglen, base + sigoff) == (long)siglen && be32(b) == 0xfade0cc0) {
        unsigned count = be32(b + 8);
        for (unsigned i = 0; i < count && 12 + 8 * i + 8 <= siglen; i++) {
            unsigned o = be32(b + 12 + 8 * i + 4);
            if (siglen < 24 || o > siglen - 24) continue;   /* o + 24 could wrap */
            unsigned magic = be32(b + o), len = be32(b + o + 4);
            if (len < 8 || len > siglen - o) continue;   /* the entitlements' len - 8 must not wrap */
            if (magic == 0xfade0c02) {
                // CS_CodeDirectory.hashType is byte 36; 1 = SHA1, 2/3 = SHA256.
                if (!as_have_cdhash && o + 37 <= siglen) {
                    unsigned char digest[32];
                    unsigned char hashType = b[o + 36];
                    if (hashType == 1) {
                        CC_SHA1(b + o, len, digest);
                        as_have_cdhash = 1;
                    } else if (hashType == 2 || hashType == 3) {
                        CC_SHA256(b + o, len, digest);
                        as_have_cdhash = 1;
                    }
                    for (int z = 0; as_have_cdhash && z < 20; z++) as_cdhash[z] = digest[z];
                }
                if (!*ident) {
                    unsigned io = be32(b + o + 20);
                    if (io < len && memchr(b + o + io, 0, len - io))
                        *ident = CFStringCreateWithCString(kCFAllocatorDefault, (char *)b + o + io, 0x08000100);
                }
            } else if (magic == 0xfade7171 && !*ents) {
                CFDataRef x = CFDataCreate(kCFAllocatorDefault, b + o + 8, len - 8);
                CFTypeRef pl = x ? CFPropertyListCreateFromXMLData(kCFAllocatorDefault, x, 0, 0) : 0;
                if (x) CFRelease((CFTypeRef)x);
                if (pl && CFGetTypeID(pl) != CFDictionaryGetTypeID()) { CFRelease(pl); pl = 0; }
                *ents = pl;
            }
        }
    }
    free(b);
    free(h);
    close(fd);
}

static void signature_info(CFStringRef path, CFStringRef *ident, CFTypeRef *ents) {
    char file[1024];
    if (!path || !CFStringGetCString(path, file, sizeof file, 0x08000100)) return;
    if (!bundle_signature_info(file, ident, ents)) executable_signature_info(path, ident, ents);
}

// The original MISValidateSignatureAndCopyInfo, reached through the inline
// hook's trampoline (set in the constructor); the test leaves it 0 and the
// dlsym fallback below reaches its mock.
static int (*as_orig_mis)(void *, void *, CFDictionaryRef *);
extern const void *const kCFBooleanFalse;
extern void *CFNumberCreate(CFAllocatorRef, long, const void *);
extern CFTypeRef CFDateCreate(CFAllocatorRef, double);
extern double CFAbsoluteTimeGetCurrent(void);

// AppSync Unified's fakeInfo keys (literal strings installd reads, cdhash.m).
static CFStringRef as_key(const char *name) {
    return CFStringCreateWithCString(kCFAllocatorDefault, name, 0x08000100);
}

static int as_MISValidateSignatureAndCopyInfo(void *path, void *options,
                                             CFDictionaryRef *info) {
    // Copy APIs may leave the out parameter untouched on failure.
    if (info) *info = 0;
    int result;
    if (as_orig_mis) {
        result = as_orig_mis(path, options, info);
    } else {
        int (*real)(void *, void *, CFDictionaryRef *) =
            dlsym(RTLD_NEXT, "MISValidateSignatureAndCopyInfo");
        result = real ? real(path, options, info) : -1;
    }
    if (!in_installd()) return result;
    if (info && !*info) {
        CFMutableDictionaryRef d = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (!d) return -1;
        CFDataRef der = CFDataCreate(kCFAllocatorDefault,
                                    kAppSyncCertDER, (CFIndex)kAppSyncCertDERLen);
        if (!der) { CFRelease((CFTypeRef)d); return -1; }
        // AppSync Unified treats a fakesigned app as a profile-free App Store app.
        CFDictionarySetValue(d, kMISValidationInfoValidatedByProfile, kCFBooleanFalse);
        CFDictionarySetValue(d, kMISValidationInfoSignerCertificate, der);
        CFRelease((CFTypeRef)der);
        int signerType = 2, signatureVersion = 0x20100;  // App Store app
        CFTypeRef st = CFNumberCreate(kCFAllocatorDefault, 9, &signerType);          // kCFNumberIntType
        CFTypeRef sv = CFNumberCreate(kCFAllocatorDefault, 9, &signatureVersion);
        CFStringRef kst = as_key("SignerType"), ksv = as_key("SignatureVersion"), ktime = as_key("SigningTime");
        CFTypeRef now = CFDateCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent());
        if (st) { CFDictionarySetValue(d, kst, st); CFRelease(st); }
        if (sv) { CFDictionarySetValue(d, ksv, sv); CFRelease(sv); }
        if (now) { CFDictionarySetValue(d, ktime, now); CFRelease(now); }
        CFRelease((CFTypeRef)kst); CFRelease((CFTypeRef)ksv); CFRelease((CFTypeRef)ktime);
        as_have_cdhash = 0;
        const CFStringRef *ik = dlsym(RTLD_DEFAULT, "kMISValidationInfoSigningID");
        const CFStringRef *ek = dlsym(RTLD_DEFAULT, "kMISValidationInfoEntitlements");
        CFStringRef ident = 0;
        CFTypeRef ents = 0;
        signature_info((CFStringRef)path, &ident, &ents);  // also fills as_cdhash
        if (as_have_cdhash) {
            CFDataRef cd = CFDataCreate(kCFAllocatorDefault, as_cdhash, 20);
            CFStringRef kcd = as_key("CdHash");
            if (cd) { CFDictionarySetValue(d, kcd, cd); CFRelease((CFTypeRef)cd); }
            CFRelease((CFTypeRef)kcd);
        }
        if (ik || ek) {
            if (!ents) ents = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            if (ik && ident) CFDictionarySetValue(d, *ik, ident);
            if (ek && ents) CFDictionarySetValue(d, *ek, ents);
        }
        if (ident) CFRelease((CFTypeRef)ident);
        if (ents) CFRelease(ents);
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

// ---- inline hook of libmis for iOS 7+ --------------------------------------
// Ported from AppSync Unified (AppSyncUnified-installd.x, cdhash.m) by
// Karen/あけみ (akemi) and Linus Yang. On iOS 7+ the signature check runs inside
// the dyld shared cache: MobileSystemServices calls libmis for installd, past
// any interposition. So hook the libmis function body itself, the way AppSync
// Unified does with MSHookFunction, and every caller is intercepted. The 14G61
// kernel boots cs_enforcement_disable=1 (KBoot.defaultBootArgs), so a modified
// cache text page executes.
#ifndef APPSYNC_TEST
// Interposition still serves the 2.x-6.x direct-import path and amfid's own
// MISValidateSignature import; the cache byte-patch keeps amfid's exec working.
typedef struct { const void *replacement; const void *original; } interpose_t;
__attribute__((used)) static const interpose_t as_interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)as_MISValidateSignature,        (const void *)MISValidateSignature },
    { (const void *)as_SecCertificateCreateWithData,     (const void *)SecCertificateCreateWithData },
    { (const void *)as_SecCertificateCopySubjectSummary, (const void *)SecCertificateCopySubjectSummary },
};

extern void *mmap(void *, unsigned long, int, int, int, long);
extern int mprotect(void *, unsigned long, int);
extern void sys_icache_invalidate(void *, unsigned long);
extern void *dlopen(const char *, int);
extern void syslog(int, const char *, ...);
extern int *__error(void);

static int as_mprotect_rc, as_mprotect_errno;

// Make [addr, addr+len) writable+executable. The cache page is shared RX;
// cs_enforcement_disable lets the modified page run. Page-align the span.
static int as_make_rwx(void *addr, unsigned len) {
    unsigned long page = 4096;
    unsigned long start = (unsigned long)addr & ~(page - 1);
    unsigned long end = ((unsigned long)addr + len + page - 1) & ~(page - 1);
    as_mprotect_rc = mprotect((void *)start, end - start, 1 | 2 | 4);  // R|W|X
    as_mprotect_errno = as_mprotect_rc ? *__error() : 0;
    return as_mprotect_rc == 0;
}

// Thumb instruction length at p: 4 for a Thumb-32 (first halfword 0b111{01,10,11}xxxxx), else 2.
static unsigned as_thumb_len(const unsigned char *p) {
    unsigned short hw = *(const unsigned short *)p;
    return (hw & 0xf800) >= 0xe800 ? 4 : 2;
}

// Inline-hook a Thumb function: copy the displaced prologue into an RWX
// trampoline, append an absolute branch back, and write an absolute branch to
// `hook` at the entry. Returns a Thumb-callable pointer to the original, or 0.
static void *as_inline_hook(void *target, void *hook) {
    unsigned long addr = (unsigned long)target & ~1UL;
    unsigned char *fn = (unsigned char *)addr;
    // The `ldr.w pc,[pc,#0]` literal is read from Align(PC,4); a 4-byte-aligned
    // entry keeps the appended word where the instruction expects it.
    if (addr & 3) return 0;
    unsigned short first = *(unsigned short *)fn;
    // Only a push prologue (16-bit PUSH or 32-bit PUSH.W): its displaced bytes
    // are frame setup, safe to copy verbatim.
    if ((first & 0xfe00) != 0xb400 && first != 0xe92d) return 0;
    unsigned n = 0;
    while (n < 8) n += as_thumb_len(fn + n);   // whole instructions, >= 8 bytes
    unsigned char *tramp = mmap(0, n + 12, 1 | 2 | 4, 0x1000 | 0x0002, -1, 0);  // MAP_ANON|MAP_PRIVATE
    if (tramp == (void *)-1) return 0;
    for (unsigned i = 0; i < n; i++) tramp[i] = fn[i];
    unsigned t = n;
    if (t & 2) { tramp[t] = 0x00; tramp[t + 1] = 0xbf; t += 2; }         // nop, to 4-align the literal
    tramp[t+0]=0xdf; tramp[t+1]=0xf8; tramp[t+2]=0x00; tramp[t+3]=0xf0;  // ldr.w pc,[pc,#0]
    *(unsigned *)(tramp + t + 4) = (unsigned)(unsigned long)(addr + n) | 1;
    sys_icache_invalidate(tramp, t + 8);
    if (!as_make_rwx(fn, 8)) return 0;
    fn[0]=0xdf; fn[1]=0xf8; fn[2]=0x00; fn[3]=0xf0;                      // ldr.w pc,[pc,#0]
    *(unsigned *)(fn + 4) = (unsigned)(unsigned long)hook | 1;
    sys_icache_invalidate(fn, 8);
    return (void *)((unsigned long)tramp | 1);
}

__attribute__((constructor)) static void appsync_install_hooks(void) {
    if (!in_installd()) return;
    void *h = dlopen("/usr/lib/libmis.dylib", 0x10 | 0x2);  // RTLD_NOLOAD|RTLD_NOW
    void *target = h ? dlsym(h, "MISValidateSignatureAndCopyInfo") : 0;
    if (target) as_orig_mis = as_inline_hook(target, (void *)as_MISValidateSignatureAndCopyInfo);
    unsigned char *fn = target ? (unsigned char *)((unsigned long)target & ~1UL) : 0;
    syslog(3, "appsync: %s hook libmis MIS target=%p orig=%p mprotect=%d errno=%d prologue=%02x%02x%02x%02x",
           getprogname(), target, (void *)as_orig_mis, as_mprotect_rc, as_mprotect_errno,
           fn ? fn[0] : 0, fn ? fn[1] : 0, fn ? fn[2] : 0, fn ? fn[3] : 0);
}

#endif