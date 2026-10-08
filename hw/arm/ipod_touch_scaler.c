#include "qemu/osdep.h"
#include "exec/cpu-common.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"

/* 7E18 AppleM2ScalerCSCDriver: 0xc0732388 programs formats, 0xc07323f0
 * source geometry, 0xc073246c destination geometry, 0xc0730288 acknowledges
 * completion. The device-tree scaler node supplies VIC interrupt 0x25. */
typedef struct {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[0x1000 / 4];
    /* iPad: buffers are dart2 IOVAs (DT mapper-scaler, stream 2). */
    hwaddr (*xlate)(void *opaque, uint32_t va, unsigned sid);
    void *xlate_opaque;
    unsigned sid;
    uint32_t version;   /* +0x260, the block's version: 0 on the S5L8720 (no tiled buffers) */
} IPodScalerState;

void ipod_scaler_set_iommu(DeviceState *dev,
                           hwaddr (*xlate)(void *opaque, uint32_t va, unsigned sid),
                           void *opaque, unsigned sid);
void ipod_scaler_set_iommu(DeviceState *dev,
                           hwaddr (*xlate)(void *opaque, uint32_t va, unsigned sid),
                           void *opaque, unsigned sid)
{
    IPodScalerState *s = (IPodScalerState *)dev;

    s->xlate = xlate;
    s->xlate_opaque = opaque;
    s->sid = sid;
}

/*
 * AppleM2ScalerCSCDriver reads +0x260 once at start (8F190 0x80a45696, kept at driver+0x34) and gates features
 * on it: tiled buffers from 0x20002, more capabilities past 0x20006 and past 0x40006. 4.3's QuartzCore scales an
 * EAGL layer from a tiled surface, which version 0 refuses ("Scaler block (version = 0x0) for this chip does not
 * support tiled buffers"). The model reads the rows linearly either way: the host writes rendered surfaces linear.
 */
void ipod_scaler_set_version(DeviceState *dev, uint32_t version);
void ipod_scaler_set_version(DeviceState *dev, uint32_t version)
{
    IPodScalerState *s = (IPodScalerState *)dev;

    s->version = version;
    s->regs[0x260 / 4] = version;
}

static hwaddr scaler_pa(IPodScalerState *s, uint32_t va)
{
    return s->xlate ? s->xlate(s->xlate_opaque, va, s->sid) : va;
}

/* Write `len` bytes at a bus address (an IOVA behind the IOMMU), a page at a
 * time. False if any page is unmapped. */
static bool scaler_write_bus(IPodScalerState *s, uint32_t va, const void *buf,
                             unsigned len)
{
    const uint8_t *p = buf;

    while (len) {
        unsigned n = MIN(len, 0x1000 - (va & 0xfff));
        hwaddr pa = scaler_pa(s, va);

        if (pa == (hwaddr)-1) {
            return false;
        }
        cpu_physical_memory_write(pa, p, n);
        va += n;
        p += n;
        len -= n;
    }
    return true;
}

/*
 * The bus address of row `y` of a buffer whose rows are `pitch` bytes apart in memory and
 * `stride` apart as the block is programmed (+0x1c/+0x3c). Where the stride is a power of
 * two past the pitch, the kernel maps the buffer in 64 KiB windows of 64 KiB / stride rows,
 * each packed at the pitch and only as many pages mapped as those rows fill: 2x of a 1x
 * iPad app, 960x640 at stride 4096 (16 rows of 3840 bytes, 15 pages of each window; issue
 * 47), as the Zoom source below (11 of 16 at 683 wide).
 */
static uint32_t scaler_row(uint32_t base, unsigned y, unsigned stride, unsigned pitch)
{
    if (pitch < stride && stride < 0x10000 && !(stride & (stride - 1))) {
        unsigned rows = 0x10000 / stride;

        return base + (y / rows) * 0x10000 + (y % rows) * pitch;
    }
    return base + y * stride;
}

/*
 * 32-bit RGB to 32-bit RGB with scaling: what iPad Accessibility > Zoom asks
 * for. CA renders the visible crop (683x512 at 1.5x) and the scaler blows it
 * up onto the 1024x768 framebuffer. +0x10/+0x30 formats (low 3 bits 6 =
 * 32 bpp, same layout both sides, so pixels are copied as they are), +0x14
 * source, +0x24 source w << 16 | h, +0x34 destination, +0x3c destination
 * stride in pixels, +0x40 destination w << 16 | h, +0x50/+0x54 x/y step in
 * 16.16.
 *
 * The source as measured (7B500 Zoom, a one-shot dump of dart2 stream 2):
 * rows of ALIGN(w, 64) pixels, packed, but reached through an IOVA range in
 * which only the leading pages of every 64 KiB window are mapped (11 of 16
 * for w = 683). Read back-to-back, the mapped pages hold exactly that packed
 * image, so the source is gathered that way; +0x1c (0x0400_0400 here) is not
 * what locates the rows.
 * ponytail: layout inferred from one mode (Zoom, 32 bpp); nearest sampling
 * where the hardware has polyphase taps at +0x70 on. Decode +0x1c and the
 * mapping properly if another RGB client shows up.
 */
static bool scaler_rgb(IPodScalerState *s)
{
    uint32_t *r = s->regs;
    unsigned sw = (r[0x24 / 4] >> 16) & 0x1fff, sh = r[0x24 / 4] & 0x1fff;
    unsigned dw = (r[0x40 / 4] >> 16) & 0x1fff, dh = r[0x40 / 4] & 0x1fff;
    unsigned ds = (r[0x3c / 4] & 0xffff) * 4, pitch = ROUND_UP(sw, 64);
    uint32_t sx = r[0x50 / 4], sy = r[0x54 / 4];
    size_t need, got = 0;

    if ((r[0x10 / 4] & 7) != 6 || (r[0x30 / 4] & 7) != 6 || !sw || !sh ||
        !dw || !dh || sw > 2048 || sh > 2048 || dw > 2048 || dh > 2048 ||
        ds < dw * 4) {
        return false;
    }
    /* Rotated 90 degrees when the steps walk the destination's rows down the source's
     * columns: a 1x iPhone app on the iPad's landscape panel (320x480 -> 480x320 at 1:1,
     * 960x640 at 2x), the way the rest of the portrait UI is turned. */
    bool rot = sx && sy && sw != sh && ((uint64_t)dw * sx) >> 16 == sh && ((uint64_t)dh * sy) >> 16 == sw;
    sx = sx ? sx : ((uint64_t)sw << 16) / dw;
    sy = sy ? sy : ((uint64_t)sh << 16) / dh;
    need = (size_t)pitch * sh * 4;
    g_autofree uint8_t *src = g_malloc(need);
    /* Gather the mapped pages in IOVA order; give up after 4x the size. */
    for (uint32_t va = r[0x14 / 4]; got < need && va - r[0x14 / 4] < need * 4;
         va += 0x1000) {
        hwaddr pa = scaler_pa(s, va);

        if (pa != (hwaddr)-1) {
            size_t n = MIN(0x1000, need - got);
            cpu_physical_memory_read(pa, src + got, n);
            got += n;
        }
    }
    if (got < need) {
        return false;
    }
    g_autofree uint32_t *row = g_new(uint32_t, dw);
    for (unsigned y = 0; y < dh; y++) {
        const uint32_t *line = (const uint32_t *)src +
            (size_t)MIN(((uint64_t)y * sy) >> 16, sh - 1) * pitch;

        for (unsigned x = 0; x < dw; x++) {
            if (rot) {      /* destination (x, y) is source column sw - 1 - y, row x */
                unsigned u = sw - 1 - MIN(((uint64_t)y * sy) >> 16, sw - 1);
                row[x] = ((const uint32_t *)src)[(size_t)MIN(((uint64_t)x * sx) >> 16, sh - 1) * pitch + u];
            } else {
                row[x] = line[MIN(((uint64_t)x * sx) >> 16, sw - 1)];
            }
        }
        if (!scaler_write_bus(s, scaler_row(r[0x34 / 4], y, ds, ROUND_UP(dw, 64) * 4), row, dw * 4)) {
            return false;
        }
    }
    return true;
}

static bool scaler_range(uint64_t base, unsigned stride, unsigned rows,
                         unsigned bytes)
{
    return rows && stride >= bytes && base >= 0x08000000 &&
           base + (uint64_t)(rows - 1) * stride + bytes <= 0x10000000;
}

static bool scaler_convert(IPodScalerState *s)
{
    uint32_t *r = s->regs;
    unsigned w = (r[0x24 / 4] >> 16) & 0x1fff, h = r[0x24 / 4] & 0x1fff;
    unsigned ys = r[0x1c / 4] & 0xffff, uvs = r[0x1c / 4] >> 16;
    unsigned fmt = r[0x30 / 4] & 7, bpp = fmt == 4 ? 2 : 4;
    unsigned ds = (r[0x3c / 4] & 0xffff) * bpp;
    uint32_t ybase = r[0x14 / 4], uvbase = r[0x18 / 4], dest = r[0x34 / 4];
    /* ponytail: implement the observed unscaled NV12-to-RGB movie path first;
     * scaling needs the programmed polyphase filters, not guessed sampling. */
    if (!w || !h || w > 2048 || h > 2048 || (w | h) & 1 ||
        r[0x10 / 4] || (fmt != 4 && fmt != 6) || (r[0x30 / 4] & ~7u) ||
        r[0x20 / 4] || r[0x24 / 4] != r[0x40 / 4] ||
        !scaler_range(ybase, ys, h, w) ||
        !scaler_range(uvbase, uvs, h / 2, w) ||
        !scaler_range(dest, ds, h, w * bpp)) return false;
    g_autofree uint8_t *yplane = g_malloc((size_t)w * h);
    g_autofree uint8_t *uvplane = g_malloc((size_t)w * h / 2);
    g_autofree uint8_t *row = g_malloc(w * bpp);
    /* Snapshot sources before writing: IOSurface transfers may alias. */
    for (unsigned y = 0; y < h; y++) {
        cpu_physical_memory_read(ybase + y * ys, yplane + y * w, w);
        if (!(y & 1)) cpu_physical_memory_read(uvbase + (y / 2) * uvs,
                                             uvplane + (y / 2) * w, w);
    }
    int matrix[9];
    for (unsigned i = 0; i < 9; i++) {
        unsigned v = r[0x220 / 4 + i] & 0xfff;
        matrix[i] = (v ^ 0x800) - 0x800;
    }
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            int in[] = { yplane[y * w + x] - ((r[1] & 0x200) ? 16 : 0),
                         uvplane[(y / 2) * w + (x & ~1u)] - 128,
                         uvplane[(y / 2) * w + (x & ~1u) + 1] - 128 };
            unsigned rgb[3];
            for (unsigned c = 0; c < 3; c++) {
                int v = (matrix[c * 3] * in[0] + matrix[c * 3 + 1] * in[1] +
                         matrix[c * 3 + 2] * in[2] + 256) >> 9;
                rgb[c] = MIN(255, MAX(0, v));
            }
            if (fmt == 4) {
                stw_le_p(row + x * 2, ((rgb[0] >> 3) << 11) |
                         ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
            } else {
                row[x * 4] = rgb[2]; row[x * 4 + 1] = rgb[1];
                row[x * 4 + 2] = rgb[0]; row[x * 4 + 3] = 255;
            }
        }
        cpu_physical_memory_write(dest + y * ds, row, w * bpp);
    }
    return true;
}

static void scaler_irq(IPodScalerState *s)
{
    qemu_set_irq(s->irq, (s->regs[2] & s->regs[3] & 1) != 0);
}

static uint64_t scaler_read(void *opaque, hwaddr off, unsigned size)
{
    IPodScalerState *s = opaque;
    return s->regs[off / 4];
}

static void scaler_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    IPodScalerState *s = opaque;
    if (off == 0xc) s->regs[3] &= ~value;
    else s->regs[off / 4] = value;
    if (off == 4 && (value & 2)) {
        memset(s->regs, 0, sizeof(s->regs));
        s->regs[0x260 / 4] = s->version;
    } else if (off == 4 && (value & 1)) {
        if (!scaler_convert(s) && !scaler_rgb(s)) error_report("scaler: unsupported or invalid transfer %08x -> %08x geometry %08x -> %08x",
            s->regs[4], s->regs[12], s->regs[9], s->regs[16]);
        s->regs[1] &= ~1u;
        s->regs[3] |= 1;
    }
    scaler_irq(s);
}

static const MemoryRegionOps scaler_ops = {
    .read = scaler_read, .write = scaler_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* 5.x's reset polls +0x4 and +0x10 with byte loads (9A334 0x809cfddc ldrb): a word access serves them */
    .valid.min_access_size = 1, .valid.max_access_size = 4,
    .impl.min_access_size = 4, .impl.max_access_size = 4,
};

static void scaler_reset(DeviceState *dev)
{
    IPodScalerState *s = (IPodScalerState *)dev;
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0x260 / 4] = s->version;
    scaler_irq(s);
}

static void scaler_init(Object *obj)
{
    IPodScalerState *s = (IPodScalerState *)obj;
    memory_region_init_io(&s->iomem, obj, &scaler_ops, s, "scaler-csc", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static int scaler_post_load(void *opaque, int version_id)
{
    scaler_irq(opaque);
    return 0;
}

static const VMStateDescription scaler_vmstate = {
    .name = "ipod-scaler", .version_id = 1, .minimum_version_id = 1,
    .post_load = scaler_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IPodScalerState, 0x1000 / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void scaler_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &scaler_vmstate;
    device_class_set_legacy_reset(dc, scaler_reset);
}

static const TypeInfo scaler_type = {
    .name = "ipodtouch.scaler", .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodScalerState), .instance_init = scaler_init,
    .class_init = scaler_class_init,
};
static void scaler_register(void) { type_register_static(&scaler_type); }
type_init(scaler_register)
