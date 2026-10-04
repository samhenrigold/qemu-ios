#ifndef HW_ARM_MRVL8686_H
#define HW_ARM_MRVL8686_H

/*
 * Marvell 88W8686 802.11b/g SDIO card, the iPod touch 1G's Wi-Fi (N45: the
 * DT's sdio node, AppleMRVL868x-69 in iPhone OS 1.x).
 *
 * The card is register level on the SDIO side: function 1's byte registers
 * and the I/O port, the helper/firmware download handshake with the images'
 * own block CRCs, the helper's EEPROM read, and the host interrupt register.
 * The firmware images (the helper and the main program, carried inside the
 * driver) are accepted but never run: the model then answers the running
 * firmware's host-command / event / TxPD-RxPD protocol itself, with one open
 * access point behind it and its data path on "-netdev ...,id=wifi0".
 * Layouts are the ones AppleMRVL868x parses (they match Linux libertas).
 *
 * Mrvl8686Card is the whole protocol state and is plain C, so the host tests
 * (tests/ipod/test_mrvl8686.py) compile the core against it directly.
 */

#include <stdint.h>
#include <stdbool.h>

/* Function 1 byte registers. */
#define MRVL_REG_IOPORT         0x00    /* three bytes: the I/O port address */
#define MRVL_REG_CONFIG         0x03    /* bit 1: host power up (deep sleep wake) */
#define MRVL_REG_HINT_MASK      0x04
#define MRVL_REG_HINT_STATUS    0x05    /* write 0 to clear */
#define MRVL_REG_RD_BASE        0x10    /* two bytes: length the helper wants next */
#define MRVL_REG_CARD_STATUS    0x20
#define MRVL_REG_SCRATCH        0x34    /* two bytes: FIRMWARE_OK, else upload length */
#define MRVL_REG_SCRATCH_ERR    0x36    /* the helper's EEPROM read status (0xe0: error) */

#define MRVL_IOPORT             0x10000

#define MRVL_HINT_UP_LD         0x01    /* the card has a packet for the host */
#define MRVL_HINT_DN_LD         0x02    /* the card takes the next packet */

#define MRVL_STATUS_DL_RDY      0x01
#define MRVL_STATUS_UL_RDY      0x02
#define MRVL_STATUS_CIS_RDY     0x04
#define MRVL_STATUS_IO_RDY      0x08

#define MRVL_CONFIG_HOST_PWR_UP 0x02

#define MRVL_FIRMWARE_OK        0xfedc

/* The helper image (bootstrap) comes in chunks of a 4-byte length and data. */
#define MRVL_HELPER_MAX         0x4000
/* Main program: 16-byte headers {command, load address, length, CRC}, then
 * length bytes whose last four are the data's CRC. */
#define MRVL_FW_HDR_LEN         16
#define MRVL_FW_CMD_DATA        1
#define MRVL_FW_CMD_LAST        4
/* The helper's EEPROM read request, sent where a header would go. */
#define MRVL_HELPER_GETMEM      0x14
#define MRVL_EEPROM_SIZE        0x800

/* SDIO packet header on the I/O port: length (header included), type. */
#define MRVL_SDIO_HDR           4
#define MRVL_TYPE_DATA          0
#define MRVL_TYPE_CMD           1
#define MRVL_TYPE_EVENT         3

/* Host command header: command, size (header included), sequence, result. */
#define MRVL_CMD_HDR            8
#define MRVL_CMD_RESP           0x8000

#define MRVL_CMD_GET_HW_SPEC            0x0003
#define MRVL_CMD_802_11_SCAN            0x0006
#define MRVL_CMD_802_11_AUTHENTICATE    0x0011
#define MRVL_CMD_802_11_ASSOCIATE_OLD   0x0012
#define MRVL_CMD_802_11_RF_TX_POWER     0x001e
#define MRVL_CMD_802_11_RSSI            0x001f
#define MRVL_CMD_802_11_PS_MODE         0x0021
#define MRVL_CMD_802_11_DEAUTHENTICATE  0x0024
#define MRVL_CMD_802_11_DEEP_SLEEP      0x003e
#define MRVL_CMD_802_11_MAC_ADDR        0x004d
#define MRVL_CMD_802_11_ASSOCIATE       0x0050
#define MRVL_CMD_802_11_BG_SCAN_QUERY   0x006c

#define MRVL_PS_SLEEP_CONFIRM   0x0034

#define MRVL_TLV_SSID           0x0000
#define MRVL_TLV_CHANLIST       0x0101
#define MRVL_TLV_TSF            0x0113
#define MRVL_CHANLIST_ENTRY     7       /* radio, channel, scan type, min, max */

#define MRVL_EVENT_DEEP_SLEEP_AWAKE 0x10

#define MRVL_TXPD_LEN           24
#define MRVL_RXPD_LEN           20

/* The one access point: open, 802.11b/g, channel 6. */
#define MRVL_AP_SSID            "qemu-ios"
#define MRVL_AP_CHANNEL         6

#define MRVL_UPQ_SLOTS          16
#define MRVL_UPQ_BYTES          2048

typedef enum {
    MRVL_STAGE_BOOTROM,     /* waiting for the helper */
    MRVL_STAGE_HELPER,      /* helper up: EEPROM reads, main program download */
    MRVL_STAGE_FIRMWARE,    /* main program up: host commands and data */
} MrvlStage;

typedef struct Mrvl8686Card {
    uint8_t mac[6];
    uint8_t bssid[6];
    uint8_t eeprom[MRVL_EEPROM_SIZE];

    uint32_t stage;
    uint8_t config, hint_mask, hint_status, scratch_err;
    uint16_t rd_base, scratch;
    bool ul_rdy;                /* helper: an EEPROM chunk is waiting */

    uint32_t helper_bytes;
    bool fw_want_data;          /* next write is a block's data, not a header */
    uint32_t fw_bytes, fw_blocks;

    uint16_t getmem_len;        /* helper: the EEPROM chunk waiting */
    uint32_t getmem_off;

    bool asleep;                /* deep sleep */
    bool associated;
    uint32_t tx_frames, rx_frames;

    /* Packets for the host, oldest first. */
    uint8_t upq[MRVL_UPQ_SLOTS][MRVL_UPQ_BYTES];
    uint16_t upq_len[MRVL_UPQ_SLOTS];
    uint32_t upq_head, upq_count;
} Mrvl8686Card;

#ifndef MRVL8686_CORE_ONLY
#include "hw/qdev-core.h"
#include "net/net.h"

#define TYPE_MRVL8686 "mrvl8686"
OBJECT_DECLARE_SIMPLE_TYPE(Mrvl8686State, MRVL8686)

struct Mrvl8686State {
    DeviceState parent_obj;
    Mrvl8686Card card;
    qemu_irq irq;       /* the card interrupt (DAT1), level */
    NICState *nic;
    NICConf conf;
};

/* What the SDIO host calls: function 1 CMD52 and CMD53. */
uint8_t mrvl8686_readb(Mrvl8686State *s, uint32_t addr);
void mrvl8686_writeb(Mrvl8686State *s, uint32_t addr, uint8_t val);
void mrvl8686_read(Mrvl8686State *s, uint32_t addr, uint8_t *buf, uint32_t len);
void mrvl8686_write(Mrvl8686State *s, uint32_t addr, const uint8_t *buf, uint32_t len);
/* CCCR INT_PENDING, and the CCCR RES bit (a card reset). */
bool mrvl8686_irq_pending(Mrvl8686State *s);
void mrvl8686_card_reset(Mrvl8686State *s);
/* Bind the data path to "-netdev ...,id=wifi0", if there is one. */
void mrvl8686_setup_net(Mrvl8686State *s);
#endif

#endif
