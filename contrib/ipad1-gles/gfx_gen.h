/*
 * Which libGFXShared this process loaded, read off the image itself: glishim (gfxInitializeLibrary's
 * arguments) and gldshim (the gld interface it answers) both follow it.
 *
 *   0  none loaded (3.2.x: its EAGL never loads libGFXShared)
 *   4  iOS 4: gld 3.1.0, 79 gld* entry points, gldCreateShared(&slot, mask, n);
 *      gfxInitializeLibrary(svcs, z, n, EAGL's IOSurface callback, the engine's, io, init)
 *   5  iOS 5: gld 4.0.44, 111 entry points, gldCreateDevice per device and
 *      gldCreateShareGroup(device, &slot, n); gfxInitializeLibrary(svcs, z, n, the engine's
 *      surface-properties and new-surface callbacks, io, flags)
 *
 * The generation is whether the gld table libGFXShared dlsyms (its __cstring) names
 * gldCreateShareGroup. docs/ipad1/ios5.md, "GL on 5.1.1".
 *
 * Within generation 4 the gld revision moved with the interface: 4.3's libGFXShared accepts only
 * 3.1.4, and its table adds the sampler objects (gldCreateSampler), gldCopyBufferSubData and
 * gldUpdateReadFramebuffer; 4.2.1's accepts only 3.1.0 and names none of them. gfx_gld_revision
 * reads that off the same table (README.md, "4.3.x").
 */
extern unsigned _dyld_image_count(void);
extern const char *_dyld_get_image_name(unsigned);
extern const void *_dyld_get_image_header(unsigned);
extern long _dyld_get_image_vmaddr_slide(unsigned);

static int gfx_ends_with(const char *s, const char *tail)
{
    unsigned long n = 0, t = 0;
    while (s[n]) n++;
    while (tail[t]) t++;
    if (t > n) return 0;
    for (unsigned long i = 0; i < t; i++)
        if (s[n - t + i] != tail[i]) return 0;
    return 1;
}

/* 1 if the gld table of the loaded libGFXShared names `key` (sizeof key includes the NUL),
 * 0 if not, -1 if no libGFXShared is loaded. */
static int gfx_names(const char *key, unsigned long size)
{
    for (unsigned i = 0; i < _dyld_image_count(); i++) {
        const char *name = _dyld_get_image_name(i);
        if (!name || !gfx_ends_with(name, "/libGFXShared.dylib")) continue;
        const unsigned *mh = _dyld_get_image_header(i);
        const unsigned char *lc = (const unsigned char *)(mh + 7);
        for (unsigned c = 0; c < mh[4]; c++, lc += ((const unsigned *)lc)[1]) {
            const unsigned *seg = (const unsigned *)lc;
            if (seg[0] != 1 /* LC_SEGMENT */) continue;
            const unsigned char *sect = lc + 56;
            for (unsigned k = 0; k < seg[12]; k++, sect += 68) {
                const char *sn = (const char *)sect, *want = "__cstring";
                unsigned j = 0;
                while (j < 10 && sn[j] == want[j]) j++;      /* incl. the NUL */
                if (j < 10) continue;
                const unsigned *hdr = (const unsigned *)(sect + 32);   /* addr, size */
                const char *p = (const char *)(hdr[0] + _dyld_get_image_vmaddr_slide(i));
                for (unsigned o = 0; o + size <= hdr[1]; o++) {
                    j = 0;
                    while (j < size && p[o + j] == key[j]) j++;
                    if (j == size && (o == 0 || p[o - 1] == 0)) return 1;
                }
            }
        }
        return 0;
    }
    return -1;
}

static int gfx_generation(void)
{
    static const char key[] = "gldCreateShareGroup";
    int n = gfx_names(key, sizeof key);
    return n < 0 ? 0 : n ? 5 : 4;
}

/* The gld revision a generation-4 libGFXShared checks for: 4 (3.1.4) if its table names
 * gldCreateSampler, else 0 (3.1.0). */
__attribute__((unused)) static int gfx_gld_revision(void)
{
    static const char key[] = "gldCreateSampler";
    return gfx_names(key, sizeof key) > 0 ? 4 : 0;
}
