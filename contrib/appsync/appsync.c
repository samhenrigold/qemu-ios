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
            if (magic == 0xfade0c02 && !*ident) {
                unsigned io = be32(b + o + 20);
                if (io < len && memchr(b + o + io, 0, len - io))
                    *ident = CFStringCreateWithCString(kCFAllocatorDefault, (char *)b + o + io, 0x08000100);
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
        const CFStringRef *ik = dlsym(RTLD_DEFAULT, "kMISValidationInfoSigningID");
        const CFStringRef *ek = dlsym(RTLD_DEFAULT, "kMISValidationInfoEntitlements");
        if (ik || ek) {
            CFStringRef ident = 0;
            CFTypeRef ents = 0;
            signature_info((CFStringRef)path, &ident, &ents);
            if (!ents) ents = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            if (ik && ident) CFDictionarySetValue(d, *ik, ident);
            if (ek && ents) CFDictionarySetValue(d, *ek, ents);
            if (ident) CFRelease((CFTypeRef)ident);
            if (ents) CFRelease(ents);
        }
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

// ---- fishhook-style rebinding for iOS 7+ -----------------------------------
// On iOS 7+ the signature check runs inside the dyld shared cache
// (MobileSystemServices for installd, amfid for exec), and those cache images
// call libmis through their own __got / __la_symbol_ptr slots, bound to the
// real libmis before this dylib loaded. Interposition reaches only an image's
// own imports, so it misses the cache-internal calls. Rewrite the bound
// pointers directly, the way fishhook does: walk each image's indirect symbol
// table and replace the slots whose undefined symbol names a hook. Addresses
// use unsigned long so the same code is exact on the ILP32 guest and the LP64
// host test.
struct as_mach_header { unsigned magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags; };
struct as_load_command { unsigned cmd, cmdsize; };
struct as_segment_command {
    unsigned cmd, cmdsize; char segname[16];
    unsigned vmaddr, vmsize, fileoff, filesize; int maxprot, initprot; unsigned nsects, flags;
};
struct as_section {
    char sectname[16], segname[16];
    unsigned addr, size, offset, align, reloff, nreloc, flags, reserved1, reserved2;
};
struct as_symtab_command { unsigned cmd, cmdsize, symoff, nsyms, stroff, strsize; };
struct as_dysymtab_command {
    unsigned cmd, cmdsize, ilocalsym, nlocalsym, iextdefsym, nextdefsym, iundefsym, nundefsym,
        tocoff, ntoc, modtaboff, nmodtab, extrefsymoff, nextrefsyms,
        indirectsymoff, nindirectsyms, extreloff, nextrel, locreloff, nlocrel;
};
struct as_nlist { unsigned n_strx; unsigned char n_type, n_sect; short n_desc; unsigned n_value; };
#define AS_LC_SEGMENT 0x1
#define AS_LC_SYMTAB 0x2
#define AS_LC_DYSYMTAB 0xb
#define AS_IS_PTR_SECTION(f) (((f) & 0xff) == 6 || ((f) & 0xff) == 7)  // NON_LAZY / LAZY
#define AS_INDIRECT_SKIP 0xc0000000u                                   // LOCAL | ABS

struct as_hook { const char *name; const void *fn; };

// Diagnostics for the one-time rebind: how many MIS pointers were seen in the
// cache images, how many were rewritten, how many could not be made writable.
static unsigned as_bind_seen, as_bind_done, as_bind_failed;

#ifdef APPSYNC_TEST
extern int as_make_writable(void *addr, unsigned len);  // the test owns the memory
#else
extern unsigned mach_task_self_;
extern int vm_protect(unsigned, unsigned long, unsigned long, int, int);
static int as_make_writable(void *addr, unsigned len) {
    // Cache __DATA_CONST slots are read-only after fixups; COPY (0x10) forces a
    // private writable page. READ|WRITE is 3.
    return vm_protect(mach_task_self_, (unsigned long)addr, len, 0, 3 | 0x10) == 0;
}
#endif

static void appsync_rebind_image(const struct as_mach_header *mh, long slide,
                                 const struct as_hook *hooks, unsigned nhooks) {
    if (!mh || mh->magic != 0xfeedface) return;  // 32-bit Mach-O only
    const struct as_symtab_command *st = 0;
    const struct as_dysymtab_command *dy = 0;
    unsigned long linkedit_base = 0;
    int have_linkedit = 0;
    const char *p = (const char *)mh + sizeof *mh;
    for (unsigned i = 0; i < mh->ncmds; i++) {
        const struct as_load_command *lc = (const struct as_load_command *)p;
        if (lc->cmdsize < sizeof *lc) return;
        if (lc->cmd == AS_LC_SEGMENT) {
            const struct as_segment_command *sg = (const struct as_segment_command *)p;
            if (!strcmp(sg->segname, "__LINKEDIT")) {
                linkedit_base = (unsigned long)slide + sg->vmaddr - sg->fileoff;
                have_linkedit = 1;
            }
        } else if (lc->cmd == AS_LC_SYMTAB) {
            st = (const struct as_symtab_command *)p;
        } else if (lc->cmd == AS_LC_DYSYMTAB) {
            dy = (const struct as_dysymtab_command *)p;
        }
        p += lc->cmdsize;
    }
    if (!st || !dy || !have_linkedit) return;
    const struct as_nlist *symtab = (const struct as_nlist *)(linkedit_base + st->symoff);
    const char *strtab = (const char *)(linkedit_base + st->stroff);
    const unsigned *indirect = (const unsigned *)(linkedit_base + dy->indirectsymoff);
    p = (const char *)mh + sizeof *mh;
    for (unsigned i = 0; i < mh->ncmds; i++) {
        const struct as_load_command *lc = (const struct as_load_command *)p;
        if (lc->cmd == AS_LC_SEGMENT) {
            const struct as_segment_command *sg = (const struct as_segment_command *)p;
            const struct as_section *sec = (const struct as_section *)(sg + 1);
            for (unsigned s = 0; s < sg->nsects; s++) {
                if (!AS_IS_PTR_SECTION(sec[s].flags)) continue;
                unsigned count = sec[s].size / 4;
                void **slots = (void **)((unsigned long)slide + sec[s].addr);
                const unsigned *isym = indirect + sec[s].reserved1;
                for (unsigned k = 0; k < count; k++) {
                    unsigned idx = isym[k];
                    if (idx & AS_INDIRECT_SKIP) continue;
                    if (idx >= st->nsyms) continue;
                    unsigned strx = symtab[idx].n_strx;
                    if (strx >= st->strsize) continue;
                    const char *name = strtab + strx;  // external names lead with '_'
                    for (unsigned h = 0; h < nhooks; h++) {
                        if (name[0] == '_' && !strcmp(name + 1, hooks[h].name)) {
                            as_bind_seen++;
                            if (slots[k] != hooks[h].fn) {
                                if (as_make_writable(&slots[k], sizeof slots[k])) {
                                    slots[k] = (void *)hooks[h].fn;
                                    as_bind_done++;
                                } else {
                                    as_bind_failed++;
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
        p += lc->cmdsize;
    }
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

extern unsigned _dyld_image_count(void);
extern const struct as_mach_header *_dyld_get_image_header(unsigned);
extern long _dyld_get_image_vmaddr_slide(unsigned);

static const struct as_hook as_hooks[] = {
    { "MISValidateSignature", (const void *)as_MISValidateSignature },
    { "MISValidateSignatureAndCopyInfo", (const void *)as_MISValidateSignatureAndCopyInfo },
    { "SecCertificateCreateWithData", (const void *)as_SecCertificateCreateWithData },
    { "SecCertificateCopySubjectSummary", (const void *)as_SecCertificateCopySubjectSummary },
};

// iOS 7+ (installd via MobileSystemServices, amfid on exec) reaches libmis only
// through cache-internal pointers interposition cannot touch; rebind them here.
extern void syslog(int, const char *, ...);
__attribute__((constructor)) static void appsync_install_hooks(void) {
    if (!in_installd()) return;
    unsigned n = _dyld_image_count();
    for (unsigned i = 0; i < n; i++)
        appsync_rebind_image(_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i),
                             as_hooks, sizeof as_hooks / sizeof *as_hooks);
    syslog(3, "appsync: %s MIS rebind seen=%u done=%u protect_failed=%u images=%u",
           getprogname(), as_bind_seen, as_bind_done, as_bind_failed, n);
}

#endif
