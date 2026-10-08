/*
 * machotool mkold --legacy on a synthetic armv7 dylib: its rebases and data binds come out as the
 * classic relocations 2.x dyld slides and binds from (checked by applying them as its doRebase,
 * doBindIndirectSymbolPointers and external-relocation pass do), and an image whose rebases have no
 * classic form is refused rather than passed with them dropped.
 *
 *   mkold-test MACHOTOOL        (mkold-test.sh builds both and runs this; exit 0 = pass)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOCAL 0x80000000u
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)

static const char *tool;
static char dir[] = "/tmp/mkold-test.XXXXXX";

static uint32_t rd(const uint8_t *b, size_t o) { return b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (uint32_t)b[o + 3] << 24; }
static void wr(uint8_t *b, size_t o, uint32_t v) { for (int k = 0; k < 4; k++) b[o + k] = v >> 8 * k; }

/* a little emitter: words and fixed-width names */
static uint8_t cmds[1024];
static size_t ncmd;
static void w(uint32_t v) { wr(cmds, ncmd, v); ncmd += 4; }
static void name16(const char *s) { memset(cmds + ncmd, 0, 16); memcpy(cmds + ncmd, s, strlen(s)); ncmd += 16; }

typedef struct { const char *name; uint32_t addr, size, flags, r1; } Sect;

static void seg(const char *name, uint32_t vmaddr, uint32_t size, uint32_t fileoff, uint32_t prot, const Sect *s, int n)
{
    w(1), w(56 + 68 * n), name16(name), w(vmaddr), w(size), w(fileoff), w(size), w(prot), w(prot), w(n), w(0);
    for (int k = 0; k < n; k++)
        name16(s[k].name), name16(name), w(s[k].addr), w(s[k].size), w(fileoff + s[k].addr - vmaddr), w(2), w(0),
            w(0), w(s[k].flags), w(s[k].r1), w(0);
}

/* __TEXT 0x0 (a __text at 0x800), __DATA 0x1000: __nl_symbol_ptr (a local slot, _ext_func),
 * __data (two pointers into the image, _OBJC_CLASS_$_Foo, _ext_data + 8), __LINKEDIT 0x2000. */
static uint8_t *dylib(const uint8_t *rebase_ops, size_t nrebase, int text_rebase, size_t *len)
{
    static const char strtab[] = "\0_OBJC_CLASS_$_Foo\0_ext_data\0_ext_func";   /* undefined, sorted */
    const size_t nstr = sizeof strtab, stroffs[3] = { 1, 19, 29 };
    uint8_t symtab[36] = { 0 };
    for (int k = 0; k < 3; k++)
        wr(symtab, 12 * k, stroffs[k]), symtab[12 * k + 4] = 1, symtab[12 * k + 6] = 0x00, symtab[12 * k + 7] = 0x01;
    static const uint8_t binds[] = "\x11" "\x40_ext_func\0" "\x51" "\x71\x04" "\x90"
                                   "\x40_OBJC_CLASS_$_Foo\0" "\x71\x10" "\x90"
                                   "\x40_ext_data\0" "\x60\x08" "\x90" "\x00";
    const size_t nbind = sizeof binds - 1;
    uint8_t indirect[8];
    wr(indirect, 0, LOCAL), wr(indirect, 4, 2);
    uint32_t le = 0x2000, rebase_off = le, bind_off = rebase_off + nrebase;
    uint32_t sym_off = (bind_off + nbind + 3) & ~3u, ind_off = sym_off + 36, str_off = ind_off + 8, end = str_off + nstr;
    ncmd = 0;
    seg("__TEXT", 0, 0x1000, 0, 5, (Sect[]){ { "__text", 0x800, 0x10, 0x80000400, 0 } }, 1);
    seg("__DATA", 0x1000, 0x1000, 0x1000, 3, (Sect[]){ { "__nl_symbol_ptr", 0x1000, 8, 6, 0 }, { "__data", 0x1008, 0x10, 0, 0 } }, 2);
    w(1), w(56), name16("__LINKEDIT"), w(le), w(0x1000), w(le), w(end - le), w(1), w(1), w(0), w(0);
    w(0x80000022), w(48), w(rebase_off), w(nrebase), w(bind_off), w(nbind);
    for (int k = 0; k < 6; k++)
        w(0);
    w(2), w(24), w(sym_off), w(3), w(str_off), w(nstr);
    uint32_t dy[20] = { 0xB, 80, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, ind_off, 2, 0, 0, 0, 0 };
    for (int k = 0; k < 20; k++)
        w(dy[k]);
    uint8_t *b = calloc(1, end);
    uint32_t hdr[7] = { 0xFEEDFACE, 12, 9, 6, 6, (uint32_t)ncmd, 0x100085 };
    for (int k = 0; k < 7; k++)
        wr(b, 4 * k, hdr[k]);
    memcpy(b + 28, cmds, ncmd);
    wr(b, 0x1000, 0x1008), wr(b, 0x1004, 0);                          /* local NL slot -> __data; _ext_func */
    wr(b, 0x1008, 0x800), wr(b, 0x100c, 0x1010), wr(b, 0x1010, 0), wr(b, 0x1014, 0);   /* -> __text, -> __data; two binds */
    if (text_rebase)
        wr(b, 0x804, 0x1008);
    memcpy(b + rebase_off, rebase_ops, nrebase);
    memcpy(b + bind_off, binds, nbind);
    memcpy(b + sym_off, symtab, 36);
    memcpy(b + ind_off, indirect, 8);
    memcpy(b + str_off, strtab, nstr);
    *len = end;
    return b;
}

static char out_text[4096], err_text[4096];

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    CHECK(f, "%s", path);
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    char *p = malloc(n + 1);
    memcpy(p, buf, n);
    p[n] = 0;
    if (len)
        *len = n;
    return p;
}

/* Run mkold --legacy on the image; returns its exit status, the image it left in *out. */
static int mkold(const uint8_t *data, size_t len, uint8_t **out, size_t *outlen)
{
    char lib[256], so[256], se[256], cmd[1024];
    snprintf(lib, sizeof lib, "%s/lib.dylib", dir);
    snprintf(so, sizeof so, "%s/stdout", dir);
    snprintf(se, sizeof se, "%s/stderr", dir);
    FILE *f = fopen(lib, "wb");
    CHECK(f && fwrite(data, 1, len, f) == len && !fclose(f), "write %s", lib);
    snprintf(cmd, sizeof cmd, "'%s' mkold '%s' --legacy >'%s' 2>'%s'", tool, lib, so, se);
    int st = system(cmd);
    char *t = slurp(so, NULL);
    snprintf(out_text, sizeof out_text, "%s", t);
    free(t);
    t = slurp(se, NULL);
    snprintf(err_text, sizeof err_text, "%s", t);
    free(t);
    *out = (uint8_t *)slurp(lib, outlen);
    return st;
}

static uint8_t mem[0x2000];
static uint32_t word(uint32_t a) { return rd(mem, a); }
static void put(uint32_t a, uint32_t v) { wr(mem, a, v); }

/* Map the file as 2.x dyld would at `slide` and apply what it applies. */
static void load(const uint8_t *b, size_t len, uint32_t slide, uint32_t *nloc, uint32_t *next)
{
    const uint32_t symbols[3] = { 0x38000000, 0x39000000, 0x31000001 };
    CHECK(len >= 0x2000, "short image");
    memcpy(mem, b, 0x2000);
    uint32_t ncmds = rd(b, 16), off = 28, dysym[18] = { 0 };
    for (uint32_t k = 0; k < ncmds; k++) {
        uint32_t cmd = rd(b, off), size = rd(b, off + 4);
        CHECK(cmd != 0x80000022, "LC_DYLD_INFO_ONLY survived");
        if (cmd == 0xB)
            for (int j = 0; j < 18; j++)
                dysym[j] = rd(b, off + 8 + 4 * j);
        off += size;
    }
    uint32_t extreloff = dysym[14], nextrel = dysym[15], locreloff = dysym[16], nlocrel = dysym[17];
    for (uint32_t k = 0; k < nlocrel; k++) {                             /* doRebase */
        uint32_t addr = rd(b, locreloff + 8 * k), info = rd(b, locreloff + 8 * k + 4);
        CHECK((info & 0xFFFFFF) && ((info >> 25) & 3) == 2 && info >> 28 == 0 && !(info >> 27 & 1), "local info 0x%x", info);
        put(addr, word(addr) + slide);
    }
    put(0x1000, word(0x1000) + slide);                                    /* INDIRECT_SYMBOL_LOCAL */
    put(0x1004, symbols[2]);                                              /* the indirect table's bind */
    for (uint32_t k = 0; k < nextrel; k++) {                             /* external relocations */
        uint32_t addr = rd(b, extreloff + 8 * k), info = rd(b, extreloff + 8 * k + 4);
        CHECK((info >> 27 & 1) && ((info >> 25) & 3) == 2, "external info 0x%x", info);
        CHECK((info & 0xFFFFFF) < 3, "symbol %u", info & 0xFFFFFF);
        put(addr, word(addr) + symbols[info & 0xFFFFFF]);
    }
    *nloc = nlocrel, *next = nextrel;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2, "usage: mkold-test MACHOTOOL");
    tool = argv[1];
    CHECK(mkdtemp(dir), "mkdtemp");
    size_t len, outlen;
    uint8_t *img, *out;

    /* 0x1000 (the local NL slot), then 0x1008 and 0x100c */
    static const uint8_t rebases[] = "\x11" "\x21\x00" "\x51" "\x41" "\x52" "\x00";
    img = dylib(rebases, sizeof rebases - 1, 0, &len);
    int st = mkold(img, len, &out, &outlen);
    CHECK(st == 0, "%s", err_text);
    CHECK(strstr(out_text, "2 rebases -> local relocations, 2 binds -> external relocations"), "%s", out_text);
    uint32_t slide = 0x40000, nloc, next;
    load(out, outlen, slide, &nloc, &next);
    CHECK(nloc == 2 && next == 2, "%u local, %u external", nloc, next);
    const uint32_t want[][2] = { { 0x1000, 0x1008 + slide }, { 0x1004, 0x31000001 }, { 0x1008, 0x800 + slide },
                                 { 0x100c, 0x1010 + slide }, { 0x1010, 0x38000000 }, { 0x1014, 0x39000008 } };
    for (int k = 0; k < 6; k++)
        CHECK(word(want[k][0]) == want[k][1], "0x%x: 0x%x, want 0x%x", want[k][0], word(want[k][0]), want[k][1]);
    free(img), free(out);

    /* a rebase in __TEXT (a text relocation) and one of another type have no classic form: refused */
    static const uint8_t text[] = "\x11" "\x21\x00" "\x51" "\x41" "\x52" "\x20\x84\x10" "\x51" "\x00";
    img = dylib(text, sizeof text - 1, 1, &len);
    st = mkold(img, len, &out, &outlen);
    CHECK(st && strstr(err_text, "text relocation"), "%d %s", st, err_text);
    free(img), free(out);
    static const uint8_t type2[] = "\x12" "\x21\x08" "\x51" "\x00";
    img = dylib(type2, sizeof type2 - 1, 0, &len);
    st = mkold(img, len, &out, &outlen);
    CHECK(st && strstr(err_text, "rebase type 2"), "%d %s", st, err_text);
    free(img), free(out);

    char cmd[256];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    system(cmd);
    puts("PASS: rebases -> local relocations (local NL slot left to dyld), data binds -> external "
         "relocations with the addend in place; text and non-pointer rebases refused");
    return 0;
}
