#ifndef IPOD_TOUCH_SDIO_H
#define IPOD_TOUCH_SDIO_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/hw-error.h"
#include "hw/core/irq.h"
#include "net/net.h"
#include "hw/net/bcm_fullmac.h"

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
#define CIS_VERS_1 0x15
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
#define CCCR_INT_PENDING_FN1 (1 << 1)
#define CCCR_INT_PENDING_FN2 (1 << 2)
#define CCCR_IO_ABORT       0x06
#define CCCR_IO_ABORT_RES   (1 << 3)
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

/* Long enough for the guest to reach its sleep, short enough to be free. */
#define IRQ_DELAY_NS        20000

#define BACKPLANE_PAGE_BITS 12
#define BACKPLANE_PAGE_SIZE (1u << BACKPLANE_PAGE_BITS)
/* The firmware image and the core registers touch a few hundred pages; the
 * guest picks the addresses, so the store stops at 16 MiB. */
#define BACKPLANE_MAX_PAGES 4096
/* Frames the dongle has for the host and the host hasn't read. */
#define SDPCM_RX_MAX        256

/* Where the window points out of reset: the chipcommon core. */
#define CHIPCOMMON_BASE     0x18000000
/*
 * chipcommon ChipID: chip number in bits 0-15, revision in 16-19. The real
 * BCM4325 answers 0x4325 there; revision 5 is the D0 silicon both drivers
 * accept (AppleBCMWLANChipManager::withDriver, 8C148 0x80779998: 0x4325 with
 * rev 5 -> "BCMWLAN revision D0", 6 -> D1). The old value had the chip number
 * zero: 3.1.3 only checked the revision, 4.2.1 checks the number first and
 * gave up with "Unknown/Unsupported chip ID: 0x0" and never downloaded firmware.
 */
#define CHIPCOMMON_CHIPID   0x00054325
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

/* What tells one Broadcom SDIO chip from another, as the driver sees it. */
typedef struct BCMSDIOChip {
    uint16_t manfid, prodid;   /* CISTPL_MANFID */
    uint32_t chipid;           /* chipcommon ChipID: id | rev << 16 */
    uint32_t sdiod_base;       /* the SDIO device core on the backplane */
    const char *vers1[4];      /* CISTPL_VERS_1 strings; none if [0] is NULL */
    uint8_t mac[6];            /* CISTPL_FUNCE type 4 */
    uint8_t bt_mac[6];         /* Apple OTP type 3 (network byte order) */
    bool has_bt_mac;
    /*
     * Leave out function 0's common FUNCE. AppleBCMWLAN-2.60 reads every
     * FUNCE body as {type, len, data} records looking for the MAC, and the
     * common one (00 00 02 32) reads as a 50-byte record that isn't there.
     */
    bool no_common_funce;
    const char *fw_version;    /* the "ver" iovar; NULL answers zeroes */
    uint8_t functions;         /* I/O functions in the CMD5 response; 0: the BCM4325's two */
    bool no_mac_funce;         /* no CISTPL_FUNCE type 4 (a Broadcom convention) */
    uint8_t fbr_iface;         /* FBR standard interface code (7: WLAN); 0 = none */
} BCMSDIOChip;

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
    uint32_t irq_pending;  /* bits waiting for the deferred raise */
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
    int64_t fw_last_us;
    uint32_t mmio_ops;
    uint32_t irq_polls;
    bool func2_seen;
    unsigned func2_reads;
    unsigned ready_log;
    unsigned rx_log;
    uint8_t tx_seq;        /* sequence number of the next frame we hand up */
    uint8_t rx_seq;        /* last sequence number the host sent us */
    BCMFullmacState fullmac;
    GHashTable *backplane;
    uint8_t *backplane_spill;  /* every page past BACKPLANE_MAX_PAGES */

    BCMSDIOChip chip;
    const uint32_t *sg;    /* CMD53 scatter list {addr, len} while one runs */
    unsigned sg_count;
    uint8_t *hbuf;         /* or the host controller's own buffer (ipod_touch_sdio_command_buf) */
    /* A Marvell 88W8686 on the bus instead of the Broadcom dongle ("mrvl" link):
     * function 1 and the card interrupt are its. */
    struct Mrvl8686State *mrvl;
    bool card_irq_level;
    bool ap_waking;        /* the next reset is the AP's power-on from hibernate */

    uint8_t sdiod_regs[SDIOD_CORE_SIZE];
    /*
     * CMD52/CMD53 carry a 17-bit register address ((arg >> 9) & 0x1ffff), and
     * the guest really does use the top half -- 0x1000e is the misc register
     * read on the clock-enable path. This array was 0x10000 and is the LAST
     * member of the struct, so any func != 1 access above 0xffff ran off the
     * end of the QOM allocation: up to 64 KiB of heap write from a single
     * guest CMD52. The func 1 paths were already bounded, which is what gave
     * the omission away. Size it to the full address the hardware decodes
     * rather than clamping, so no legal access silently lands somewhere else.
     */
    uint8_t registers[0x20000];
} IPodTouchSDIOState;

void ipod_touch_sdio_setup_net(IPodTouchSDIOState *s);

/* Replace the default BCM4325 identity (before the guest looks). */
void ipod_touch_sdio_set_chip(IPodTouchSDIOState *s, const BCMSDIOChip *chip);

/*
 * Run one SD command for a host other than the iPod's own controller: the
 * CMD53 payload moves through sg ({addr, len} pairs, guest physical).
 * Returns response word 0.
 */
uint32_t ipod_touch_sdio_command(IPodTouchSDIOState *s, uint32_t cmd,
                                 uint32_t arg, uint32_t blklen,
                                 uint32_t numblk, const uint32_t *sg,
                                 unsigned sg_count);

/* The same, the CMD53 payload in a host controller's buffer of blklen * numblk bytes. */
uint32_t ipod_touch_sdio_command_buf(IPodTouchSDIOState *s, uint32_t cmd,
                                     uint32_t arg, uint32_t blklen,
                                     uint32_t numblk, uint8_t *buf);

/* The card's interrupt line: the dongle has something for the host. */
bool ipod_touch_sdio_card_irq(IPodTouchSDIOState *s);

#endif
