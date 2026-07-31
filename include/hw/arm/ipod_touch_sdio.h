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

/*
 * Function 1's address space. Below 0x10000 is a window onto the chip
 * backplane; from 0x10000 up are the SDIO device core's own registers,
 * including the three bytes that position that window.
 */
#define SDIOD_CORE_BASE     0x10000
#define SDIOD_CORE_SIZE     0x10000
#define SB_OFFSET_MASK      0x7fff   /* bit 15 selects 32-bit access, not address */
#define SBSDIO_SBADDRLOW    0x1000a
#define SBSDIO_SBADDRHIGH   0x1000c

#define BACKPLANE_PAGE_BITS 12
#define BACKPLANE_PAGE_SIZE (1u << BACKPLANE_PAGE_BITS)

/* Where the window points out of reset: the chipcommon core. */
#define CHIPCOMMON_BASE     0x18000000
#define CHIPCOMMON_CHIPID   0x00050000  /* the driver reads this as revision D0 */
#define CHIPCOMMON_CORECTL  0x18000634  /* poked just before the core is started */

/*
 * The SDIO device core. AppleBCM4325::initHardware writes 0xe0 to 0x18002024,
 * which pins the core at 0x18002000 and confirms the register layout is the
 * same one brcmfmac documents.
 */
#define SDPCM_CORE_BASE          0x18002000
#define SDPCM_CORE_SIZE          0x100
#define SDPCM_INTSTATUS          0x20
#define SDPCM_HOSTINTMASK        0x24
#define SDPCM_TOSBMAILBOX        0x40
#define SDPCM_TOHOSTMAILBOX      0x44
#define SDPCM_TOSBMAILBOXDATA    0x48
#define SDPCM_TOHOSTMAILBOXDATA  0x4c

/* intstatus bits the host mailbox uses; 0xe0 is the set the driver enables. */
#define I_HMB_FC_CHANGE     (1 << 5)
#define I_HMB_FRAME_IND     (1 << 6)
#define I_HMB_HOST_INT      (1 << 7)

/* tosbmailbox: what the host says back. */
#define SMB_INT_ACK         (1 << 1)

/* tohostmailboxdata: how the dongle announces itself. */
#define HMB_DATA_DEVREADY   0x2
#define HMB_DATA_FWREADY    0x8
#define HMB_DATA_VERSION_SHIFT 16
#define SDPCM_PROT_VERSION  4

/* SDPCM framing on function 2. */
#define SDPCM_HWHDR_LEN     4
#define SDPCM_SWHDR_LEN     8
#define SDPCM_HDRLEN        (SDPCM_HWHDR_LEN + SDPCM_SWHDR_LEN)
#define SDPCM_CONTROL_CHANNEL 0
#define SDPCM_EVENT_CHANNEL   1
#define SDPCM_DATA_CHANNEL    2
#define SDPCM_CHANNEL_MASK    0x0f

/* The CDC control header that rides on channel 0. */
#define CDC_HDRLEN          16
#define CDC_DCMD_ERROR      0x01
#define CDC_DCMD_SET        0x02

typedef struct BCM4325FrameHeaderPacket
{
    uint16_t frame_length;
    uint16_t checksum;
} __attribute__((__packed__)) BCM4325FrameHeaderPacket;

/* One complete SDPCM frame waiting to be read out on function 2. */
typedef struct SDPCMFrame
{
    uint8_t *data;
    uint32_t len;
    uint32_t read_off;   /* how much of it the host has collected */
} SDPCMFrame;

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
    uint32_t sb_window;  /* backplane address bits 8 and up */
    uint32_t fw_bytes;   /* bytes pushed over function 1, for progress logging */
    uint32_t fw_bytes_logged;
    bool func2_seen;
    unsigned func2_reads;
    unsigned ready_log;
    bool dongle_started;   /* the driver has taken the core out of reset */
    uint8_t tx_seq;        /* sequence number of the next frame we hand up */
    uint8_t rx_seq;        /* last sequence number the host sent us */
    GHashTable *backplane;
    uint8_t sdiod_regs[SDIOD_CORE_SIZE];
    uint8_t registers[0x10000];
} IPodTouchSDIOState;

#endif