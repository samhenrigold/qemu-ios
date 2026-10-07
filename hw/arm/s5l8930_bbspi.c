/*
 * The S5L8920/S5L8930 SPI block as BasebandSPI drives it (spi2, "spi,s5l8920x,baseband"):
 * the same registers as hw/arm/ipod_touch_spi.c, but run from CDMA descriptors, one IFX
 * frame per transfer, with the ios-baseband device on the other end
 * (docs/baseband/commcenter-4.2.1-3gs.md, "Static read of AppleS5L8920XBasebandSPIController").
 *
 * BasebandSPI keeps an RX chain on RXDATA (+0x20) armed ahead of any transfer
 * (seen on the N90 guest), then per frame: CTRL = 0xc, RXCNT = TXCNT = 0x200 words,
 * a TX chain on TXDATA (+0x10), CFG |= 0x40 (go; CTRL RUN is never written), MRDY up;
 * at the end MRDY down, CFG &= ~0x40, CTRL = 0. Both FIFOs are CDMA pacing sources: RXDATA has what
 * the current frame's MISO still holds (built at RUN; the IFX reply never depends
 * on the same frame's MOSI), TXDATA has room for the rest of TXCNT. MOSI goes to
 * the modem when complete, and the TX chain is released with sink_done.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/misc/ios_baseband.h"
#include "hw/arm/s5l8930.h"

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
#define CFG_GO          (1u << 6)   /* BasebandSPI starts a frame with this, not CTRL RUN */
#define CFG_RXONLY      (1u << 0)   /* no TX half: the kernel's idle, pre-armed receive frame */
#define CFG_IE_COMPLETE (1u << 21)
#define ST_COMPLETE     (1u << 22)

#define FRAME_MAX   4096
#define BBSPI_IDLE_MS 2000          /* clock an idle receive pre-arm after this (bbspi_idle) */

struct IosBbSpiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    DeviceState *modem;          /* the ios-baseband device, set by the board */
    DeviceState *cdma;           /* the CDMA engine whose channels feed the FIFOs */
    uint32_t base;               /* MMIO base: the CDMA sees the FIFOs at base + 0x10/0x20 */
    QEMUBH *kick;                /* resume stalled chains outside our own MMIO handlers */
    QEMUTimer *idle;             /* an idle receive pre-arm left waiting too long */

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

    if (s->rx_len && s->rx_pos >= s->rx_len && (!t || s->tx_len >= t) &&
        !(s->regs[R_STATUS / 4] & ST_COMPLETE)) {
        s->regs[R_STATUS / 4] |= ST_COMPLETE;
        bbspi_irq(s);
    }
}

#define TRACE(...) do { \
    if (getenv("IOS_BB_TRACE")) { \
        fprintf(stderr, "%.3f ios-bb-spi: ", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e6); \
        fprintf(stderr, __VA_ARGS__); \
    } \
} while (0)

/*
 * The modem's frame for this transfer, built when the clock first runs (SRDY up), not
 * at go: the kernel pre-arms a receive-only frame as its idle state, and the frame
 * must carry whatever the modem queued since.
 */
static void bbspi_build_miso(IosBbSpiState *s)
{
    uint32_t n = MIN(s->regs[R_RXCNT / 4] * bbspi_ws(s), FRAME_MAX);

    if (n && !s->rx_len && s->modem) {
        ios_baseband_spi_xfer(s->modem, NULL, s->rx, n);
        s->rx_len = n;
        s->rx_pos = 0;
        TRACE("frame: %u bytes, MISO hdr %02x %02x %02x %02x\n", n,
              s->rx[0], s->rx[1], s->rx[2], s->rx[3]);
    }
}

/* GO (CFG bit 6, or CTRL RUN): chains stalled on our FIFOs may move once SRDY is up. */
static void bbspi_start(IosBbSpiState *s)
{
    qemu_bh_schedule(s->kick);
}

static uint32_t bbspi_avail(void *opaque, hwaddr addr, bool to_device)
{
    IosBbSpiState *s = opaque;

    TRACE("dma asks %s (srdy %d, rx %u/%u, tx %u)\n", to_device ? "tx" : "rx",
          s->modem && ios_baseband_spi_srdy(s->modem), s->rx_pos, s->rx_len, s->tx_len);
    if (!s->modem || !ios_baseband_spi_srdy(s->modem) || !(s->regs[R_CFG / 4] & CFG_GO)) {
        return 0;                              /* the clock only runs in a frame, once SRDY is up */
    }
    if (to_device) {
        uint32_t t = MIN(s->regs[R_TXCNT / 4] * bbspi_ws(s), FRAME_MAX);

        return t > s->tx_len ? t - s->tx_len : 0;
    }
    bbspi_build_miso(s);
    return s->rx_len - s->rx_pos;
}

static uint64_t bbspi_read(void *opaque, hwaddr addr, unsigned size)
{
    IosBbSpiState *s = opaque;
    uint32_t r = 0;

    if (addr != R_RXDATA) {
        TRACE("rd %02x = %08x\n", (unsigned)addr, addr < sizeof(s->regs) ? s->regs[addr / 4] : 0);
        return addr < sizeof(s->regs) ? s->regs[addr / 4] : 0;
    }
    for (unsigned i = 0; i < bbspi_ws(s) && s->rx_pos < s->rx_len; i++) {
        r |= (uint32_t)s->rx[s->rx_pos++] << (8 * i);
    }
    if (s->rx_len && s->rx_pos == s->rx_len) {
        TRACE("MISO drained\n");
    }
    bbspi_check_done(s);
    return r;
}

static void bbspi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IosBbSpiState *s = opaque;
    uint32_t t;

    if (addr != R_TXDATA) {
        TRACE("wr %02x = %08x\n", (unsigned)addr, (uint32_t)val);
    }
    switch (addr) {
    case R_CTRL:
        if (val & (CTRL_TX_RESET | CTRL_RX_RESET)) {
            /* A new transfer: forget the last frame in both directions (a MISO
             * nobody clocked goes back to the modem's queue). */
            if (s->rx_len && !s->rx_pos && s->modem) {
                ios_baseband_spi_done(s->modem, s->rx);
            }
            s->tx_len = 0;
            s->rx_len = s->rx_pos = 0;
            s->regs[R_STATUS / 4] &= ~ST_COMPLETE;
        }
        s->regs[R_CTRL / 4] = val & CTRL_RUN;
        if (val & CTRL_RUN) {
            /*
             * RUN on a TX frame with no MRDY (the kernel queues them back to back
             * while data flows): it waits for the modem to clock it, so ask for
             * SRDY. A receive-only frame with RUN is the idle pre-arm: leave it.
             */
            if (s->modem && (s->regs[R_CFG / 4] & CFG_GO) && !(s->regs[R_CFG / 4] & CFG_RXONLY)) {
                ios_baseband_spi_request(s->modem);
            } else if (s->regs[R_CFG / 4] & CFG_GO) {
                timer_mod(s->idle, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + BBSPI_IDLE_MS);
            }
            bbspi_start(s);
        }
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
            TRACE("MOSI hdr %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                  s->tx[0], s->tx[1], s->tx[2], s->tx[3], s->tx[4], s->tx[5], s->tx[6],
                  s->tx[7], s->tx[8], s->tx[9], s->tx[10], s->tx[11], s->tx[12], s->tx[13]);
            ios_baseband_spi_xfer(s->modem, s->tx, NULL, t);
            if (s->cdma) {
                s5l8930_cdma_sink_done(s->cdma, s->base + R_TXDATA, 4);
            }
        }
        bbspi_check_done(s);
        break;
    case R_CFG:
        t = s->regs[R_CFG / 4];
        s->regs[R_CFG / 4] = val;
        if ((val & CFG_GO) && !(t & CFG_GO)) {
            bbspi_start(s);
        } else if (!(val & CFG_GO) && (t & CFG_GO) && s->modem) {
            TRACE("frame end: MISO read %u/%u, MOSI %u\n", s->rx_pos, s->rx_len, s->tx_len);
            ios_baseband_spi_done(s->modem, s->rx_len && !s->rx_pos ? s->rx : NULL);
            s->rx_len = s->rx_pos = 0;
        }
        bbspi_irq(s);
        break;
    default:
        if (addr < sizeof(s->regs)) {
            s->regs[addr / 4] = val;
        }
        if (addr == R_TXCNT || addr == R_RXCNT) {
            qemu_bh_schedule(s->kick);
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

/*
 * The CDMA reads and writes our FIFOs through the memory API; kicking it from inside
 * one of our MMIO handlers would re-enter this device, which the reentrancy guard
 * turns into silently dropped accesses (the chain then completes with zeros).
 */
static void bbspi_kick(void *opaque)
{
    IosBbSpiState *s = opaque;

    if (s->cdma) {
        s5l8930_cdma_kick(s->cdma);
    }
}

/*
 * With an idle receive frame armed the kernel does not start a transfer of its own:
 * it waits for the modem, which is also how it learns new v2 credits. If our count
 * of its credits drifts and it believes it has none, nobody moves and CommCenter
 * resets the baseband about 45 s later (seen in N90 soaks). So clock an idle
 * pre-arm now and then: an empty frame that carries a fresh grant.
 */
static void bbspi_idle(void *opaque)
{
    IosBbSpiState *s = opaque;

    if (s->modem && (s->regs[R_CFG / 4] & CFG_GO) && (s->regs[R_CFG / 4] & CFG_RXONLY) &&
        (s->regs[R_CTRL / 4] & CTRL_RUN) && !s->rx_len) {
        ios_baseband_spi_request(s->modem);
    }
}

static void bbspi_ready(void *opaque)
{
    IosBbSpiState *s = opaque;

    qemu_bh_schedule(s->kick);
}

static void bbspi_realize(DeviceState *dev, Error **errp)
{
    IosBbSpiState *s = IOS_BASEBAND_SPI(dev);

    s->kick = qemu_bh_new(bbspi_kick, s);
    s->idle = timer_new_ms(QEMU_CLOCK_VIRTUAL, bbspi_idle, s);     /* unguarded: the kick reads our FIFOs */
    if (s->modem) {
        ios_baseband_spi_set_ready(s->modem, bbspi_ready, s);
    }

    if (s->cdma) {
        s5l8930_cdma_set_source(s->cdma, s->base + R_TXDATA, R_RXDATA + 4 - R_TXDATA,
                                bbspi_avail, s);
    }
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
    DEFINE_PROP_LINK("cdma", IosBbSpiState, cdma, TYPE_S5L8930_CDMA, DeviceState *),
    DEFINE_PROP_UINT32("base", IosBbSpiState, base, 0),
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

static void bbspi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, bbspi_reset);
    dc->realize = bbspi_realize;
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
