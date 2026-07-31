#ifndef IPOD_TOUCH_SDIO_H
#define IPOD_TOUCH_SDIO_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/irq.h"

#define TYPE_IPOD_TOUCH_SDIO                "ipodtouch.sdio"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchSDIOState, IPOD_TOUCH_SDIO)

#define SDIO_CMD        0x8
#define SDIO_ARGU       0xC
#define SDIO_STATE      0x10
#define SDIO_STAC       0x14
#define SDIO_DSTA       0x18
#define SDIO_RESP0      0x20
#define SDIO_RESP1      0x24
#define SDIO_RESP2      0x28
#define SDIO_RESP3      0x2C
#define SDIO_CSR        0x34
#define SDIO_IRQ        0x38
#define SDIO_IRQMASK    0x3C
#define SDIO_BADDR      0x44
#define SDIO_BLKLEN     0x48
#define SDIO_NUMBLK     0x4C

#define CMD5_FUNC_OFFSET 28
#define CIS_OFFSET 0xC8
#define CIS_MANUFACTURER_ID 0x20
#define CIS_FUNCTION_EXTENSION 0x22
#define CIS_END 0xFF

/* The BCM4325 presents two I/O functions: 1 is the chip backplane, 2 carries
 * SDPCM traffic once firmware is running. */
#define BCM4325_FUNCTIONS 0x2
#define BCM4325_MANUFACTURER 0x4D50
#define BCM4325_PRODUCT_ID 0x4D48

/* Card Common Control Registers, in function 0's address space. */
#define CCCR_REVISION       0x00
#define CCCR_SD_REVISION    0x01
#define CCCR_IO_ENABLE      0x02
#define CCCR_IO_READY       0x03
#define CCCR_INT_ENABLE     0x04
#define CCCR_INT_PENDING    0x05
#define CCCR_BUS_CONTROL    0x07
#define CCCR_CARD_CAPS      0x08
#define CCCR_CIS_PTR        0x09  /* three bytes, little endian */
#define CCCR_HIGH_SPEED     0x13

/* Function Basic Registers: 0x100 * function. */
#define FBR_BASE(fn)        (0x100 * (fn))
#define FBR_IFACE_CODE      0x00
#define FBR_CIS_PTR         0x09

/* Where we lay the tuple chains out. Function 0's chain has to sit above the
 * FBRs, so the stock 0xC8 is not usable. */
#define CIS_COMMON_OFFSET   0x1000
#define CIS_FUNC_OFFSET(fn) (0x1000 + 0x100 * (fn))

/* CMD5 response (R4). */
#define R4_CARD_READY       (1u << 31)
#define R4_NUM_FUNCS_SHIFT  28
#define R4_IO_OCR           0x00fff000u  /* 2.0V - 3.6V */

#define SDIO_RCA            0x0001

typedef struct BCM4325FrameHeaderPacket
{
    uint16_t frame_length;
    uint16_t checksum;
} __attribute__((__packed__)) BCM4325FrameHeaderPacket;

typedef struct IPodTouchSDIOState
{
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t cmd;
    uint32_t arg;
    uint32_t state;
    uint32_t stac;
    uint32_t csr;
    uint32_t resp0;
    uint32_t resp1;
    uint32_t resp2;
    uint32_t resp3;
    uint32_t irq_reg;
    uint32_t irq_mask;
    uint32_t baddr;
    uint32_t blklen;
    uint32_t numblk;
    QEMUTimer *irq_timer;
    qemu_irq irq;
    qemu_irq irq2;
    GQueue *rx_fifo;
    bool card_present;   /* answer CMD5 so the BCM4325 driver can attach */
    uint8_t registers[0x10000];
} IPodTouchSDIOState;

#endif