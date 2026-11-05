/* A snapshot file is untrusted input: post_load refuses indices, lengths and
 * positions that would take a device's next access outside its buffers.
 *
 * Each production post_load gets a state as the device leaves it (0) and the
 * same state with one restored field out of range (-EINVAL).
 *
 * SLICE:types include/hw/intc/pl192.h define PL192_(INT_SOURCES|DAISY_IRQ|NO_IRQ|PRIO_LEVELS)
 * SLICE:pl192 include/hw/intc/pl192.h range struct PL192State { | \n};
 * SLICE hw/intc/pl192.c fn pl192_post_load
 * SLICE:types include/hw/arm/ipod_touch_aes.h typedef IPodTouchAESState
 * SLICE hw/arm/ipod_touch_aes.c fn aes_post_load
 * SLICE:types include/hw/arm/ipod_touch_mbx.h define MBX
 * SLICE:types include/hw/arm/ipod_touch_mbx.h typedef MBX2DSurface IPodTouchMBXState
 * SLICE hw/arm/ipod_touch_mbx.c fn ipod_touch_mbx_2d_block_len ipod_touch_mbx_post_load
 * SLICE:types hw/arm/s5l8920_vxd.c define MTX_RAM_WORDS|VXD_PENDING|VXD_TARGETS
 * SLICE:types hw/arm/s5l8920_vxd.c typedef VXDTarget
 * SLICE:vxd hw/arm/s5l8920_vxd.c range struct S5L8920VXDState { | \n};
 * SLICE hw/arm/s5l8920_vxd.c fn vxd_post_load
 * SLICE:types hw/arm/s5l8930_bbspi.c define FRAME_MAX
 * SLICE:bbspi hw/arm/s5l8930_bbspi.c range struct IosBbSpiState { | \n};
 * SLICE hw/arm/s5l8930_bbspi.c fn bbspi_post_load
 * SLICE:sha1 hw/arm/s5l8930_sha1.c range struct S5L8930SHA1State { | \n};
 * SLICE hw/arm/s5l8930_sha1.c fn sha1_post_load
 * SLICE hw/misc/ios_baseband.c define IOSBB_
 * SLICE hw/misc/ios_baseband.c fn iosbb_post_load
 * CFLAGS -Wall -Werror -I$ROOT/include
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw/misc/ios_baseband_core.h"
typedef int SysBusDevice, MemoryRegion, DeviceState, QEMUTimer, QEMUBH, AddressSpace, VXDH264, qemu_irq;
typedef struct { int unused; } AES_KEY;
#define MAX(a, b) ((a) > (b) ? (a) : (b))
typedef struct PL192State PL192State;
typedef struct S5L8920VXDState S5L8920VXDState;
typedef struct IosBbSpiState IosBbSpiState;
typedef struct S5L8930SHA1State S5L8930SHA1State;
#include "types.h"
#include "pl192.h"
};
#include "vxd.h"
};
#include "bbspi.h"
};
#include "sha1.h"
};
/* The baseband device's state, as far as its post_load reads it. */
typedef struct IosBasebandState {
    IosBbCore bb;
    uint8_t out[8192];
    unsigned out_len;
    IosBbIfx ifx;
} IosBasebandState;
#define PL192(o) ((PL192State *)(o))
#define QEMU_CLOCK_VIRTUAL 0
static int64_t qemu_clock_get_ms(int clock) { return 0; }
static void pl192_update(PL192State *s) {}
static void aes_custom_load(IPodTouchAESState *s) {}
static void iosbb_vibrator_update(IosBasebandState *s) {}
#include "slice.h"

/* Load `field = bad` into a copy of a good state: the load must fail. */
#define REJECTS(fn, good, field, bad) do { \
    __typeof__(good) *t_ = malloc(sizeof(good)); \
    memcpy(t_, &(good), sizeof(good)); t_->field = (bad); \
    if (fn(t_, 1) != -EINVAL) { \
        fprintf(stderr, "FAIL: %s accepts %s = %s\n", #fn, #field, #bad); return 1; \
    } \
    free(t_); \
} while (0)
#define ACCEPTS(fn, good) do { \
    if (fn(&(good), 1) != 0) { fprintf(stderr, "FAIL: %s refuses a good state\n", #fn); return 1; } \
} while (0)

int main(void)
{
    static PL192State vic;
    vic.stack_i = PL192_PRIO_LEVELS;
    vic.current = PL192_NO_IRQ;
    vic.daisy_priority = vic.vect_priority[31] = PL192_PRIO_LEVELS - 1;
    vic.irq_stack[PL192_PRIO_LEVELS] = PL192_NO_IRQ;
    ACCEPTS(pl192_post_load, vic);
    REJECTS(pl192_post_load, vic, vect_priority[31], PL192_PRIO_LEVELS);
    REJECTS(pl192_post_load, vic, daisy_priority, PL192_PRIO_LEVELS);
    REJECTS(pl192_post_load, vic, stack_i, PL192_PRIO_LEVELS + 1);
    REJECTS(pl192_post_load, vic, stack_i, -1);
    REJECTS(pl192_post_load, vic, current, PL192_NO_IRQ + 1);
    REJECTS(pl192_post_load, vic, irq_stack[PL192_PRIO_LEVELS], PL192_NO_IRQ + 1);

    static IPodTouchAESState aes = { .in_fill = 16, .out_left = 16 };
    ACCEPTS(aes_post_load, aes);
    REJECTS(aes_post_load, aes, in_fill, 17);
    REJECTS(aes_post_load, aes, out_left, 17);

    static IPodTouchMBXState mbx = { .scale_x = 0x3ff, .scale_y = 1, .src_x = 0x1fff, .src_y = 0x1fff,
                                     .block = { MBX_2D_BLIT << 28 }, .block_len = 3 };
    ACCEPTS(ipod_touch_mbx_post_load, mbx);
    REJECTS(ipod_touch_mbx_post_load, mbx, block_len, 4);                 /* a whole blit never waits */
    REJECTS(ipod_touch_mbx_post_load, mbx, block_len, MBX_2D_MAX_BLOCK);
    REJECTS(ipod_touch_mbx_post_load, mbx, scale_x, 0);
    REJECTS(ipod_touch_mbx_post_load, mbx, scale_y, 0x400);
    REJECTS(ipod_touch_mbx_post_load, mbx, src_x, 0x2000);
    REJECTS(ipod_touch_mbx_post_load, mbx, src_y, 0x2000);

    static S5L8920VXDState vxd = { .pending = VXD_PENDING };
    ACCEPTS(vxd_post_load, vxd);
    REJECTS(vxd_post_load, vxd, pending, VXD_PENDING + 1);

    static IosBbSpiState spi = { .tx_len = FRAME_MAX, .rx_len = FRAME_MAX, .rx_pos = FRAME_MAX };
    ACCEPTS(bbspi_post_load, spi);
    REJECTS(bbspi_post_load, spi, tx_len, FRAME_MAX + 1);
    REJECTS(bbspi_post_load, spi, rx_len, FRAME_MAX + 1);
    spi.rx_len = 10;
    spi.rx_pos = 10;
    ACCEPTS(bbspi_post_load, spi);
    REJECTS(bbspi_post_load, spi, rx_pos, 11);

    static S5L8930SHA1State sha = { .fill = 60 };
    ACCEPTS(sha1_post_load, sha);
    REJECTS(sha1_post_load, sha, fill, 64);
    REJECTS(sha1_post_load, sha, fill, 62);

    static IosBasebandState bb;
    bb.out_len = sizeof(bb.out);
    bb.ifx.txq_len = sizeof(bb.ifx.txq);
    bb.bb.h5_nunacked = IOS_BB_H5_WINDOW;
    bb.bb.h5_unacked[IOS_BB_H5_WINDOW - 1].len = sizeof(bb.bb.h5_unacked[0].data);
    bb.bb.muxrx.cnt = sizeof(bb.bb.muxrx.buf);
    bb.bb.ch[IOS_BB_MAX_CH - 1].len = sizeof(bb.bb.ch[0].line) - 1;
    bb.bb.call_ch = IOS_BB_MAX_CH - 1;
    bb.bb.gps.ch = -1;
    memset(bb.bb.operator_long, 'A', sizeof(bb.bb.operator_long) - 1);    /* full, its NUL last */
    memset(bb.bb.calls[0].number, '1', sizeof(bb.bb.calls[0].number) - 1);
    memset(bb.bb.store[0].pdu, '0', sizeof(bb.bb.store[0].pdu) - 1);
    ACCEPTS(iosbb_post_load, bb);
    REJECTS(iosbb_post_load, bb, out_len, sizeof(bb.out) + 1);
    REJECTS(iosbb_post_load, bb, ifx.txq_len, sizeof(bb.ifx.txq) + 1);
    REJECTS(iosbb_post_load, bb, bb.h5_rxlen, sizeof(bb.bb.h5_rx) + 1);
    REJECTS(iosbb_post_load, bb, bb.h5_nunacked, IOS_BB_H5_WINDOW + 1);
    REJECTS(iosbb_post_load, bb, bb.h5_unacked[IOS_BB_H5_WINDOW - 1].len, sizeof(bb.bb.h5_unacked[0].data) + 1);
    REJECTS(iosbb_post_load, bb, bb.muxrx.cnt, sizeof(bb.bb.muxrx.buf) + 1);
    REJECTS(iosbb_post_load, bb, bb.ch[IOS_BB_MAX_CH - 1].len, sizeof(bb.bb.ch[0].line));
    REJECTS(iosbb_post_load, bb, bb.ch[0].line[sizeof(bb.bb.ch[0].line) - 1], 'a');
    REJECTS(iosbb_post_load, bb, bb.call_ch, IOS_BB_MAX_CH);
    REJECTS(iosbb_post_load, bb, bb.creg_ch, -1);
    REJECTS(iosbb_post_load, bb, bb.gps.ch, -2);
    REJECTS(iosbb_post_load, bb, bb.operator_long[sizeof(bb.bb.operator_long) - 1], 'A');
    REJECTS(iosbb_post_load, bb, bb.calls[0].number[sizeof(bb.bb.calls[0].number) - 1], '1');
    REJECTS(iosbb_post_load, bb, bb.store[0].pdu[sizeof(bb.bb.store[0].pdu) - 1], '0');

    puts("PASS: pl192, AES, MBX, VXD, baseband SPI, SHA1 and baseband post_load refuse out-of-range state");
    return 0;
}
