/*
 * S5L8930 display: DisplayPipe0 (+ the RGBOUT pipe1 twin), CLCD timing
 * generators, dart2 and a QEMU console scanning out the UI layer.
 *
 * Register contract: docs/ipad1/research/gap-display-stack-kernel.md §1.
 * The pipe is a RAM register file with three special things: the parameter
 * FIFO port (0x103c: header 0x8000_0000|nwords<<16|swapID, then packets of
 * count<<16|reg followed by `count` data words), the frame interrupt
 * (0x1028 enable / 0x102c W1C status, bit0 VBL + bit8 swap-done, completed
 * swap ID in 0x1048) and the UI layer registers the scanout reads
 * (0x1038 bit8/9 layer enable, 0x4044/0x5044 base, 0x4048 stride, 0x4040
 * format, 0x1030 size). Layer bases are dart2 IOVAs when the kernel has
 * installed a segment table for SID 0, physical addresses otherwise.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/arm/s5l8930.h"
#include "exec/address-spaces.h"
#include "migration/vmstate.h"
#include "ui/console.h"

#define PIPE_WORDS          (S5L8930_DISP_PIPE0_SIZE / 4)
#define CLCD_WORDS          (S5L8930_CLCD_SIZE / 4)
#define DART_WORDS          (S5L8930_DART2_SIZE / 4)
#define TVOUT_SIZE          0x2000
#define DART_SIDS           4
#define DART_SEGS           64

#define DP_FLAGS            0x101c    /* the kernel spins WHILE (v & 0xf0) == 0x20: read idle */
#define DP_IRQ_ENABLE       0x1028
#define DP_IRQ_STATUS       0x102c    /* W1C */
#define DP_SIZE             0x1030    /* w << 16 | h */
#define DP_LAYERS           0x1038    /* bit8 UI0, bit9 UI1 */
#define DP_FIFO_PORT        0x103c
#define DP_FIFO_COUNT       0x1044    /* polled to 0 */
#define DP_SWAP_DONE        0x1048    /* low 16 bits: last completed swap ID */
#define DP_UNDERRUN_COLOR   0x2064
#define DP_UI_BASE(l)       (0x4000 + (l) * 0x1000)
#define DP_UI_FORMAT        0x40
#define DP_UI_ADDR          0x44
#define DP_UI_STRIDE        0x48

#define DP_IRQ_VBL          0x001
#define DP_IRQ_SWAP_DONE    0x100

#define CLCD_CTRL           0x00      /* bit8 = soft reset, self-clearing */
#define CLCD_ENVID          0x50      /* bit0 ENVID, bit1 "ready down" */

#define DART_TLB_OP         0x00      /* bit3 busy; op nibble 4 = read STE, 5 = write STE */
#define DART_DATA           0x08
#define DART_ERROR_STATUS   0x10      /* W1C */

#define VBL_PERIOD_NS       (NANOSECONDS_PER_SECOND / 60)
#define DEFAULT_WIDTH       1024
#define DEFAULT_HEIGHT      768

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930DisplayState, S5L8930_DISPLAY)

typedef struct {
    uint32_t regs[PIPE_WORDS];
    /* Parameter FIFO parser: packet in progress + the swap it belongs to. */
    uint32_t pkt_left;
    uint32_t pkt_off;
    uint32_t swap_id;
    struct S5L8930DisplayState *dev;
} DisplayPipe;

struct S5L8930DisplayState {
    SysBusDevice parent_obj;

    MemoryRegion pipe_mr[2], clcd_mr[2], dart_mr, tvout_mr;
    qemu_irq pipe_irq, clcd_irq;
    QemuConsole *con;
    QEMUTimer *vbl;
    uint64_t fb_base;        /* property: seed iBoot's scanout when nonzero */

    DisplayPipe pipe[2];     /* 0 = DisplayPipe0 (scanned out), 1 = RGBOUT */
    uint32_t clcd[2][CLCD_WORDS];
    uint32_t tvout[TVOUT_SIZE / 4];
    uint32_t dart[DART_WORDS];
    uint32_t ste[DART_SIDS][DART_SEGS];
};

/* ---- DisplayPipe ------------------------------------------------------- */

static void pipe0_update_irq(S5L8930DisplayState *s)
{
    uint32_t *r = s->pipe[0].regs;
    qemu_set_irq(s->pipe_irq, (r[DP_IRQ_STATUS / 4] & r[DP_IRQ_ENABLE / 4]) != 0);
}

static uint64_t pipe_read(void *opaque, hwaddr addr, unsigned size)
{
    DisplayPipe *p = opaque;

    switch (addr) {
    case DP_FLAGS:
        return 0;
    case DP_FIFO_COUNT:
        return 0;
    default:
        return p->regs[addr / 4];
    }
}

static void pipe_fifo_write(DisplayPipe *p, uint32_t val)
{
    if (val & 0x80000000) {                     /* transaction header */
        p->swap_id = val & 0xffff;
        p->pkt_left = 0;
    } else if (p->pkt_left == 0) {              /* packet header */
        p->pkt_left = val >> 16;
        p->pkt_off = val & 0xffff;
    } else {                                    /* packet data */
        if (p->pkt_off < S5L8930_DISP_PIPE0_SIZE) {
            p->regs[p->pkt_off / 4] = val;
        }
        p->pkt_off += 4;
        p->pkt_left--;
    }
}

static void pipe_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    DisplayPipe *p = opaque;
    S5L8930DisplayState *s = p->dev;

    switch (addr) {
    case DP_FIFO_PORT:
        pipe_fifo_write(p, val);
        break;
    case DP_IRQ_STATUS:
        p->regs[addr / 4] &= ~val;
        break;
    case DP_FIFO_COUNT:
    case DP_SWAP_DONE:
        break;
    default:
        p->regs[addr / 4] = val;
    }
    if (p == &s->pipe[0]) {
        pipe0_update_irq(s);
    }
}

static const MemoryRegionOps pipe_ops = {
    .read = pipe_read,
    .write = pipe_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* One frame: the queued swap completes and the VBL fires. */
static void vbl_tick(void *opaque)
{
    S5L8930DisplayState *s = opaque;
    uint32_t *r = s->pipe[0].regs;

    r[DP_SWAP_DONE / 4] = (r[DP_SWAP_DONE / 4] & ~0xffff) | s->pipe[0].swap_id;
    r[DP_IRQ_STATUS / 4] |= DP_IRQ_VBL | DP_IRQ_SWAP_DONE;
    pipe0_update_irq(s);
    timer_mod(s->vbl, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VBL_PERIOD_NS);
}

/* ---- CLCD timing generator ---------------------------------------------- */

static uint64_t clcd_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t *regs = opaque;
    uint32_t v = regs[addr / 4];

    if (addr == CLCD_ENVID) {
        v = (v & ~2u) | ((v & 1) ? 0 : 2);
    }
    return v;
}

static void clcd_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    uint32_t *regs = opaque;

    if (addr == CLCD_CTRL) {
        val &= ~0x100;    /* soft reset completes at once */
    }
    regs[addr / 4] = val;
}

/*
 * RGBOUT control (0x89600000, AppleRGBOUT reg index 1, this+0x1ab4) and
 * TV-out (SDO regs, this+0x5c): both power down by writing 0 to register
 * 0x00 and polling bit1 ("CLCK_DOWN_READY" c0627c6e, "SDO_CLKCON & 0x2"
 * c06757d6 in the 7B500 kernelcache). RGBOUT's reset writes 0x04 = 1 and
 * expects to read 1 back (c0627cf4), which plain RAM gives.
 */
static uint64_t ram_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t v = ((uint32_t *)opaque)[addr / 4];

    if (addr == 0) {
        v = (v & ~2u) | ((v & 1) ? 0 : 2);
    }
    return v;
}

static void ram_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    ((uint32_t *)opaque)[addr / 4] = val;
}

static const MemoryRegionOps ram_ops = {
    .read = ram_read,
    .write = ram_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps clcd_ops = {
    .read = clcd_read,
    .write = clcd_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* ---- dart2 --------------------------------------------------------------- */

static uint64_t dart_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8930DisplayState *s = opaque;

    return addr == DART_TLB_OP ? 0 : s->dart[addr / 4];   /* never busy */
}

static void dart_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    S5L8930DisplayState *s = opaque;
    unsigned sid = (val >> 8) & 0xf, seg = (val >> 22) & 0x3f;

    switch (addr) {
    case DART_TLB_OP:
        if (sid >= DART_SIDS) {
            break;
        }
        if ((val & 0xf) == 5) {
            s->ste[sid][seg] = s->dart[DART_DATA / 4];
        } else if ((val & 0xf) == 4) {
            s->dart[DART_DATA / 4] = s->ste[sid][seg];
        }
        break;
    case DART_ERROR_STATUS:
        s->dart[addr / 4] &= ~val;
        break;
    default:
        s->dart[addr / 4] = val;
    }
}

static const MemoryRegionOps dart_ops = {
    .read = dart_read,
    .write = dart_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * SID 0 (mapper-clcd) IOVA -> PA. STE = page-table PA, PTE = PA | 1. A
 * segment with no table is passed through untranslated, which is also what
 * iBoot's untranslated framebuffer and the kernel's identity "transition
 * mapping" amount to. Returns -1 for an invalid PTE.
 */
static hwaddr dart_xlate(S5L8930DisplayState *s, uint32_t va)
{
    uint32_t ste = s->ste[0][(va >> 22) & 0x3f] & ~0xfffu;
    uint32_t pte;

    if (!ste) {
        return va;
    }
    pte = ldl_le_phys(&address_space_memory, ste + ((va >> 12) & 0x3ff) * 4);
    if (!(pte & 1)) {
        qemu_log_mask(LOG_GUEST_ERROR, "dart2: invalid PTE 0x%08x for iova 0x%08x\n", pte, va);
    }
    return (pte & 1) ? ((pte & ~0xfffu) | (va & 0xfff)) : (hwaddr)-1;
}

/* Read `len` bytes of framebuffer at IOVA `va`, page by page. */
static void fb_read(S5L8930DisplayState *s, uint32_t va, uint8_t *dst, unsigned len)
{
    while (len) {
        unsigned n = MIN(len, 0x1000 - (va & 0xfff));
        hwaddr pa = dart_xlate(s, va);

        if (pa == (hwaddr)-1) {
            memset(dst, 0, n);
        } else {
            address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, dst, n);
        }
        va += n;
        dst += n;
        len -= n;
    }
}

/* ---- scanout ------------------------------------------------------------- */

static void display_invalidate(void *opaque)
{
}

/* ponytail: full redraw every host refresh (~30 Hz, 3 MiB), add dirty
 * tracking via framebuffer_update_display if it shows up in profiles. */
static void display_update(void *opaque)
{
    S5L8930DisplayState *s = opaque;
    uint32_t *r = s->pipe[0].regs;
    DisplaySurface *surface;
    unsigned w = (r[DP_SIZE / 4] >> 16) & 0x7ff, h = r[DP_SIZE / 4] & 0x7ff;
    int layer = (r[DP_LAYERS / 4] & 0x100) ? 0 : (r[DP_LAYERS / 4] & 0x200) ? 1 : -1;
    uint32_t fmt, base, stride, bpp;
    g_autofree uint8_t *row = NULL;

    if (!w || !h) {
        w = DEFAULT_WIDTH;
        h = DEFAULT_HEIGHT;
    }
    surface = qemu_console_surface(s->con);
    if (surface_width(surface) != w || surface_height(surface) != h) {
        qemu_console_resize(s->con, w, h);
        surface = qemu_console_surface(s->con);
    }
    if (surface_bits_per_pixel(surface) != 32) {
        return;
    }

    if (layer >= 0) {
        base = r[(DP_UI_BASE(layer) + DP_UI_ADDR) / 4];
        fmt = (r[(DP_UI_BASE(layer) + DP_UI_FORMAT) / 4] >> 8) & 7;
        stride = r[(DP_UI_BASE(layer) + DP_UI_STRIDE) / 4] & ~0x3fu;
        if (!base) {
            layer = -1;
        }
    }
    if (layer < 0) {
        uint32_t fill = r[DP_UNDERRUN_COLOR / 4] & 0xffffff;
        for (unsigned y = 0; y < h; y++) {
            uint32_t *d = (uint32_t *)(surface_data(surface) + y * surface_stride(surface));
            for (unsigned x = 0; x < w; x++) {
                d[x] = fill;
            }
        }
        dpy_gfx_update(s->con, 0, 0, w, h);
        return;
    }

    bpp = fmt ? 2 : 4;     /* 0 ARGB/BGRA, 2 ARGB4444, 4 RGB565 */
    row = g_malloc(w * 4);
    for (unsigned y = 0; y < h; y++) {
        uint32_t *d = (uint32_t *)(surface_data(surface) + y * surface_stride(surface));

        fb_read(s, base + y * stride, row, w * bpp);
        if (fmt == 0) {
            memcpy(d, row, w * 4);
        } else {
            const uint16_t *p = (const uint16_t *)row;
            for (unsigned x = 0; x < w; x++) {
                uint16_t v = le16_to_cpu(p[x]);
                d[x] = fmt == 2
                    ? ((v & 0xf00) << 12 | (v & 0xf00) << 8 |
                       (v & 0x0f0) << 8 | (v & 0x0f0) << 4 |
                       (v & 0x00f) << 4 | (v & 0x00f))
                    : ((v & 0xf800) << 8 | (v & 0xe000) << 3 |
                       (v & 0x07e0) << 5 | (v & 0x0600) >> 1 |
                       (v & 0x001f) << 3 | (v & 0x001c) >> 2);
            }
        }
    }
    dpy_gfx_update(s->con, 0, 0, w, h);
}

static const GraphicHwOps display_gfx_ops = {
    .invalidate = display_invalidate,
    .gfx_update = display_update,
};

/* ---- device -------------------------------------------------------------- */

static void s5l8930_display_reset(DeviceState *dev)
{
    S5L8930DisplayState *s = S5L8930_DISPLAY(dev);
    uint32_t *r = s->pipe[0].regs;

    for (int i = 0; i < 2; i++) {
        memset(s->pipe[i].regs, 0, sizeof(s->pipe[i].regs));
        s->pipe[i].pkt_left = s->pipe[i].pkt_off = s->pipe[i].swap_id = 0;
    }
    memset(s->clcd, 0, sizeof(s->clcd));
    memset(s->tvout, 0, sizeof(s->tvout));
    memset(s->dart, 0, sizeof(s->dart));
    memset(s->ste, 0, sizeof(s->ste));

    /* What iBoot leaves behind: UI0 live on a 1024x768 32bpp buffer. The
     * kernel adopts it from these registers, so without them there is no
     * console framebuffer. */
    r[DP_SIZE / 4] = DEFAULT_WIDTH << 16 | DEFAULT_HEIGHT;
    if (s->fb_base) {
        r[DP_LAYERS / 4] = 0x100;
        r[(DP_UI_BASE(0) + DP_UI_ADDR) / 4] = s->fb_base;
        r[(DP_UI_BASE(0) + DP_UI_STRIDE) / 4] = (DEFAULT_WIDTH * 4) | 2;
        r[0x4060 / 4] = DEFAULT_WIDTH << 16 | DEFAULT_HEIGHT;
    }
    pipe0_update_irq(s);
    timer_mod(s->vbl, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VBL_PERIOD_NS);
}

static void s5l8930_display_realize(DeviceState *dev, Error **errp)
{
    S5L8930DisplayState *s = S5L8930_DISPLAY(dev);

    s->vbl = timer_new_ns(QEMU_CLOCK_VIRTUAL, vbl_tick, s);
    s->con = graphic_console_init(dev, 0, &display_gfx_ops, s);
    qemu_console_resize(s->con, DEFAULT_WIDTH, DEFAULT_HEIGHT);
}

static void s5l8930_display_init(Object *obj)
{
    S5L8930DisplayState *s = S5L8930_DISPLAY(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->pipe[0].dev = s->pipe[1].dev = s;
    /* Order matches the header: pipe0, CLCD, DART2, RGBOUT (= AppleRGBOUT's
     * own DisplayPipe, reg index 0), TVOUT, RGBOUT2 (its control block). */
    memory_region_init_io(&s->pipe_mr[0], obj, &pipe_ops, &s->pipe[0],
                          "s5l8930.disp.pipe0", S5L8930_DISP_PIPE0_SIZE);
    memory_region_init_io(&s->clcd_mr[0], obj, &clcd_ops, s->clcd[0],
                          "s5l8930.disp.clcd", S5L8930_CLCD_SIZE);
    memory_region_init_io(&s->dart_mr, obj, &dart_ops, s,
                          "s5l8930.disp.dart2", S5L8930_DART2_SIZE);
    memory_region_init_io(&s->pipe_mr[1], obj, &pipe_ops, &s->pipe[1],
                          "s5l8930.disp.rgbout", S5L8930_DISP_PIPE0_SIZE);
    memory_region_init_io(&s->tvout_mr, obj, &ram_ops, s->tvout,
                          "s5l8930.disp.tvout", TVOUT_SIZE);
    memory_region_init_io(&s->clcd_mr[1], obj, &ram_ops, s->clcd[1],
                          "s5l8930.disp.rgbout2", S5L8930_CLCD_SIZE);
    sysbus_init_mmio(sbd, &s->pipe_mr[0]);
    sysbus_init_mmio(sbd, &s->clcd_mr[0]);
    sysbus_init_mmio(sbd, &s->dart_mr);
    sysbus_init_mmio(sbd, &s->pipe_mr[1]);
    sysbus_init_mmio(sbd, &s->tvout_mr);
    sysbus_init_mmio(sbd, &s->clcd_mr[1]);
    sysbus_init_irq(sbd, &s->pipe_irq);
    sysbus_init_irq(sbd, &s->clcd_irq);
}

static int s5l8930_display_post_load(void *opaque, int version_id)
{
    pipe0_update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_display_pipe = {
    .name = "s5l8930.display.pipe",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, DisplayPipe, PIPE_WORDS),
        VMSTATE_UINT32(pkt_left, DisplayPipe),
        VMSTATE_UINT32(pkt_off, DisplayPipe),
        VMSTATE_UINT32(swap_id, DisplayPipe),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_s5l8930_display = {
    .name = "s5l8930.display",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8930_display_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(pipe, S5L8930DisplayState, 2, 1,
                             vmstate_display_pipe, DisplayPipe),
        VMSTATE_UINT32_2DARRAY(clcd, S5L8930DisplayState, 2, CLCD_WORDS),
        VMSTATE_UINT32_ARRAY(tvout, S5L8930DisplayState, TVOUT_SIZE / 4),
        VMSTATE_UINT32_ARRAY(dart, S5L8930DisplayState, DART_WORDS),
        VMSTATE_UINT32_2DARRAY(ste, S5L8930DisplayState, DART_SIDS, DART_SEGS),
        VMSTATE_TIMER_PTR(vbl, S5L8930DisplayState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property s5l8930_display_properties[] = {
    DEFINE_PROP_UINT64("fb-base", S5L8930DisplayState, fb_base, 0),
};

static void s5l8930_display_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s5l8930_display_realize;
    device_class_set_legacy_reset(dc, s5l8930_display_reset);
    device_class_set_props(dc, s5l8930_display_properties);
    dc->vmsd = &vmstate_s5l8930_display;
}

static const TypeInfo s5l8930_display_info = {
    .name          = TYPE_S5L8930_DISPLAY,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8930DisplayState),
    .instance_init = s5l8930_display_init,
    .class_init    = s5l8930_display_class_init,
};

static void s5l8930_display_register_types(void)
{
    type_register_static(&s5l8930_display_info);
}

type_init(s5l8930_display_register_types)
