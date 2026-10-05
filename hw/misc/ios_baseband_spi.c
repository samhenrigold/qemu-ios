/*
 * The S5L8920/S5L8930 SPI block as BasebandSPI drives it (spi2, "spi,s5l8920x,baseband"):
 * the same registers as hw/arm/ipod_touch_spi.c, but run from CDMA descriptors, one IFX
 * frame per transfer, with the ios-baseband device on the other end
 * (docs/baseband/commcenter-4.2.1-3gs.md, "Static read of AppleS5L8920XBasebandSPIController").
 *
 * The kext programs RXCNT/TXCNT in words, points CDMA at TXDATA (+0x10) and RXDATA
 * (+0x20) and sets RUN. The CDMA model runs a chain to completion inside its go write,
 * so whichever channel starts first must already see the whole frame. The IFX reply
 * does not depend on what the AP sends in the same frame, so MISO is built on the
 * first RXDATA read of a transfer and MOSI is handed over once TXCNT words have arrived.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/misc/ios_baseband.h"

OBJECT_DECLARE_SIMPLE_TYPE(IosBbSpiState, IOS_BASEBAND_SPI)

#define R_CTRL      0x00
#define R_CFG       0x04
#define R_STATUS    0x08
#define R_RXCNT     0x34
#define R_TXDATA    0x10
#define R_RXDATA    0x20
#define R_TXCNT     0x4c

#define CTRL_RUN        (1u << 0)
#define CTRL_TX_RESET   (1u << 2)
#define CTRL_RX_RESET   (1u << 3)
#define CFG_IE_COMPLETE (1u << 21)
#define ST_COMPLETE     (1u << 22)

#define FRAME_MAX   4096

struct IosBbSpiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    DeviceState *modem;          /* the ios-baseband device, set by the board */

    uint32_t regs[0x100 / 4];
    uint8_t tx[FRAME_MAX];
    uint32_t tx_len;             /* MOSI bytes collected this transfer */
    uint8_t rx[FRAME_MAX];
    uint32_t rx_len, rx_pos;     /* MISO built for this transfer, bytes read */
};

static unsigned bbspi_ws(IosBbSpiState *s)
{
    return 1u << MIN((s->regs[R_CFG / 4] >> 15) & 3, 2);
}

static void bbspi_irq(IosBbSpiState *s)
{
    qemu_set_irq(s->irq, (s->regs[R_CFG / 4] & CFG_IE_COMPLETE) &&
                         (s->regs[R_STATUS / 4] & ST_COMPLETE));
}

/* Both halves done (or this transfer has no TX half): the frame is complete. */
static void bbspi_check_done(IosBbSpiState *s)
{
    uint32_t t = s->regs[R_TXCNT / 4] * bbspi_ws(s);

    if (s->rx_len && s->rx_pos >= s->rx_len && (!t || s->tx_len >= t)) {
        s->regs[R_STATUS / 4] |= ST_COMPLETE;
        bbspi_irq(s);
    }
}

static uint64_t bbspi_read(void *opaque, hwaddr addr, unsigned size)
{
    IosBbSpiState *s = opaque;
    uint32_t r = 0;

    if (addr != R_RXDATA) {
        return addr < sizeof(s->regs) ? s->regs[addr / 4] : 0;
    }
    if (!s->rx_len) {
        uint32_t n = MIN(s->regs[R_RXCNT / 4] * bbspi_ws(s), FRAME_MAX);

        if (!n || !s->modem) {
            return 0;
        }
        ios_baseband_spi_xfer(s->modem, NULL, s->rx, n);
        s->rx_len = n;
        s->rx_pos = 0;
    }
    for (unsigned i = 0; i < bbspi_ws(s) && s->rx_pos < s->rx_len; i++) {
        r |= (uint32_t)s->rx[s->rx_pos++] << (8 * i);
    }
    bbspi_check_done(s);
    return r;
}

static void bbspi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IosBbSpiState *s = opaque;
    uint32_t t;

    switch (addr) {
    case R_CTRL:
        if (val & (CTRL_TX_RESET | CTRL_RX_RESET)) {
            /* A new transfer: forget the last frame in both directions. */
            s->tx_len = 0;
            s->rx_len = s->rx_pos = 0;
        }
        s->regs[R_CTRL / 4] = val & CTRL_RUN;
        break;
    case R_STATUS:
        s->regs[R_STATUS / 4] &= ~val;
        bbspi_irq(s);
        break;
    case R_TXDATA:
        t = MIN(s->regs[R_TXCNT / 4] * bbspi_ws(s), FRAME_MAX);
        for (unsigned i = 0; i < bbspi_ws(s) && s->tx_len < t; i++) {
            s->tx[s->tx_len++] = val >> (8 * i);
        }
        if (t && s->tx_len == t && s->modem) {
            ios_baseband_spi_xfer(s->modem, s->tx, NULL, t);
        }
        bbspi_check_done(s);
        break;
    case R_CFG:
        s->regs[R_CFG / 4] = val;
        bbspi_irq(s);
        break;
    default:
        if (addr < sizeof(s->regs)) {
            s->regs[addr / 4] = val;
        }
        break;
    }
}

static const MemoryRegionOps bbspi_ops = {
    .read = bbspi_read,
    .write = bbspi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void bbspi_reset(DeviceState *dev)
{
    IosBbSpiState *s = IOS_BASEBAND_SPI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->tx_len = s->rx_len = s->rx_pos = 0;
    qemu_set_irq(s->irq, 0);
}

static void bbspi_init(Object *obj)
{
    IosBbSpiState *s = IOS_BASEBAND_SPI(obj);

    memory_region_init_io(&s->iomem, obj, &bbspi_ops, s, "ios-baseband-spi", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property bbspi_props[] = {
    DEFINE_PROP_LINK("modem", IosBbSpiState, modem, TYPE_IOS_BASEBAND, DeviceState *),
};

static const VMStateDescription bbspi_vmstate = {
    .name = "ios-baseband-spi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IosBbSpiState, 0x100 / 4),
        VMSTATE_UINT8_ARRAY(tx, IosBbSpiState, FRAME_MAX),
        VMSTATE_UINT32(tx_len, IosBbSpiState),
        VMSTATE_UINT8_ARRAY(rx, IosBbSpiState, FRAME_MAX),
        VMSTATE_UINT32(rx_len, IosBbSpiState),
        VMSTATE_UINT32(rx_pos, IosBbSpiState),
        VMSTATE_END_OF_LIST()
    }
};

static void bbspi_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, bbspi_reset);
    device_class_set_props(dc, bbspi_props);
    dc->vmsd = &bbspi_vmstate;
    dc->desc = "S5L89xx baseband SPI (spi2) in front of ios-baseband";
}

static const TypeInfo bbspi_info = {
    .name = TYPE_IOS_BASEBAND_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IosBbSpiState),
    .instance_init = bbspi_init,
    .class_init = bbspi_class_init,
};

static void bbspi_register(void)
{
    type_register_static(&bbspi_info);
}

type_init(bbspi_register)
