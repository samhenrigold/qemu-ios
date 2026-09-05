#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "exec/address-spaces.h"
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
} IPodScalerState;

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
    } else if (off == 4 && (value & 1)) {
        if (!scaler_convert(s)) error_report("scaler: unsupported or invalid transfer %08x -> %08x geometry %08x -> %08x",
            s->regs[4], s->regs[12], s->regs[9], s->regs[16]);
        s->regs[1] &= ~1u;
        s->regs[3] |= 1;
    }
    scaler_irq(s);
}

static const MemoryRegionOps scaler_ops = {
    .read = scaler_read, .write = scaler_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static void scaler_reset(DeviceState *dev)
{
    IPodScalerState *s = (IPodScalerState *)dev;
    memset(s->regs, 0, sizeof(s->regs));
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

static void scaler_class_init(ObjectClass *klass, void *data)
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
