/*
 * Fake cellular baseband: the QEMU device side. The transport-independent core
 * (and its API) is in ios_baseband_core.h.
 */
#ifndef HW_MISC_IOS_BASEBAND_H
#define HW_MISC_IOS_BASEBAND_H

#include "hw/misc/ios_baseband_core.h"
#include "chardev/char.h"
#include "net/net.h"

#define TYPE_IOS_BASEBAND "ios-baseband"
#define TYPE_IOS_BASEBAND_SPI "ios-baseband-spi"

OBJECT_DECLARE_SIMPLE_TYPE(IosBasebandState, IOS_BASEBAND)

struct IosBasebandState {
    DeviceState parent_obj;

    IosBbCore bb;
    Chardev *chr;                 /* created at realize; hand to the UART */
    QEMUTimer *timer;
    uint8_t out[8192];            /* bytes waiting for the UART */
    unsigned out_len;

    /* SPI transport (3GS/iPhone 4): ifx-version 1 or 2; 0 = UART chardev (M68). */
    int ifx_version;
    int ifx_max_data;
    IosBbIfx ifx;
    qemu_irq srdy;                /* out: "clock me" (edge to the AP's GPIO) */
    bool srdy_level;
    bool mrdy_level;              /* in: the AP wants a transfer */
    void (*spi_ready)(void *opaque);  /* SRDY rose: the controller may move the frame */
    void *spi_ready_opaque;

    /* Cellular data: raw IP on the PDP DLCI <-> Ethernet on -netdev id "cell0". */
    NICConf conf;
    NICState *nic;
};

/* The chardev to pass to the UART model as its backend. */
Chardev *ios_baseband_chardev(DeviceState *dev);

/*
 * SPI transport: one full-duplex IFX frame of n bytes, called by the baseband SPI
 * controller once the AP has clocked it (MRDY is the named GPIO in "mrdy", SRDY the
 * named GPIO out "srdy"). mosi or miso may be NULL to run one half (see
 * ios_bb_ifx_xfer). The frame may only move while SRDY is high; the controller
 * learns when it rises through the ready callback.
 */
void ios_baseband_spi_xfer(DeviceState *dev, const uint8_t *mosi, uint8_t *miso, size_t n);
bool ios_baseband_spi_srdy(DeviceState *dev);
void ios_baseband_spi_done(DeviceState *dev);
void ios_baseband_spi_set_ready(DeviceState *dev, void (*cb)(void *), void *opaque);

#endif
