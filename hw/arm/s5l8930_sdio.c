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
 * 0x26.
 *
 * With the IOP core (iop-core=on) the firmware's sdiodrv drives the SDHC
 * itself, a standard SDHCI 2.0 host: software reset (0x2f, self-clearing),
 * clock control (0x2c: internal clock stable follows enable), block size and
 * count, argument, transfer mode and command (a write to 0x0e issues it),
 * responses (0x10), present state (0x24), normal/error status with their
 * status and signal enables. Data moves through the buffer data port (0x20)
 * by the CDMA (fw 8C148 sdiodrv_performDMA 0xad04: transfer mode 3/0x23,
 * |0x10 for reads, then a channel on base + 0x20): a read's blocks are in the
 * FIFO when the command completes and transfer complete follows the last
 * word taken; a write's are collected there and go to the card with the last
 * word. The command's response does not depend on the data (the card's R5).
 * With the HLE the ring commands are run at the SD-command level instead
 * (s5l8930_sdio_iop_command).
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
#define SDHC_BLKSIZE        0x04
#define SDHC_BLKCOUNT       0x06
#define SDHC_ARG            0x08
#define SDHC_XFER_MODE      0x0C
#define SDHC_COMMAND        0x0E
#define SDHC_RESPONSE       0x10
#define SDHC_DATA           0x20
#define SDHC_PRESENT        0x24
#define SDHC_CLOCK          0x2C
#define SDHC_RESET          0x2F
#define SDHC_NORMAL_STATUS  0x30
#define SDHC_ERROR_STATUS   0x32
#define SDHC_NORMAL_ENABLE  0x34
#define SDHC_NORMAL_SIGNAL  0x38
#define SDHC_CAPS           0x40
#define SDHC_VERSION        0xFE
#define SDHC_CMD_COMPLETE   0x0001
#define SDHC_XFER_COMPLETE  0x0002
#define SDHC_BUF_WRITE      0x0010
#define SDHC_BUF_READ       0x0020
#define SDHC_CARD_INT       0x0100
#define SDHC_ERROR_INT      0x8000
#define XFER_BLKCOUNT       (1u << 1)
#define XFER_READ           (1u << 4)
#define CMD_DATA            (1u << 5)
#define CLOCK_INT_EN        (1u << 0)
#define CLOCK_INT_STABLE    (1u << 1)
/* card inserted, stable, detect pin, not write protected; DAT[3:0] and CMD high */
#define PRESENT_IDLE        0x01ff0000u
#define PRESENT_BUF_WRITE   (1u << 10)
#define PRESENT_BUF_READ    (1u << 11)
/* 50 MHz base clock, 512-byte blocks, high speed, SDMA, 3.3 V */
#define SDHC_CAPS_VALUE     0x01603200u
#define SDHC_FIFO_MAX       (2048 * 511)    /* the card's own CMD53 cap */

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
    /* A CMD53's data in flight at the buffer data port: len bytes, pos done. */
    uint8_t *fifo;
    uint32_t fifo_len, fifo_pos;
    bool fifo_write;
    uint16_t cmd;
    uint32_t arg, blksize, blkcount;
};

static uint16_t sdhc_normal_status(S5L8930SDIOState *s)
{
    uint16_t st = lduw_le_p(&s->regs[SDHC_NORMAL_STATUS]);

    /* The card interrupt is a level from DAT1, never latched. */
    if (s->card && ipod_touch_sdio_card_irq(s->card) &&
        (lduw_le_p(&s->regs[SDHC_NORMAL_ENABLE]) & SDHC_CARD_INT)) {
        st |= SDHC_CARD_INT;
    }
    if (lduw_le_p(&s->regs[SDHC_ERROR_STATUS])) {
        st |= SDHC_ERROR_INT;
    }
    return st;
}

static void sdhc_update(S5L8930SDIOState *s)
{
    qemu_set_irq(s->irq, !!(sdhc_normal_status(s) &
                            lduw_le_p(&s->regs[SDHC_NORMAL_SIGNAL])));
}

/* Latch normal-status bits, as far as the status enable lets them. */
static void sdhc_raise(S5L8930SDIOState *s, uint16_t bits)
{
    uint16_t st = lduw_le_p(&s->regs[SDHC_NORMAL_STATUS]);

    stw_le_p(&s->regs[SDHC_NORMAL_STATUS], st | (bits & lduw_le_p(&s->regs[SDHC_NORMAL_ENABLE])));
}

/* The card's interrupt output changed: re-derive ours. */
static void sdhc_card_poke(void *opaque, int n, int level)
{
    sdhc_update(opaque);
}

static void sdhc_fifo_clear(S5L8930SDIOState *s)
{
    g_free(s->fifo);
    s->fifo = NULL;
    s->fifo_len = s->fifo_pos = 0;
}

/* A write to the command register: the card answers at once. */
static void sdhc_issue(S5L8930SDIOState *s)
{
    uint16_t mode = lduw_le_p(&s->regs[SDHC_XFER_MODE]);
    uint32_t resp;

    s->cmd = lduw_le_p(&s->regs[SDHC_COMMAND]);
    s->arg = ldl_le_p(&s->regs[SDHC_ARG]);
    s->blksize = lduw_le_p(&s->regs[SDHC_BLKSIZE]) & 0xfff;
    s->blkcount = (mode & XFER_BLKCOUNT) ? lduw_le_p(&s->regs[SDHC_BLKCOUNT]) : 1;
    sdhc_fifo_clear(s);
    if ((s->cmd & CMD_DATA) && s->blksize * s->blkcount <= SDHC_FIFO_MAX) {
        s->fifo_len = s->blksize * s->blkcount;
        s->fifo = g_malloc0(s->fifo_len);
        s->fifo_write = !(mode & XFER_READ);
    }
    if (s->fifo && !s->fifo_write) {
        resp = ipod_touch_sdio_command_buf(s->card, s->cmd >> 8, s->arg, s->blksize, s->blkcount, s->fifo);
    } else if (s->fifo) {
        resp = 0;                       /* R5 before the data, as the card's CMD53 */
    } else {
        resp = ipod_touch_sdio_command(s->card, s->cmd >> 8, s->arg, 0, 0, NULL, 0);
    }
    stl_le_p(&s->regs[SDHC_RESPONSE], resp);
    memset(&s->regs[SDHC_RESPONSE + 4], 0, 12);
    sdhc_raise(s, SDHC_CMD_COMPLETE | (s->fifo ? (s->fifo_write ? SDHC_BUF_WRITE : SDHC_BUF_READ) : 0));
}

/* The buffer data port, as the CDMA (or a PIO loop) moves it. */
static uint32_t sdhc_data(S5L8930SDIOState *s, uint32_t val, unsigned size, bool write)
{
    uint32_t v = 0;

    if (!s->fifo || write != s->fifo_write) {
        return 0;
    }
    for (unsigned i = 0; i < size && s->fifo_pos < s->fifo_len; i++) {
        if (write) {
            s->fifo[s->fifo_pos++] = val >> (8 * i);
        } else {
            v |= s->fifo[s->fifo_pos++] << (8 * i);
        }
    }
    if (s->fifo_pos == s->fifo_len) {
        if (write) {
            ipod_touch_sdio_command_buf(s->card, s->cmd >> 8, s->arg, s->blksize, s->blkcount, s->fifo);
        }
        sdhc_fifo_clear(s);
        sdhc_raise(s, SDHC_XFER_COMPLETE);
        sdhc_update(s);
    }
    return v;
}

static uint32_t sdhc_present(S5L8930SDIOState *s)
{
    uint32_t v = PRESENT_IDLE;

    if (s->fifo) {
        v |= s->fifo_write ? PRESENT_BUF_WRITE : PRESENT_BUF_READ;
    }
    return v;
}

static void sdhc_soft_reset(S5L8930SDIOState *s, uint8_t what)
{
    if (what & 1) {                     /* all: everything but the card */
        memset(s->regs, 0, sizeof(s->regs));
        stl_le_p(&s->regs[SDHC_CAPS], SDHC_CAPS_VALUE);
        stw_le_p(&s->regs[SDHC_VERSION], 0x0001);   /* spec 2.00 */
    }
    if (what & 7) {                     /* the CMD and DAT lines: the transfer goes */
        sdhc_fifo_clear(s);
    }
}

static uint64_t sdhc_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8930SDIOState *s = opaque;
    uint8_t buf[4], st[2], pr[4];

    if (addr == SDHC_DATA) {
        return sdhc_data(s, 0, size, false);
    }
    memcpy(buf, &s->regs[addr], size);
    stw_le_p(st, sdhc_normal_status(s));
    stl_le_p(pr, sdhc_present(s));
    for (unsigned i = 0; i < size; i++) {
        hwaddr a = addr + i;
        if (a == SDHC_NORMAL_STATUS || a == SDHC_NORMAL_STATUS + 1) {
            buf[i] = st[a - SDHC_NORMAL_STATUS];
        } else if (a >= SDHC_PRESENT && a < SDHC_PRESENT + 4) {
            buf[i] = pr[a - SDHC_PRESENT];
        } else if (a == SDHC_CLOCK) {
            buf[i] = (buf[i] & ~CLOCK_INT_STABLE) | ((buf[i] & CLOCK_INT_EN) ? CLOCK_INT_STABLE : 0);
        }
    }
    return ldn_le_p(buf, size);
}

static void sdhc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    S5L8930SDIOState *s = opaque;
    bool issue = false;

    if (addr == SDHC_DATA) {
        sdhc_data(s, val, size, true);
        return;
    }
    for (unsigned i = 0; i < size; i++, val >>= 8) {
        hwaddr a = addr + i;
        if (a >= SDHC_NORMAL_STATUS && a < SDHC_NORMAL_STATUS + 4) {
            s->regs[a] &= ~(uint8_t)val;       /* status is write-1-to-clear */
        } else if (a == SDHC_RESET) {
            sdhc_soft_reset(s, val);            /* self-clearing: done at once */
        } else if (a >= SDHC_PRESENT && a < SDHC_PRESENT + 4) {
            /* read-only */
        } else {
            s->regs[a] = val;
            issue |= a == SDHC_COMMAND + 1;     /* the high byte of the command issues it */
        }
    }
    if (issue && s->card) {
        sdhc_issue(s);
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
            if (!(i & 1)) {
                sg[i] = s5l8930_iop_pa(sg[i]);   /* iOS 4.3+ passes IOP-window addresses */
            }
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

    sdhc_soft_reset(s, 1);
    qemu_irq_lower(s->irq);
}

static const VMStateDescription vmstate_s5l8930_sdio = {
    .name = TYPE_S5L8930_SDIO,
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, S5L8930SDIOState, SDHC_SIZE),
        /* 2: a CMD53 waiting at the data port (between the command and its CDMA chain) */
        VMSTATE_UINT32_V(fifo_len, S5L8930SDIOState, 2),
        VMSTATE_VBUFFER_ALLOC_UINT32(fifo, S5L8930SDIOState, 2, NULL, fifo_len),
        VMSTATE_UINT32_V(fifo_pos, S5L8930SDIOState, 2),
        VMSTATE_BOOL_V(fifo_write, S5L8930SDIOState, 2),
        VMSTATE_UINT16_V(cmd, S5L8930SDIOState, 2),
        VMSTATE_UINT32_V(arg, S5L8930SDIOState, 2),
        VMSTATE_UINT32_V(blksize, S5L8930SDIOState, 2),
        VMSTATE_UINT32_V(blkcount, S5L8930SDIOState, 2),
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
