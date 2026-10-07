#ifndef IPOD_TOUCH_MIPI_DSI_H
#define IPOD_TOUCH_MIPI_DSI_H

#include <math.h>
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"

#define TYPE_IPOD_TOUCH_MIPI_DSI                "ipodtouch.mipidsi"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchMIPIDSIState, IPOD_TOUCH_MIPI_DSI)

#define REG_STATUS   0x00
#define REG_SWRST    0x04
#define REG_ESCMODE  0x14
#define REG_CLKCTRL  0x08
#define REG_INTSRC   0x2C
#define REG_PKTHDR   0x34
#define REG_RXFIFO   0x3C
#define REG_FIFOCTRL 0x44

#define DSIM_RSP_LONG_READ 0x1A
#define rDSIM_FIFOCTRL_EmptyHSfr 0x400000
#define rDSIM_STATUS_StopStateClk 0x100   /* clock lane in LP stop state (no HS clock) */
#define rDSIM_STATUS_TxReadyHsClk 0x400
#define rDSIM_STATUS_SwRstRelease 0x00100000
#define rDSIM_INTSRC_RxDatDone    0x00040000

// CLKCTRL bit 31 requests the high-speed byte clock. STATUS.TxReadyHsClk
// follows it: the driver sets it and waits for ready on panel bring-up, then
// clears it and waits for the bit to drop when shutting the panel down.
#define rDSIM_CLKCTRL_TxRequestHsClk 0x80000000

typedef struct IPodTouchMIPIDSIState
{
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    bool swrst_released; /* software reset completed; read-only STATUS bit20 */
    bool ulps_clock, ulps_data; /* independently requested D-PHY low-power states */
    uint32_t escmode;
    uint32_t lanes;   /* data lanes: 2 on the iPod, 4 on K48 */
    uint32_t panel_id;      /* the panel's register-B1 reply, little-endian bytes */
    uint32_t panel_id_len;  /* its byte count (1-4) */
    bool hs_clock_at_reset; /* the machine boots a kernel with no boot stage
                             * programming the DSIM (ipad1 kboot=): start with
                             * the HS clock iBoot would have left running */
    qemu_irq irq;
    uint32_t pkthdr_reg;
    uint32_t clkctrl;
    uint32_t cmd_pending;   /* Legacy VMState wire slot only; no live hardware effect */
    bool return_panel_id; /* legacy migration field */
    uint32_t rx_fifo[16];
    uint32_t rx_head;
    uint32_t rx_count;
    uint32_t intsrc;
} IPodTouchMIPIDSIState;

#endif