/*
 * S5L8930 SDIO: the SDHC block at 0x80000000 and the IOP's SDIO task in front
 * of it, with the Wi-Fi card (the iPod's Broadcom dongle model, as a BCM4329)
 * behind. docs/ipad1/wifi.md.
 *
 * On the real part the AP never drives the SDHC for commands: the kext
 * (AppleS5L8920XIOPSDIO) sends 512-byte commands on IOP ring 3 and the IOP
 * firmware runs them. What the AP does touch directly is the standard SDHCI
 * interrupt block, for the card interrupt (IOSDIOFamily c057f830..c057f88a:
 * normal status 0x30 bit 8, status enable 0x34, signal enable 0x38), on IRQ
 * 0x26. So this models the ring commands at the SD-command level and the
 * SDHCI registers only as far as that interrupt path needs.
 *
 * Ring command layout (kext 0xc0584000, getIOPSDIOMessage callers):
 *   +0x00 opcode  +0x08 status (0 = ok)  +0x0c SDHC base  +0x10 IOP state
 *   2 initIOPState    +0x10 out: state handle
 *   3 freeIOPState    4 resetController    5 setBusParam (clock, width)
 *   6 sendCommand     +0x14 u16 cmd, +0x18 arg; response[0..3] at +0x1c
 *   7 sendDMACommand  +0x2c u16 cmd, +0x30 arg, +0x36 u16 blkSize,
 *                     +0x38 u16 blkCount, +0x44 segment count,
 *                     +0x4c {u32 addr, u32 len}[]; response at +0x1c
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/arm/s5l8930.h"

#define SDHC_SIZE           0x100
#define SDHC_NORMAL_STATUS  0x30
#define SDHC_NORMAL_ENABLE  0x34
#define SDHC_NORMAL_SIGNAL  0x38
#define SDHC_CARD_INT       0x0100

#define CMD_OPCODE          0x00
#define CMD_STATUS          0x08
#define CMD_STATE           0x10
#define CMD_RESPONSE        0x1c
#define CMD6_CMD            0x14
#define CMD6_ARG            0x18
#define CMD7_CMD            0x2c
#define CMD7_ARG            0x30
#define CMD7_BLKSIZE        0x36
#define CMD7_BLKCOUNT       0x38
#define CMD7_NSEG           0x44
#define CMD7_SEGS           0x4c
#define CMD7_MAX_SEGS       ((S5L8930_SDIO_CMD_SIZE - CMD7_SEGS) / 8)

#define OP_INIT             2
#define OP_FREE             3
#define OP_RESET            4
#define OP_BUS_PARAM        5
#define OP_COMMAND          6
#define OP_DMA_COMMAND      7

#define STATUS_OK           0
#define STATUS_UNKNOWN      2           /* fw 0x164c "unrecognised sdio opcode" */

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930SDIOState, S5L8930_SDIO)

struct S5L8930SDIOState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    IPodTouchSDIOState *card;
    uint8_t regs[SDHC_SIZE];
};

static uint16_t sdhc_normal_status(S5L8930SDIOState *s)
{
    uint16_t st = lduw_le_p(&s->regs[SDHC_NORMAL_STATUS]);

    /* The card interrupt is a level from DAT1, never latched. */
    if (s->card && ipod_touch_sdio_card_irq(s->card) &&
        (lduw_le_p(&s->regs[SDHC_NORMAL_ENABLE]) & SDHC_CARD_INT)) {
        st |= SDHC_CARD_INT;
    }
    return st;
}

static void sdhc_update(S5L8930SDIOState *s)
{
    qemu_set_irq(s->irq, !!(sdhc_normal_status(s) &
                            lduw_le_p(&s->regs[SDHC_NORMAL_SIGNAL])));
}

/* The card's interrupt output changed: re-derive ours. */
static void sdhc_card_poke(void *opaque, int n, int level)
{
    sdhc_update(opaque);
}

static uint64_t sdhc_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8930SDIOState *s = opaque;
    uint8_t buf[4];

    memcpy(buf, &s->regs[addr], size);
    if (addr <= SDHC_NORMAL_STATUS + 1 && addr + size > SDHC_NORMAL_STATUS) {
        uint8_t st[2];
        stw_le_p(st, sdhc_normal_status(s));
        for (unsigned i = 0; i < size; i++) {
            if (addr + i == SDHC_NORMAL_STATUS || addr + i == SDHC_NORMAL_STATUS + 1) {
                buf[i] = st[addr + i - SDHC_NORMAL_STATUS];
            }
        }
    }
    return ldn_le_p(buf, size);
}

static void sdhc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    S5L8930SDIOState *s = opaque;

    for (unsigned i = 0; i < size; i++, val >>= 8) {
        hwaddr a = addr + i;
        if (a == SDHC_NORMAL_STATUS || a == SDHC_NORMAL_STATUS + 1 ||
            a == SDHC_NORMAL_STATUS + 2 || a == SDHC_NORMAL_STATUS + 3) {
            s->regs[a] &= ~(uint8_t)val;       /* status is write-1-to-clear */
        } else {
            s->regs[a] = val;
        }
    }
    sdhc_update(s);
}

static const MemoryRegionOps sdhc_ops = {
    .read = sdhc_read,
    .write = sdhc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

void s5l8930_sdio_iop_command(DeviceState *dev, uint8_t *cmd)
{
    S5L8930SDIOState *s = S5L8930_SDIO(dev);
    uint32_t op = ldl_le_p(cmd + CMD_OPCODE);
    uint32_t status = STATUS_OK;
    uint32_t resp = 0;

    switch (op) {
    case OP_INIT:
        stl_le_p(cmd + CMD_STATE, 1);
        break;
    case OP_FREE:
    case OP_RESET:
    case OP_BUS_PARAM:
        break;
    case OP_COMMAND:
        resp = ipod_touch_sdio_command(s->card, lduw_le_p(cmd + CMD6_CMD),
                                       ldl_le_p(cmd + CMD6_ARG), 0, 0, NULL, 0);
        break;
    case OP_DMA_COMMAND: {
        uint32_t nseg = MIN(ldl_le_p(cmd + CMD7_NSEG), CMD7_MAX_SEGS);
        uint32_t sg[2 * CMD7_MAX_SEGS];

        for (unsigned i = 0; i < 2 * nseg; i++) {
            sg[i] = ldl_le_p(cmd + CMD7_SEGS + 4 * i);
        }
        resp = ipod_touch_sdio_command(s->card, lduw_le_p(cmd + CMD7_CMD),
                                       ldl_le_p(cmd + CMD7_ARG),
                                       lduw_le_p(cmd + CMD7_BLKSIZE),
                                       lduw_le_p(cmd + CMD7_BLKCOUNT),
                                       sg, nseg);
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "%s: sdio opcode %u\n", __func__, op);
        status = STATUS_UNKNOWN;
        break;
    }
    if (op == OP_COMMAND || op == OP_DMA_COMMAND) {
        stl_le_p(cmd + CMD_RESPONSE, resp);
        memset(cmd + CMD_RESPONSE + 4, 0, 12);
    }
    stl_le_p(cmd + CMD_STATUS, status);
    sdhc_update(s);
}

static void s5l8930_sdio_init(Object *obj)
{
    S5L8930SDIOState *s = S5L8930_SDIO(obj);

    memory_region_init_io(&s->iomem, obj, &sdhc_ops, s, TYPE_S5L8930_SDIO,
                          SDHC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in(DEVICE(obj), sdhc_card_poke, 1);
}

static void s5l8930_sdio_realize(DeviceState *dev, Error **errp)
{
    if (!S5L8930_SDIO(dev)->card) {
        error_setg(errp, "s5l8930.sdio needs its card");
    }
}

static void s5l8930_sdio_reset(DeviceState *dev)
{
    S5L8930SDIOState *s = S5L8930_SDIO(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_irq_lower(s->irq);
}

static const VMStateDescription vmstate_s5l8930_sdio = {
    .name = TYPE_S5L8930_SDIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, S5L8930SDIOState, SDHC_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static const Property s5l8930_sdio_props[] = {
    DEFINE_PROP_LINK("card", S5L8930SDIOState, card, TYPE_IPOD_TOUCH_SDIO,
                     IPodTouchSDIOState *),
};

static void s5l8930_sdio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s5l8930_sdio_realize;
    dc->vmsd = &vmstate_s5l8930_sdio;
    device_class_set_legacy_reset(dc, s5l8930_sdio_reset);
    device_class_set_props(dc, s5l8930_sdio_props);
}

static const TypeInfo s5l8930_sdio_info = {
    .name = TYPE_S5L8930_SDIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8930SDIOState),
    .instance_init = s5l8930_sdio_init,
    .class_init = s5l8930_sdio_class_init,
};

static void s5l8930_sdio_register_types(void)
{
    type_register_static(&s5l8930_sdio_info);
}

type_init(s5l8930_sdio_register_types)
