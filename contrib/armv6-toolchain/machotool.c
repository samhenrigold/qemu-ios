/*
 * machotool: the Mach-O rewrites armv6.sh (and hello-2007's 1.x link stubs) need.
 * Plain C11, libc only; armv6.sh builds it with the host compiler on first use.
 *
 *   machotool subtype FILE N
 *       Rewrite the cpusubtype of a thin 32-bit ARM Mach-O (cc6 presents armv6 objects as armv7).
 *
 *   machotool mkold FILE [--subtype N] [--legacy]
 *       Turn a modern thin armv7 Mach-O into something 2010 dyld will load. Strips the load
 *       commands that did not exist in 2010 (LC_VERSION_MIN_IPHONEOS, LC_SOURCE_VERSION,
 *       LC_ENCRYPTION_INFO, LC_DATA_IN_CODE, LC_FUNCTION_STARTS, LC_UUID, LC_BUILD_VERSION),
 *       rewrites LC_MAIN as the LC_UNIXTHREAD it would have been (pc at _main, no crt1, so main
 *       must not return) and sets cpusubtype (default 6). Removing commands only shrinks the
 *       header, so every file offset stays valid; the tail of the old load-command region is
 *       zero-filled so the padding before __text is untouched.
 *
 *       --legacy also drops LC_DYLD_INFO_ONLY, which 2.x dyld refuses (0x80000022), leaving the
 *       classic tables it binds and slides from. What the compressed info says is carried over
 *       into those tables, or the image is refused:
 *         binds     a non-lazy pointer the indirect symbol table names is bound by name already;
 *                   any other bind (ObjC metadata's superclass, a CFString's isa) becomes a
 *                   classic external relocation. No lazy or weak binds.
 *         rebases   (dylibs and bundles, which dyld slides) become classic local relocations,
 *                   section-relative-free: r_address from the first segment, r_symbolnum the
 *                   section, as 2.x dyld's doRebase reads them. A local non-lazy pointer
 *                   (INDIRECT_SYMBOL_LOCAL) is left out: dyld slides those itself
 *                   (doBindIndirectSymbolPointers), twice would be wrong. A rebase into a
 *                   read-only segment or of a type other than a pointer is refused.
 *         executables  must be non-PIE (link with -no_pie): dyld never slides them.
 *       An armv7 link against a 3.x+ target already routes calls through non-lazy pointers, so
 *       link with -no_pie and this checks and converts the rest.
 *
 *   machotool tbd ROOT INSTALL-PATH OUT.tbd
 *       Write a .tbd link stub for a dylib or framework taken from a guest's own root filesystem
 *       (e.g. tbd rootfs /System/Library/Frameworks/UIKit.framework/UIKit UIKit.tbd). iPhone OS 1.x
 *       had no SDK, so its frameworks themselves are the link target: the stub carries the binary's
 *       install name, versions and exported symbols (fragile-ABI classes as their
 *       `.objc_class_name_X` symbols), declared for armv7, the arch link6 links as. Modern ld/nm
 *       refuse the 1.x binaries outright (LC_PREBIND_CKSUM), so the symbol table is read here.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    LC_SEGMENT = 0x01, LC_SYMTAB = 0x02, LC_DYSYMTAB = 0x0B, LC_ID_DYLIB = 0x0D, LC_UUID = 0x1B,
    LC_CODE_SIGNATURE = 0x1D, LC_ENCRYPTION_INFO = 0x21, LC_VERSION_MIN_IPHONEOS = 0x25,
    LC_FUNCTION_STARTS = 0x26, LC_DATA_IN_CODE = 0x29, LC_SOURCE_VERSION = 0x2A, LC_BUILD_VERSION = 0x32,
};
#define LC_MAIN 0x80000028u
#define LC_DYLD_INFO_ONLY 0x80000022u
#define MH_PIE 0x200000u
#define MH_SPLIT_SEGS 0x20u
#define MH_DYLIB 6u
#define MH_BUNDLE 8u
#define S_NON_LAZY_SYMBOL_POINTERS 0x6u
#define INDIRECT_SYMBOL_LOCAL 0x80000000u

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

typedef struct { uint8_t *p; size_t n, cap; } Buf;

static void grow(Buf *b, size_t n)
{
    if (n <= b->cap)
        return;
    b->cap = n * 2 + 64;
    if (!(b->p = realloc(b->p, b->cap)))
        die("out of memory");
}

static void put(Buf *b, const void *d, size_t n)
{
    grow(b, b->n + n);
    if (n)
        memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void put32(Buf *b, uint32_t v)
{
    uint8_t x[4] = { v, v >> 8, v >> 16, v >> 24 };
    put(b, x, 4);
}

static void need(const Buf *b, uint64_t off, uint64_t len)
{
    if (off > b->n || len > b->n - off)
        die("truncated Mach-O: %llu bytes at offset 0x%llx are past its end",
            (unsigned long long)len, (unsigned long long)off);
}

static uint8_t rd8(const Buf *b, uint64_t off) { need(b, off, 1); return b->p[off]; }

static uint32_t rd32(const Buf *b, uint64_t off)
{
    need(b, off, 4);
    const uint8_t *p = b->p + off;
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(Buf *b, uint64_t off, uint32_t v)
{
    need(b, off, 4);
    for (int k = 0; k < 4; k++)
        b->p[off + k] = v >> 8 * k;
}

static Buf slurp(const char *path)
{
    Buf b = { 0 };
    FILE *f = fopen(path, "rb");
    if (!f)
        die("%s: cannot open", path);
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        put(&b, chunk, n);
    fclose(f);
    return b;
}

static void spit(const char *path, const Buf *b)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(b->p, 1, b->n, f) != b->n || fclose(f))
        die("%s: cannot write", path);
}

/* A NUL-terminated string in the file at off (the NUL must be there). */
static const char *cstr(const Buf *b, uint64_t off)
{
    need(b, off, 1);
    if (!memchr(b->p + off, 0, b->n - off))
        die("unterminated string at offset 0x%llx", (unsigned long long)off);
    return (const char *)b->p + off;
}

static uint64_t uleb(const Buf *b, uint64_t *i)
{
    uint64_t v = 0;
    unsigned shift = 0;
    for (;;) {
        uint8_t c = rd8(b, (*i)++);
        if (shift < 64)
            v |= (uint64_t)(c & 0x7F) << shift;
        if (shift < 64)
            shift += 7;
        if (c < 0x80)
            return v;
    }
}

static long arg_int(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (!*s || *end)
        die("not an integer: %s", s);
    return v;
}

/* ---- subtype ---- */

static int cmd_subtype(int argc, char **argv)
{
    if (argc != 2)
        die("usage: machotool subtype FILE N");
    const char *p = argv[0];
    int32_t n = (int32_t)arg_int(argv[1]);
    Buf b = slurp(p);
    uint32_t magic = rd32(&b, 0);
    if (magic != 0xFEEDFACE)
        die("not a thin 32-bit little-endian Mach-O: %08x", magic);
    int32_t cputype = (int32_t)rd32(&b, 4), old = (int32_t)rd32(&b, 8);
    if (cputype != 12)
        die("not CPU_TYPE_ARM: %d", cputype);
    wr32(&b, 8, (uint32_t)n);
    spit(p, &b);
    printf("%s: cpusubtype %d -> %d\n", p, old, n);
    return 0;
}

/* ---- mkold ---- */

typedef struct { uint64_t vmaddr, vmsize, fileoff, prot; } Seg;
typedef struct { uint64_t addr, size, type, r1; } Sect;
typedef struct { uint64_t where; const char *name; uint64_t addend; } Bind;
typedef struct { uint64_t off, word; } Patch;

/* ponytail: fixed tables; a real link6 output has 4 segments and a dozen sections. */
static Seg segs[256];
static int nseg;
static Sect sects[2048];
static int nsect;

static char why[512];
static Buf locrel, extrel;
static Patch *patches;
static size_t npatch;

static int refuse(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    return 1;
}

static const char *nm(const char *name) { return name ? name : "None"; }

static uint64_t seg_base(uint64_t seg)
{
    if (seg >= (uint64_t)nseg)
        die("segment index %llu out of range", (unsigned long long)seg);
    return segs[seg].vmaddr;
}

static int section_of(uint64_t where)
{
    for (int n = 0; n < nsect; n++)
        if (sects[n].addr <= where && where < sects[n].addr + sects[n].size)
            return n;
    return -1;
}

static int seg_fileoff(uint64_t where, uint64_t *fo)
{
    for (int k = 0; k < nseg; k++)
        if (segs[k].vmaddr <= where && where < segs[k].vmaddr + segs[k].vmsize) {
            *fo = segs[k].fileoff + where - segs[k].vmaddr;
            return 1;
        }
    return 0;
}

static int writable(uint64_t where)
{
    for (int k = 0; k < nseg; k++)
        if (segs[k].vmaddr <= where && where < segs[k].vmaddr + segs[k].vmsize && segs[k].prot & 2)
            return 1;
    return 0;
}

static void reloc(Buf *out, uint64_t where, uint32_t symbolnum, uint32_t ext)
{
    if (!nseg)
        die("no segments");
    put32(out, (uint32_t)(where - segs[0].vmaddr));
    put32(out, symbolnum | 2u << 25 | ext << 27);
}

static const char *symname(const Buf *b, uint32_t symoff, uint32_t stroff, uint64_t index)
{
    return cstr(b, (uint64_t)stroff + rd32(b, symoff + 12 * index));
}

/* What the classic tables need so LC_DYLD_INFO_ONLY can go (see --legacy). Returns 1 with `why`
 * set when it has no classic form; else fills locrel, extrel and patches. */
static int legacy_convert(const Buf *b, uint32_t ncmds, uint32_t flags, uint32_t filetype)
{
    uint32_t info[10], symtab[4], dysym[18];
    int have_info = 0, have_symtab = 0, have_dysym = 0;
    uint64_t off = 28;
    for (uint32_t c = 0; c < ncmds; c++) {
        uint32_t cmd = rd32(b, off), cmdsize = rd32(b, off + 4);
        if (cmd == LC_SEGMENT) {
            if (nseg == (int)(sizeof segs / sizeof *segs))
                die("too many segments");
            segs[nseg++] = (Seg){ rd32(b, off + 24), rd32(b, off + 28), rd32(b, off + 32), rd32(b, off + 44) };
            for (uint32_t k = 0, ns = rd32(b, off + 48); k < ns; k++) {
                uint64_t s = off + 56 + 68 * (uint64_t)k;
                if (nsect == (int)(sizeof sects / sizeof *sects))
                    die("too many sections");
                sects[nsect++] = (Sect){ rd32(b, s + 32), rd32(b, s + 36), rd32(b, s + 56) & 0xFF, rd32(b, s + 60) };
            }
        } else if (cmd == LC_DYLD_INFO_ONLY) {
            for (int k = 0; k < 10; k++)
                info[k] = rd32(b, off + 8 + 4 * k);
            have_info = 1;
        } else if (cmd == LC_SYMTAB) {
            for (int k = 0; k < 4; k++)
                symtab[k] = rd32(b, off + 8 + 4 * k);
            have_symtab = 1;
        } else if (cmd == LC_DYSYMTAB) {
            for (int k = 0; k < 18; k++)
                dysym[k] = rd32(b, off + 8 + 4 * k);
            have_dysym = 1;
        }
        off += cmdsize;
    }
    if (!have_info)
        return 0;
    uint32_t rebase_off = info[0], rebase_size = info[1], bind_off = info[2], bind_size = info[3];
    uint32_t weak_size = info[5], lazy_size = info[7];
    int slid = filetype == MH_DYLIB || filetype == MH_BUNDLE;
    if (flags & MH_PIE)
        return refuse("PIE (link with -no_pie)");
    if (flags & MH_SPLIT_SEGS)
        return refuse("split segments (relocations would be from the first writable segment)");
    if (weak_size || lazy_size)
        return refuse("has %s binds", lazy_size ? "lazy" : "weak");
    if (!have_symtab || !have_dysym)
        die("LC_DYLD_INFO_ONLY without LC_SYMTAB and LC_DYSYMTAB");
    if (dysym[15] || dysym[17])
        return refuse("already has classic relocations");
    uint32_t symoff = symtab[0], stroff = symtab[2];
    uint32_t indoff = dysym[12], nind = dysym[13], iundef = dysym[4], nundef = dysym[5];

    /* binds: (address, name, addend) */
    Bind *binds = NULL;
    size_t nbind = 0, capbind = 0;
#define BIND() do { \
        if (nbind == capbind && !(binds = realloc(binds, (capbind = capbind * 2 + 16) * sizeof *binds))) \
            die("out of memory"); \
        binds[nbind++] = (Bind){ seg_base(seg) + addr, name, addend }; \
    } while (0)
    uint64_t i = bind_off, end = (uint64_t)bind_off + bind_size, seg = 0, addr = 0, addend = 0;
    const char *name = NULL;
    while (i < end) {
        uint8_t op = rd8(b, i) & 0xF0, imm = rd8(b, i) & 0x0F;
        i++;
        if (op == 0x00) {
            break;
        } else if (op == 0x10 || op == 0x30 || op == 0x50) {
        } else if (op == 0x20) {
            uleb(b, &i);
        } else if (op == 0x40) {
            name = cstr(b, i);
            i += strlen(name) + 1;
        } else if (op == 0x60) {
            addend = uleb(b, &i);                       /* sleb, same byte framing */
            if (addend & (1ull << 31))
                return refuse("negative bind addend for %s", nm(name));
        } else if (op == 0x70) {
            seg = imm;
            addr = uleb(b, &i);
        } else if (op == 0x80) {
            addr = (addr + uleb(b, &i)) & 0xFFFFFFFF;
        } else if (op == 0x90) {
            BIND();
            addr += 4;
        } else if (op == 0xA0) {
            BIND();
            addr = (addr + uleb(b, &i) + 4) & 0xFFFFFFFF;
        } else if (op == 0xB0) {
            BIND();
            addr += imm * 4 + 4;
        } else if (op == 0xC0) {
            uint64_t count = uleb(b, &i), skip = uleb(b, &i);
            for (uint64_t k = 0; k < count; k++) {
                BIND();
                addr += skip + 4;
            }
        } else {
            return refuse("unknown bind opcode 0x%x", op);
        }
    }
#undef BIND
    for (size_t k = 0; k < nbind; k++) {
        uint64_t where = binds[k].where, fo;
        const char *bname = binds[k].name;
        int n = section_of(where);
        if (n >= 0 && sects[n].type == S_NON_LAZY_SYMBOL_POINTERS) {
            uint64_t slot = sects[n].r1 + (where - sects[n].addr) / 4;
            if (slot >= nind || !bname || strcmp(symname(b, symoff, stroff, rd32(b, indoff + 4 * slot)), bname))
                return refuse("bind of %s at 0x%llx is not in the indirect symbol table", nm(bname),
                              (unsigned long long)where);
            if (binds[k].addend)
                return refuse("bind of %s at 0x%llx through a non-lazy pointer has an addend", nm(bname),
                              (unsigned long long)where);
            continue;
        }
        /* classic dyld adds the symbol to the word in place: that word is the addend.
         * The undefined symbol's index: the last of that name, as a name->index map keeps it. */
        int64_t sym = -1;
        for (uint64_t u = iundef; bname && u < (uint64_t)iundef + nundef; u++)
            if (!strcmp(symname(b, symoff, stroff, u), bname))
                sym = (int64_t)u;
        if (sym < 0 || !writable(where) || !seg_fileoff(where, &fo))
            return refuse("bind of %s at 0x%llx has no classic form", nm(bname), (unsigned long long)where);
        if (binds[k].addend) {
            if (!(patches = realloc(patches, (npatch + 1) * sizeof *patches)))
                die("out of memory");
            patches[npatch++] = (Patch){ fo, binds[k].addend };
        }
        reloc(&extrel, where, (uint32_t)sym, 1);
    }
    free(binds);

    /* rebases: addresses */
    uint64_t *rebases = NULL;
    size_t nrebase = 0, caprebase = 0;
    uint64_t kind = 1;
    i = rebase_off, end = (uint64_t)rebase_off + rebase_size, seg = 0, addr = 0;
    while (i < end) {
        uint8_t op = rd8(b, i) & 0xF0, imm = rd8(b, i) & 0x0F;
        i++;
        if (op == 0x00) {
            break;
        } else if (op == 0x10) {
            kind = imm;
        } else if (op == 0x20) {
            seg = imm;
            addr = uleb(b, &i);
        } else if (op == 0x30) {
            addr = (addr + uleb(b, &i)) & 0xFFFFFFFF;
        } else if (op == 0x40) {
            addr += imm * 4;
        } else if (op >= 0x50 && op <= 0x80) {
            uint64_t count = imm, skip = 0;
            if (op != 0x50)
                count = uleb(b, &i);
            if (op == 0x70)
                skip = count, count = 1;
            else if (op == 0x80)
                skip = uleb(b, &i);
            for (uint64_t k = 0; k < count; k++) {
                if (kind != 1)
                    return refuse("rebase type %d at 0x%llx (only pointers have a classic form)", (int)kind,
                                  (unsigned long long)(seg_base(seg) + addr));
                if (nrebase == caprebase && !(rebases = realloc(rebases, (caprebase = caprebase * 2 + 16) * sizeof *rebases)))
                    die("out of memory");
                rebases[nrebase++] = seg_base(seg) + addr;
                addr += 4 + skip;
            }
        } else {
            return refuse("unknown rebase opcode 0x%x", op);
        }
    }
    if (!slid)
        nrebase = 0;                                    /* an executable is never slid */
    for (size_t k = 0; k < nrebase; k++) {
        uint64_t where = rebases[k];
        int local = 0;                                  /* dyld slides INDIRECT_SYMBOL_LOCAL pointers itself */
        for (int s = 0; s < nsect && !local; s++)
            if (sects[s].type == S_NON_LAZY_SYMBOL_POINTERS && sects[s].addr <= where &&
                where < sects[s].addr + sects[s].size / 4 * 4 && (where - sects[s].addr) % 4 == 0 &&
                rd32(b, indoff + 4 * (sects[s].r1 + (where - sects[s].addr) / 4)) == INDIRECT_SYMBOL_LOCAL)
                local = 1;
        if (local)
            continue;
        int n = section_of(where);
        if (n < 0 || !writable(where))
            return refuse("rebase at 0x%llx is not in a writable section (a text relocation)", (unsigned long long)where);
        reloc(&locrel, where, (uint32_t)n + 1, 0);
    }
    free(rebases);
    return 0;
}

static uint32_t dysym_get(const Buf *b, uint64_t at, int k) { return rd32(b, at + 8 + 4 * k); }

/* Rebuild __LINKEDIT as the classic tables in ld's classic order (relocations, symbols, indirect
 * symbols, strings: signing tools want the strings last) and point LC_SYMTAB/LC_DYSYMTAB at them.
 * The compressed info, and a stale code signature (signing redoes it), are left out. */
static void add_relocations(Buf *b, uint32_t ncmds, char *note, size_t notelen)
{
    note[0] = 0;
    for (size_t k = 0; k < npatch; k++)
        wr32(b, patches[k].off, (uint32_t)(rd32(b, patches[k].off) + patches[k].word));
    if (!locrel.n && !extrel.n)
        return;
    uint64_t linkedit = 0, symtab = 0, dysym = 0, off = 28;
    for (uint32_t c = 0; c < ncmds; c++) {
        uint32_t cmd = rd32(b, off), cmdsize = rd32(b, off + 4);
        need(b, off + 8, 11);
        if (cmd == LC_SEGMENT && !memcmp(b->p + off + 8, "__LINKEDIT", 11))
            linkedit = off;
        else if (cmd == LC_SYMTAB)
            symtab = off;
        else if (cmd == LC_DYSYMTAB)
            dysym = off;
        else if (cmd == 0x22 || cmd == 0x2B || cmd == 0x2E || cmd == 0x80000033 || cmd == 0x80000034)
            /* DYLD_INFO, DYLIB_CODE_SIGN_DRS, LINKER_OPTIMIZATION_HINT, EXPORTS_TRIE, CHAINED_FIXUPS */
            die("load command 0x%x keeps data in __LINKEDIT; cannot rebuild it", cmd);
        off += cmdsize;
    }
    if (!linkedit || !symtab || !dysym)
        die("no __LINKEDIT, LC_SYMTAB or LC_DYSYMTAB; cannot add relocations");
    uint32_t vmaddr = rd32(b, linkedit + 24), vmsize = rd32(b, linkedit + 28);
    uint32_t fileoff = rd32(b, linkedit + 32), filesize = rd32(b, linkedit + 36);
    if ((uint64_t)fileoff + filesize != b->n)
        die("__LINKEDIT does not end the file; cannot add relocations");
    uint32_t symoff = rd32(b, symtab + 8), nsyms = rd32(b, symtab + 12);
    uint32_t stroff = rd32(b, symtab + 16), strsize = rd32(b, symtab + 20);
    uint32_t d[18];
    for (int k = 0; k < 18; k++)
        d[k] = dysym_get(b, dysym, k);
    if (d[6] || d[8] || d[10])
        die("a table of contents, module table or reference table; cannot rebuild __LINKEDIT");
    Buf le = { 0 };
    /* the tables as slices of the file (clamped at its end) */
#define SLICE(from, len) do { uint64_t f_ = (from), l_ = (len); \
        if (f_ < b->n) put(&le, b->p + f_, l_ < b->n - f_ ? l_ : b->n - f_); } while (0)
    put(&le, extrel.p, extrel.n);
    put(&le, locrel.p, locrel.n);
    d[14] = extrel.n ? fileoff : 0;
    d[15] = (uint32_t)(extrel.n / 8);
    d[16] = locrel.n ? fileoff + (uint32_t)extrel.n : 0;
    d[17] = (uint32_t)(locrel.n / 8);
    uint32_t nsymoff = fileoff + (uint32_t)le.n;
    SLICE(symoff, 12 * (uint64_t)nsyms);
    uint32_t indoff = d[12], nind = d[13];
    d[12] = nind ? fileoff + (uint32_t)le.n : 0;
    SLICE(indoff, 4 * (uint64_t)nind);
    uint32_t nstroff = fileoff + (uint32_t)le.n;
    SLICE(stroff, strsize);
#undef SLICE
    b->n = fileoff;
    put(b, le.p, le.n);
    uint32_t lesize = (uint32_t)le.n, rounded = (lesize + 0xFFF) & ~0xFFFu;
    wr32(b, linkedit + 24, vmaddr);
    wr32(b, linkedit + 28, vmsize > rounded ? vmsize : rounded);
    wr32(b, linkedit + 32, fileoff);
    wr32(b, linkedit + 36, lesize);
    wr32(b, symtab + 8, nsymoff);
    wr32(b, symtab + 12, nsyms);
    wr32(b, symtab + 16, nstroff);
    wr32(b, symtab + 20, strsize);
    for (int k = 0; k < 18; k++)
        wr32(b, dysym + 8 + 4 * k, d[k]);
    free(le.p);
    snprintf(note, notelen, " (%d rebases -> local relocations, %d binds -> external relocations)",
             (int)(locrel.n / 8), (int)(extrel.n / 8));
}

static const char *dropped_name(uint32_t cmd)
{
    switch (cmd) {
    case LC_UUID: return "LC_UUID";
    case LC_VERSION_MIN_IPHONEOS: return "LC_VERSION_MIN_IPHONEOS";
    case LC_FUNCTION_STARTS: return "LC_FUNCTION_STARTS";
    case LC_DATA_IN_CODE: return "LC_DATA_IN_CODE";
    case LC_SOURCE_VERSION: return "LC_SOURCE_VERSION";
    case LC_ENCRYPTION_INFO: return "LC_ENCRYPTION_INFO";
    case LC_BUILD_VERSION: return "LC_BUILD_VERSION";
    }
    return NULL;
}

static int cmd_mkold(int argc, char **argv)
{
    if (argc < 1)
        die("usage: machotool mkold FILE [--subtype N] [--legacy]");
    const char *path = argv[0];
    int32_t subtype = 6;
    int legacy = 0;
    for (int k = 0; k < argc; k++) {
        if (!strcmp(argv[k], "--subtype")) {
            if (k + 1 >= argc)
                die("--subtype needs a value");
            subtype = (int32_t)arg_int(argv[k + 1]);
            break;                                      /* the first --subtype counts */
        }
    }
    for (int k = 0; k < argc; k++)
        legacy |= !strcmp(argv[k], "--legacy");

    Buf b = slurp(path);
    uint32_t magic = b.n >= 4 ? rd32(&b, 0) : 0;
    if (magic != 0xFEEDFACE)
        die("not a thin 32-bit LE Mach-O: %08x (run lipo -thin first)", magic);
    int32_t cputype = (int32_t)rd32(&b, 4), oldsub = (int32_t)rd32(&b, 8);
    uint32_t filetype = rd32(&b, 12), ncmds = rd32(&b, 16), sizeofcmds = rd32(&b, 20), flags = rd32(&b, 24);

    /* __TEXT vmaddr, needed to turn LC_MAIN's entryoff into an absolute pc. */
    uint32_t text_vmaddr = 0;
    uint64_t off = 28;
    for (uint32_t c = 0; c < ncmds; c++) {
        uint32_t cmd = rd32(&b, off), cmdsize = rd32(&b, off + 4);
        need(&b, off + 8, 6);
        if (cmd == LC_SEGMENT && !memcmp(b.p + off + 8, "__TEXT", 6))
            text_vmaddr = rd32(&b, off + 24);
        off += cmdsize;
    }

    char relocated[160] = "";
    if (legacy) {
        if (legacy_convert(&b, ncmds, flags, filetype))
            die("%s: cannot link for 2.x dyld: %s", path, why);
        add_relocations(&b, ncmds, relocated, sizeof relocated);
    }

    Buf kept = { 0 }, dropped = { 0 };
    uint32_t nkept = 0;
    char item[256];
    off = 28;
    for (uint32_t c = 0; c < ncmds; c++) {
        uint32_t cmd = rd32(&b, off), cmdsize = rd32(&b, off + 4);
        const char *dname = NULL;
        need(&b, off, cmdsize);
        if (cmd == LC_MAIN) {
            /* 2010 dyld predates LC_MAIN (iOS 6) and refuses the image outright. Rebuild it as the
             * LC_UNIXTHREAD it would have been in 2010, with pc pointing straight at _main. There is
             * no crt1 in the way, so _main must never return -- see tester.c, which calls _exit(). */
            uint64_t entryoff = rd32(&b, off + 8) | (uint64_t)rd32(&b, off + 12) << 32;
            uint32_t pc = (uint32_t)(text_vmaddr + entryoff);
            put32(&kept, 0x5), put32(&kept, 84), put32(&kept, 1), put32(&kept, 17);
            for (int r = 0; r < 17; r++)
                put32(&kept, r == 15 ? pc : 0);
            nkept++;
            snprintf(item, sizeof item, "LC_MAIN->LC_UNIXTHREAD(pc=0x%x)", pc);
            dname = item;
        } else if (legacy && cmd == LC_DYLD_INFO_ONLY) {
            snprintf(item, sizeof item, "LC_DYLD_INFO_ONLY%s", relocated);
            dname = item;
        } else if (legacy && cmd == LC_CODE_SIGNATURE) {
            dname = "LC_CODE_SIGNATURE (stale: sign after this)";
        } else if (!(dname = dropped_name(cmd))) {
            put(&kept, b.p + off, cmdsize);
            nkept++;
        }
        if (dname) {
            if (dropped.n)
                put(&dropped, ", ", 2);
            put(&dropped, dname, strlen(dname));
        }
        off += cmdsize;
    }
    put(&dropped, "", 1);

    /* Zero the whole old load-command region, then lay the kept commands back down. */
    uint64_t span = 28 + (uint64_t)(sizeofcmds > kept.n ? sizeofcmds : kept.n);
    if (span > b.n) {
        grow(&b, span);
        memset(b.p + b.n, 0, span - b.n);
        b.n = span;
    }
    memset(b.p + 28, 0, sizeofcmds);
    memcpy(b.p + 28, kept.p, kept.n);
    wr32(&b, 4, (uint32_t)cputype);
    wr32(&b, 8, (uint32_t)subtype);
    wr32(&b, 12, filetype);
    wr32(&b, 16, nkept);
    wr32(&b, 20, (uint32_t)kept.n);
    wr32(&b, 24, flags);
    spit(path, &b);
    printf("%s: cpusubtype %d -> %d; ncmds %u -> %u; dropped %s\n", path, oldsub, subtype, ncmds, nkept,
           dropped.n > 1 ? (char *)dropped.p : "(none)");
    return 0;
}

/* ---- tbd ---- */

static int by_bytes(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Python wrote these as Latin-1 decoded text to a UTF-8 file: bytes >= 0x80 become two bytes. */
static void latin1_utf8(FILE *f, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x80)
            fputc(*p, f);
        else
            fputc(0xC0 | *p >> 6, f), fputc(0x80 | (*p & 0x3F), f);
}

static void version(FILE *f, const char *key, uint32_t v)
{
    fprintf(f, "%s: %u.%u.%u\n", key, v >> 16, (v >> 8) & 0xFF, v & 0xFF);
}

static int cmd_tbd(int argc, char **argv)
{
    if (argc != 3)
        die("usage: machotool tbd ROOT INSTALL-PATH OUT.tbd");
    const char *root = argv[0], *install = argv[1], *out = argv[2];
    while (*install == '/')
        install++;
    size_t rl = strlen(root);
    size_t pl = rl + strlen(install) + 2;
    char *path = malloc(pl);
    if (!path)
        die("out of memory");
    snprintf(path, pl, "%s%s%s", root, rl && root[rl - 1] != '/' ? "/" : "", install);

    Buf whole = slurp(path), b = whole;
    if (b.n >= 8 && !memcmp(b.p, "\xCA\xFE\xBA\xBE", 4)) {   /* fat: the first arm slice */
        uint32_t n = (uint32_t)b.p[4] << 24 | b.p[5] << 16 | b.p[6] << 8 | b.p[7];
        for (uint32_t k = 0; k < n; k++) {
            need(&whole, 8 + 20 * (uint64_t)k, 20);
            const uint8_t *e = whole.p + 8 + 20 * k;
            uint32_t cpu = (uint32_t)e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3];
            uint32_t so = (uint32_t)e[8] << 24 | e[9] << 16 | e[10] << 8 | e[11];
            uint32_t ss = (uint32_t)e[12] << 24 | e[13] << 16 | e[14] << 8 | e[15];
            if (cpu == 12) {
                need(&whole, so, ss);
                b = (Buf){ whole.p + so, ss, ss };
                break;
            }
        }
    }
    if (b.n < 28 || rd32(&b, 0) != 0xFEEDFACE || rd32(&b, 4) != 12)
        die("%s: not a 32-bit ARM Mach-O", path);
    uint32_t ncmds = rd32(&b, 16), cur = 0, compat = 0;
    const char *name = NULL;
    char **syms = NULL;
    size_t nsyms = 0, cap = 0;
    uint64_t off = 28;
    for (uint32_t c = 0; c < ncmds; c++) {
        uint32_t cmd = rd32(&b, off), size = rd32(&b, off + 4);
        if (cmd == LC_ID_DYLIB) {
            uint32_t name_off = rd32(&b, off + 8);
            cur = rd32(&b, off + 16), compat = rd32(&b, off + 20);
            need(&b, off, size);
            uint64_t from = off + name_off, to = off + size;
            if (from > to)
                from = to;
            const uint8_t *nul = memchr(b.p + from, 0, to - from);
            size_t len = nul ? (size_t)(nul - (b.p + from)) : (size_t)(to - from);
            char *s = malloc(len + 1);
            if (!s)
                die("out of memory");
            memcpy(s, b.p + from, len);
            s[len] = 0;
            name = s;
        } else if (cmd == LC_SYMTAB) {
            uint32_t symoff = rd32(&b, off + 8), n = rd32(&b, off + 12), stroff = rd32(&b, off + 16);
            for (uint32_t k = 0; k < n; k++) {
                uint64_t e = symoff + 12 * (uint64_t)k;
                uint8_t typ = rd8(&b, e + 4);
                if (typ & 0x01 && ((typ & 0x0E) == 0x0E || (typ & 0x0E) == 0x02)) {  /* N_EXT, N_SECT|N_ABS */
                    if (nsyms == cap && !(syms = realloc(syms, (cap = cap * 2 + 256) * sizeof *syms)))
                        die("out of memory");
                    syms[nsyms++] = (char *)cstr(&b, (uint64_t)stroff + rd32(&b, e));
                }
            }
        }
        off += size;
    }
    if (!name)
        die("%s: no LC_ID_DYLIB", path);
    if (nsyms)
        qsort(syms, nsyms, sizeof *syms, by_bytes);
    size_t uniq = 0;
    for (size_t k = 0; k < nsyms; k++)
        if (!uniq || strcmp(syms[uniq - 1], syms[k]))
            syms[uniq++] = syms[k];

    FILE *f = fopen(out, "w");
    if (!f)
        die("%s: cannot write", out);
    fputs("--- !tapi-tbd\ntbd-version: 4\ntargets: [ armv7-ios ]\ninstall-name: '", f);
    fputs(name, f);
    fputs("'\n", f);
    version(f, "current-version", cur);
    version(f, "compatibility-version", compat);
    fputs("exports:\n  - targets: [ armv7-ios ]\n    symbols: [ ", f);
    for (size_t k = 0; k < uniq; k++) {
        if (k)
            fputs(",\n                 ", f);
        fputc('\'', f);
        latin1_utf8(f, syms[k]);
        fputc('\'', f);
    }
    fputs(" ]\n...\n", f);
    if (fclose(f))
        die("%s: cannot write", out);
    printf("%s: %zu symbols\n", out, uniq);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "subtype"))
        return cmd_subtype(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "mkold"))
        return cmd_mkold(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "tbd"))
        return cmd_tbd(argc - 2, argv + 2);
    die("usage: machotool subtype FILE N\n"
        "       machotool mkold FILE [--subtype N] [--legacy]\n"
        "       machotool tbd ROOT INSTALL-PATH OUT.tbd");
}
