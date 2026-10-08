/*
 * The dispatch table, discovered at load. Included by mbxshim.c (so by glishim.c too) after
 * qc(), w(), refused() and the hand-written thunks are declared.
 *
 * OpenGLES's gl* trampolines index a table of function pointers by a slot number fixed by the
 * firmware's __GLIFunctionDispatchRec: 822 slots on 3.1.3, 826 on 3.2 (three inserted at 761),
 * 841 on 4.2.1. The shim used to be built once per layout from a table derived offline; now one
 * binary per architecture reads the layout out of the running firmware:
 *
 *   (a) the ObjC @encode of _EAGLContextPrivate that OpenGLES carries names every field of the
 *       dispatch record, in slot order, e.g. {__GLIFunctionDispatchRec="accum"^?"alpha_func"^?...}.
 *       It sits in OpenGLES's __TEXT (__cstring on 3.x, __objc_methtype on 4.x), so it is read
 *       straight out of the image dyld already mapped. Present in every 3.x and 4.x firmware seen;
 *       2.x has no such string (and no shim).
 *   (b) failing that, OpenGLES's own exported trampolines: each _gl* export loads its target with
 *       one `ldr rX, [ctx, #0x10 + 4*slot]`, which contrib/gles-public/glitsv.py decodes offline and
 *       this file decodes in place for every name the table marks GLES_F_EXPORT.
 *
 * Either way each slot is matched by name to a row of include/hw/arm/guest-services/gles-names.h,
 * the table the host is built from too, and takes that row's wire id: a hand-written thunk if
 * mbxshim has one for the id, else the generated forwarder, else a by-name stub. A field the table
 * does not know gets a by-slot stub. So a firmware with a different layout needs no new build, and
 * the wire carries function ids, never a firmware's slot numbers.
 */

#define NA -1
#define GLES_FN(n, f, id, argc, fl) { #n, #f, id, argc, fl },
typedef struct { const char *name, *field; unsigned short id; signed char argc; unsigned char flags; } gles_fn_t;
static const gles_fn_t gles_fns[] = {
#include "../../include/hw/arm/guest-services/gles-names.h"
};
#undef GLES_FN
#undef NA
#define GLES_N_FNS (sizeof gles_fns / sizeof gles_fns[0])

/* GLES_ID_glClear and friends: the wire ids by name. */
#define GLES_FN(n, f, id, argc, fl) GLES_ID_##n = id,
enum {
#include "../../include/hw/arm/guest-services/gles-names.h"
};
#undef GLES_FN

/* A by-name stub: the table knows the function but cannot forward it (no argc). */
static int gles_stub(const char *name);

/* The forwarders: one per row, sending argc 32-bit words under the row's id. Arguments are
 * declared `unsigned` even where GL says `float`: soft-float AAPCS delivers floats as bit
 * patterns in the core registers, which is what the host wants. */
#define GLES_FWD_NA(n, id) static int fn_##n(void *gc) { (void)gc; return gles_stub(#n); }
#define GLES_FWD_0(n, id) static int fn_##n(void *gc) { return (int)qc(id, gc, 0, A(0)); }
#define GLES_FWD_1(n, id) static int fn_##n(void *gc, unsigned a0) { return (int)qc(id, gc, 1, A(a0)); }
#define GLES_FWD_2(n, id) static int fn_##n(void *gc, unsigned a0, unsigned a1) { return (int)qc(id, gc, 2, A(a0, a1)); }
#define GLES_FWD_3(n, id) static int fn_##n(void *gc, unsigned a0, unsigned a1, unsigned a2) { return (int)qc(id, gc, 3, A(a0, a1, a2)); }
#define GLES_FWD_4(n, id) static int fn_##n(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3) { return (int)qc(id, gc, 4, A(a0, a1, a2, a3)); }
#define GLES_FWD_5(n, id) static int fn_##n(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4) { return (int)qc(id, gc, 5, A(a0, a1, a2, a3, a4)); }
#define GLES_FWD_6(n, id) static int fn_##n(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4, unsigned a5) { return (int)qc(id, gc, 6, A(a0, a1, a2, a3, a4, a5)); }
#define GLES_FWD_7(n, id) static int fn_##n(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4, unsigned a5, unsigned a6) { return (int)qc(id, gc, 7, A(a0, a1, a2, a3, a4, a5, a6)); }
#define GLES_FWD_8(n, id) static int fn_##n(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4, unsigned a5, unsigned a6, unsigned a7) { return (int)qc(id, gc, 8, A(a0, a1, a2, a3, a4, a5, a6, a7)); }
#define GLES_FWD_9(n, id) static int fn_##n(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4, unsigned a5, unsigned a6, unsigned a7, unsigned a8) { return (int)qc(id, gc, 9, A(a0, a1, a2, a3, a4, a5, a6, a7, a8)); }
#define GLES_FWD_10(n, id) static int fn_##n(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4, unsigned a5, unsigned a6, unsigned a7, unsigned a8, unsigned a9) { return (int)qc(id, gc, 10, A(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9)); }
#define GLES_FWD_11(n, id) static int fn_##n(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4, unsigned a5, unsigned a6, unsigned a7, unsigned a8, unsigned a9, unsigned a10) { return (int)qc(id, gc, 11, A(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10)); }
#define GLES_FWD_12(n, id) static int fn_##n(void *gc, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4, unsigned a5, unsigned a6, unsigned a7, unsigned a8, unsigned a9, unsigned a10, unsigned a11) { return (int)qc(id, gc, 12, A(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11)); }
#define GLES_FN(n, f, id, argc, fl) GLES_FWD_##argc(n, id)
#include "../../include/hw/arm/guest-services/gles-names.h"
#undef GLES_FN
#define GLES_FN(n, f, id, argc, fl) (void *)fn_##n,
static void *const gles_fn_ptr[] = {        /* by row, as gles_fns */
#include "../../include/hw/arm/guest-services/gles-names.h"
};
#undef GLES_FN

/* By wire id: calls glishim may queue (see gles_batch). */
#define GLES_FN(n, f, id, argc, fl) [id] = ((fl) & GLES_F_BATCH) != 0,
static const unsigned char gles_batchable[GLES_ID_MAX + 1] = {
#include "../../include/hw/arm/guest-services/gles-names.h"
};
#undef GLES_FN

/* This firmware's layout, discovered once per process. */
static struct {
    unsigned n;                          /* slots; 0 = not discovered yet */
    const char *how;                     /* "encode", "exports" or "none" */
    short fn[GLES_MAX_SLOTS];            /* row of gles_fns per slot, -1 = a field the table lacks */
    const char *field[GLES_MAX_SLOTS];   /* the firmware's field name per slot (copied), 0 = none */
    short slot[GLES_ID_MAX + 1];         /* slot per wire id, -1 = this firmware has no such slot */
    char names[24 * 1024];               /* the copied field names */
    unsigned names_used;
} gli;

static int gles_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* The row whose dispatch field is name[0..len), or -1. */
static int gles_row_of_field(const char *name, unsigned len)
{
    unsigned i;
    for (i = 0; i < GLES_N_FNS; i++) {
        const char *f = gles_fns[i].field;
        unsigned k = 0;
        while (k < len && f[k] && f[k] == name[k]) k++;
        if (k == len && !f[k]) return (int)i;
    }
    return -1;
}

static int gles_row_of_name(const char *name)
{
    unsigned i;
    for (i = 0; i < GLES_N_FNS; i++)
        if (gles_streq(gles_fns[i].name, name)) return (int)i;
    return -1;
}

static void gles_layout_reset(void)
{
    unsigned i;
    gli.n = 0;
    gli.how = "none";
    gli.names_used = 0;
    for (i = 0; i < GLES_MAX_SLOTS; i++) { gli.fn[i] = -1; gli.field[i] = 0; }
    for (i = 0; i <= GLES_ID_MAX; i++) gli.slot[i] = -1;
}

/* Slot i is the field name[0..len): remember it and its row. */
static void gles_layout_set(unsigned i, const char *name, unsigned len)
{
    if (i >= GLES_MAX_SLOTS) return;
    if (gli.names_used + len + 1 <= sizeof gli.names) {
        char *d = gli.names + gli.names_used;
        unsigned k;
        for (k = 0; k < len; k++) d[k] = name[k];
        d[len] = 0;
        gli.field[i] = d;
        gli.names_used += len + 1;
    }
    gli.fn[i] = (short)gles_row_of_field(name, len);
    if (gli.fn[i] >= 0) gli.slot[gles_fns[gli.fn[i]].id] = (short)i;
    if (i + 1 > gli.n) gli.n = i + 1;
}

/* ---- (a) the @encode ------------------------------------------------------------------ */

static unsigned gles_encode_unnamed;        /* fields of a name-less @encode (6.x/7.x), else 0 */

/* Parse {__GLIFunctionDispatchRec="a"^?"b"^?...} at enc into the layout; slots found. 6.x and 7.x
 * carry the record without field names ({__GLIFunctionDispatchRec=^?^?...}): then only the count is
 * kept (gles_encode_unnamed) for the exports to name, and 0 is returned. */
static unsigned gles_layout_from_encode(const char *enc)
{
    static const char tag[] = "{__GLIFunctionDispatchRec=";
    const char *p = enc;
    unsigned i = 0, k, fields = 0;

    for (k = 0; tag[k]; k++) if (p[k] != tag[k]) return 0;
    p += k;
    while (*p && *p != '}') {
        if (*p == '"') {
            const char *s = ++p;
            while (*p && *p != '"') p++;
            if (!*p) break;
            gles_layout_set(i++, s, (unsigned)(p - s));
        } else if (p[0] == '^' && p[1] == '?') {
            fields++;
            p++;
        }
        p++;
    }
    if (!i) gles_encode_unnamed = fields;
    return i;
}

extern unsigned _dyld_image_count(void);
extern const char *_dyld_get_image_name(unsigned);
extern const void *_dyld_get_image_header(unsigned);
extern long _dyld_get_image_vmaddr_slide(unsigned);

static const char *gles_encode_override;    /* the front end: the stock OpenGLES's, read from the shared cache */

/* OpenGLES's mapped __TEXT: the @encode is a C string in it. */
static const char *gles_find_encode(void)
{
    static const char tag[] = "{__GLIFunctionDispatchRec=";
    unsigned i, n = _dyld_image_count();
    if (gles_encode_override) return gles_encode_override;
    for (i = 0; i < n; i++) {
        const char *name = _dyld_get_image_name(i);
        const unsigned *mh;
        const unsigned char *lc;
        unsigned c, ncmds;
        if (!name) continue;
        {   /* ends with "/OpenGLES.framework/OpenGLES" */
            static const char want[] = "/OpenGLES.framework/OpenGLES";
            unsigned l = slen(name), wl = sizeof want - 1;
            if (l < wl || !gles_streq(name + l - wl, want)) continue;
        }
        mh = _dyld_get_image_header(i);
        if (!mh || mh[0] != 0xfeedface) continue;
        ncmds = mh[4];
        lc = (const unsigned char *)mh + 28;
        for (c = 0; c < ncmds; c++) {
            const unsigned *w = (const unsigned *)lc;
            if (w[0] == 1 && gles_streq((const char *)lc + 8, "__TEXT")) {   /* LC_SEGMENT */
                const char *base = (const char *)(unsigned long)(w[6] + _dyld_get_image_vmaddr_slide(i));
                unsigned long size = w[7], off;
                for (off = 0; off + sizeof tag - 1 <= size; off++) {
                    unsigned k = 0;
                    while (tag[k] && base[off + k] == tag[k]) k++;
                    if (!tag[k]) return base + off;
                }
            }
            lc += w[1];
        }
    }
    return 0;
}

/* ---- (b) the exported trampolines ------------------------------------------------------ */

/* The one dispatch-table offset the trampoline at p loads its target from, else -1: the
 * `ldr` immediates a register call or a load into pc goes through (contrib/gles-public/glitsv.py
 * and FirmwareKit's GLIDispatch.callLoads read the same forms), up to the unconditional
 * bx / pop {pc} / tail-call `ldr pc` that ends it. armv6 3.x calls with `mov lr, pc; ldr pc,
 * [ip, #off]`; 4.x armv7 is Thumb-2 with IT blocks (blxne in the glIs* trampolines). The table
 * starts right after the GC the trampoline loads into r0 from the same context: +0xc on 3.x/4.x
 * (slots from +0x10), +0x10 on 5.x (slots from +0x14, a word more ahead of the table). 5.x's
 * float-argument trampolines keep the context in lr (ldr.w lr, [r0, #0x78]; ldr.w lr, [lr, #off]). 6.x/7.x
 * often load the context into r0 itself (ldr r0, [r0, #0x78] off the thread pointer, then ldr r0, [r0, #0x10]),
 * so the TLS context load at 0x78 is never the GC's; their float trampolines hold the context in sb (r9). */
static int gles_trampoline_slot(const void *code, int thumb, unsigned nslots)
{
    const unsigned char *p = code;
    unsigned loads[16], offs[4], noffs = 0, pos = 0, i, j, gc = 0;
    int prev = 0, mlp = 0, it_left = 0, it_cond = 0;

    for (i = 0; i < 16; i++) loads[i] = 0;
#define NOTE(o) do { unsigned o_ = (o); if (o_ != 0xc && o_ != 0xc0 && o_ >= 0x10 && o_ < 0x10 + 4 * nslots) { \
        for (j = 0; j < noffs && offs[j] != o_; j++) {} \
        if (j == noffs && noffs < 4) offs[noffs++] = o_; } } while (0)
#define LOAD(rt, rn, imm, tail) do { unsigned rn_ = (rn), rt_ = (rt), imm_ = (imm); \
        if ((rn_ <= 9 || rn_ == 12 || rn_ == 14) && imm_ > 9) { loads[rt_] = imm_; if (rt_ == 0 && !gc && imm_ != 0x78) gc = imm_; \
            if (rt_ == 15) { NOTE(imm_); if (tail) goto done; } } } while (0)
#define CALL(rm) do { if (loads[(rm) & 15]) NOTE(loads[(rm) & 15]); } while (0)
    if (!thumb) {
        for (; pos < 160; pos += 4) {
            unsigned w = *(const unsigned *)(p + pos), cond = w >> 28, always = cond == 0xE;
            prev = mlp;
            mlp = w == 0xE1A0E00F;                                           /* mov lr, pc */
            if (cond == 0xF) continue;
            if ((w & 0x0E500000) == 0x04100000 || (w & 0x0E500000) == 0x04500000) {   /* ldr/ldrb imm12 */
                if ((w & 0x01800000) == 0x01800000)                          /* P=1, U=1 */
                    LOAD((w >> 12) & 0xF, (w >> 16) & 0xF, w & 0xFFF, always && !(w & 0x00400000) && !prev);
                if (w == 0xE49DF004) break;                                  /* pop {pc} */
            } else if ((w & 0x0FFFFFF0) == 0x012FFF30) {                     /* blx rm */
                CALL(w);
            } else if ((w & 0x0FFFFFF0) == 0x012FFF10) {                     /* bx rm */
                CALL(w);
                if (always) break;
            } else if (always && (w & 0x0FFF0000) == 0x08BD0000 && (w & 0x8000)) {
                break;                                                       /* pop {..., pc} */
            }
        }
    } else {
        while (pos + 2 <= 160) {
            unsigned h1 = *(const unsigned short *)(p + pos), h2, rn, rt, top = h1 >> 11;
            int wide = top == 0x1D || top == 0x1E || top == 0x1F, conditional = it_left > 0 && it_cond;
            if (it_left) it_left--;
            prev = mlp;
            mlp = 0;
            if (!wide) {
                pos += 2;
                if ((h1 & 0xF800) == 0x6800) LOAD(h1 & 7, (h1 >> 3) & 7, ((h1 >> 6) & 0x1F) * 4, 0);   /* ldr */
                else if ((h1 & 0xFF07) == 0x4780) CALL(h1 >> 3);                                        /* blx rm */
                else if ((h1 & 0xFF07) == 0x4700) { CALL(h1 >> 3); if (!conditional) break; }            /* bx rm */
                else if ((h1 & 0xFF00) == 0xBD00) { if (!conditional) break; }                          /* pop {..pc} */
                else if (h1 == 0x46FE) mlp = !conditional;                                              /* mov lr, pc */
                else if ((h1 & 0xFF00) == 0xBF00 && (h1 & 0xF)) {                                       /* it */
                    it_left = 4 - __builtin_ctz(h1 & 0xF);
                    it_cond = ((h1 >> 4) & 0xF) != 0xE;
                }
                continue;
            }
            if (pos + 4 > 160) break;
            h2 = *(const unsigned short *)(p + pos + 2);
            pos += 4;
            rn = h1 & 0xF;
            rt = h2 >> 12;
            switch (h1 & 0xFFF0) {
            case 0xF8D0: LOAD(rt, rn, h2 & 0xFFF, 0); break;                                            /* ldr.w */
            case 0xF890: case 0xF8B0: case 0xF990: case 0xF9B0:                                         /* ldrb/h.w */
                if (rt != 15) LOAD(rt, rn, h2 & 0xFFF, 0);
                break;
            case 0xF850: case 0xF810: case 0xF830: case 0xF910: case 0xF930:                            /* [rn, #imm8] */
                if ((h2 & 0x0800) && (h2 & 0x0600) == 0x0600 && !(rt == 15 && (h1 & 0xFFF0) != 0xF850))
                    LOAD(rt, rn, h2 & 0xFF, (h1 & 0xFFF0) == 0xF850 && (h2 & 0x0100) && !conditional && !prev);
                break;
            }
        }
    }
done:
#undef NOTE
#undef LOAD
#undef CALL
    {
        unsigned base = gc == 0x10 ? 0x14 : 0x10;
        return noffs == 1 && offs[0] >= base ? (int)((offs[0] - base) / 4) : -1;
    }
}

/* Where an export's code is: dlsym, unless the front end (which replaces OpenGLES whole, so dlsym
 * finds its own exports) points this at the stock image in the shared cache. */
static void *(*gles_export_lookup)(const char *name);

/* Every exported row's trampoline, decoded; slots named. With `check` set the layout is only
 * compared against what is already there (the @encode's), and disagreements are logged. */
static unsigned gles_layout_from_exports(unsigned nslots, int check)
{
    unsigned i, found = 0, agree = 0, differ = 0;
    for (i = 0; i < GLES_N_FNS; i++) {
        const gles_fn_t *f = &gles_fns[i];
        void *p;
        int slot;
        char alias[64];
        unsigned k = 0;
        if (!(f->flags & GLES_F_EXPORT)) continue;
        if (!(p = gles_export_lookup ? gles_export_lookup(f->name) : dlsym(RTLD_DEFAULT, f->name))) {
            while (f->name[k] && k < sizeof alias - 4) { alias[k] = f->name[k]; k++; }   /* 3.x: the OES spelling */
            alias[k] = 'O'; alias[k + 1] = 'E'; alias[k + 2] = 'S'; alias[k + 3] = 0;
            p = gles_export_lookup ? gles_export_lookup(alias) : dlsym(RTLD_DEFAULT, alias);
        }
        if (!p) continue;
        slot = gles_trampoline_slot((const void *)((unsigned long)p & ~1UL), (unsigned long)p & 1, nslots);
        if (slot < 0 || slot >= (int)GLES_MAX_SLOTS) continue;
        found++;
        if (check) {
            if (gli.fn[slot] == (int)i) agree++;
            else {
                differ++;
                w("[gles] export "); w(f->name); w(" -> slot "); wd((unsigned)slot); w(", the @encode says ");
                w(gli.field[slot] ? gli.field[slot] : "(nothing)"); w("\n");
            }
        } else if (gli.fn[slot] < 0) {
            gles_layout_set((unsigned)slot, f->field, slen(f->field));
        }
    }
    if (check) {
        w("[gles] exports cross-check: "); wd(agree); w(" agree, "); wd(differ); w(" differ, of ");
        wd(found); w(" decoded\n");
    }
    return found;
}

/* ---- the hello and the discovery ------------------------------------------------------- */

#define QC_GLES_HELLO 0x142
#define GLES_HELLO_PROTO 1            /* the name-keyed wire (guest-package.h GUEST_GLES_PROTO) */
#define GLES_HELLO_VERBOSE 0x100      /* in the reply: IT_GLES_VERBOSE, log the whole table (a trap per line) */
#define GLES_HELLO_DEBUG 0x200        /* gles-debug: cross-check the layout against the exports, one line */

/* Tell the host which wire this shim speaks and which name table it was built from; the reply
 * says the host's protocol and whether it wants the layout logged (gles-debug / IT_GLES_VERBOSE). */
static long long gles_hello(void)
{
    volatile qemu_call_t q;
    q.call_number = QC_GLES_HELLO;
    q.ag.buffer = 0;
    q.ag.offset = GLES_HELLO_PROTO;
    q.ag.length = 0;
    q.ag.token = GLES_NAMES_VERSION;
    q.retval = 0;
    q.error = 0;
    qemu_call_trap(&q);
    return q.retval;
}

/* Discover the layout once. nslots: what the framework allotted (the MBX path's X+end), 0 if
 * unknown (the GLI path). Returns the number of slots known. */
static unsigned gles_discover(unsigned nslots)
{
    const char *enc;
    long long hello;
    unsigned i, named = 0, unknown = 0;

    if (gli.n) return gli.n;
    gles_layout_reset();
    hello = gles_hello();
    if ((enc = gles_find_encode()) && gles_layout_from_encode(enc)) {
        gli.how = "encode";
    } else {
        if (!nslots) nslots = gles_encode_unnamed;  /* a name-less @encode still counts the slots */
        if (gles_layout_from_exports(nslots ? nslots : GLES_MAX_SLOTS, 0)) {
            gli.how = gles_encode_unnamed ? "the @encode's count, named by the exports" : "exports";
            if (nslots) gli.n = nslots;
        }
    }
    for (i = 0; i < gli.n; i++) {
        if (gli.fn[i] >= 0) named++; else unknown++;
    }
    w("[gles] dispatch layout from "); w(gli.how); w(": "); wd(gli.n); w(" slots, "); wd(named);
    w(" named, "); wd(unknown); w(" unknown to the name table (version "); wx(GLES_NAMES_VERSION);
    w("), host protocol "); wd((unsigned)(hello & 0xff));
    if (nslots && nslots != gli.n) { w("; the framework allotted "); wd(nslots); }
    w("\n");
    if (hello < 0 || (hello & 0xff) != GLES_HELLO_PROTO) {
        w("[gles] the host speaks another wire protocol: GL calls may go astray\n");
        refused("hello:", "protocol", ~0u);
    }
    for (i = 0; i < gli.n; i++) {
        if (gli.fn[i] < 0 && gli.field[i]) {
            w("[gles] unknown dispatch field "); w(gli.field[i]); w(" at slot "); wd(i); w("\n");
        }
    }
    /* The table dump is thousands of log traps inside the first context's creation, which is
     * enough to make 4.x CoreAnimation decline the first buffers of a layer bound right after;
     * so it is asked for by IT_GLES_VERBOSE alone, and gles-debug gets the one-line cross-check. */
    if (hello > 0 && (hello & GLES_HELLO_VERBOSE)) {
        for (i = 0; i < gli.n; i++) {
            w("[gli] slot "); wd(i); w(" "); w(gli.field[i] ? gli.field[i] : "?");
            if (gli.fn[i] >= 0) { w(" "); w(gles_fns[gli.fn[i]].name); w(" id "); wd(gles_fns[gli.fn[i]].id); }
            w("\n");
        }
    }
    if (hello > 0 && (hello & (GLES_HELLO_VERBOSE | GLES_HELLO_DEBUG)) && gles_streq(gli.how, "encode"))
        gles_layout_from_exports(gli.n, 1);
    return gli.n;
}

/* The slot this firmware keeps id at, or -1. */
static int gles_slot_of(unsigned id)
{
    return id <= GLES_ID_MAX ? gli.slot[id] : -1;
}

/* Fill the framework's table of n slots (0: as many as discovered): a hand thunk (hand[id],
 * indexed by wire id, 0 = none) wins, then the row's forwarder or by-name stub, and a slot whose
 * field the table lacks gets its by-slot stub. Every entry is filled: the trampolines never
 * null-check. */
static unsigned gles_fill(void **fw, unsigned n, void *const *hand)
{
    unsigned i, have = gles_discover(n);
    if (!n) n = have;
    if (n > GLES_MAX_SLOTS) n = GLES_MAX_SLOTS;
    for (i = 0; i < n; i++) {
        int k = i < have ? gli.fn[i] : -1;
        void *f = gles_unknown_table[i];
        if (k >= 0) {
            unsigned id = gles_fns[k].id;
            f = hand && id < GLES_N_HAND && hand[id] ? hand[id] : gles_fn_ptr[k];
        }
        fw[i] = f;
    }
    return n;
}

/* A slot the name table has no row for: say so once, by the firmware's field name. */
static int gles_unknown_slot(unsigned slot)
{
    static unsigned char seen[GLES_MAX_SLOTS];
    const char *field = slot < GLES_MAX_SLOTS && gli.field[slot] ? gli.field[slot] : "?";
    if (slot < GLES_MAX_SLOTS && !seen[slot]) {
        seen[slot] = 1;
        w("[gles] unimplemented GL entry point (field "); w(field); w(", dispatch slot "); wd(slot);
        w(") -- the app will render wrong\n");
    }
    refused("unimpl:field:", field, ~0u);
    return 0;
}

static int gles_stub(const char *name)
{
    static unsigned char seen[GLES_ID_MAX + 1];
    int k;
    if (inert_stub(name)) return 0;             /* a hint: the no-op is the implementation */
    k = gles_row_of_name(name);
    if (k >= 0 && !seen[gles_fns[k].id]) {
        seen[gles_fns[k].id] = 1;
        w("[gles] unimplemented GL entry point "); w(name); w(" -- the app will render wrong\n");
    }
    refused("unimpl:", name, ~0u);
    return 0;
}
