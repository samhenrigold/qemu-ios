#!/usr/bin/env python3
"""The 88W8686 card (hw/arm/mrvl8686.c) as AppleMRVL868x-69 drives it: helper and firmware
block download with its CRCs (a bad CRC sets the error bit), FIRMWARE_OK, write-0-to-clear
on the host interrupt status, a scan that hears the access point only on its channel, and
the association answered as 0x8012. The model's core section is compiled as it is."""
from pathlib import Path
import re, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/mrvl8686.c').read_text()
header = (root / 'include/hw/arm/mrvl8686.h').read_text()
core = source[source.index('/* mrvl8686 core: begin'):source.index('/* mrvl8686 core: end */')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define mrvl_trace(...) ((void)0)
static uint16_t lduw_le_p(const void *v) { const uint8_t *p = v; return p[0] | p[1] << 8; }
static uint32_t ldl_le_p(const void *v) { return lduw_le_p(v) | (uint32_t)lduw_le_p((const uint8_t *)v + 2) << 16; }
static uint32_t ldl_be_p(const void *v) { const uint8_t *p = v; return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void stw_le_p(void *v, uint16_t x) { uint8_t *p = v; p[0] = x; p[1] = x >> 8; }
static void stl_le_p(void *v, uint32_t x) { stw_le_p(v, x); stw_le_p((uint8_t *)v + 2, x >> 16); }
''' + '\n'.join(re.findall(r'^#define MRVL_.*$', header, re.M)) + '\n' \
    + re.search(r'typedef enum \{.*?\} MrvlStage;', header, re.S)[0] + '\n' \
    + re.search(r'typedef struct Mrvl8686Card \{.*?\} Mrvl8686Card;', header, re.S)[0] + r'''
static void mrvl_card_send(Mrvl8686Card *c, const uint8_t *f, uint32_t n);
''' + core + r'''
static void mrvl_card_send(Mrvl8686Card *c, const uint8_t *f, uint32_t n) {}
static Mrvl8686Card card, *c = &card;
static const uint8_t AP[6] = {2, 0, 0x5e, 0x10, 0, 1};

static uint16_t reg16(uint32_t r) { return mrvl_card_readb(c, r) | mrvl_card_readb(c, r + 1) << 8; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* One firmware block header, CRC over its first 12 bytes. */
static void header(uint8_t *h, uint32_t cmd, uint32_t len)
{
    memset(h, 0, 16);
    stl_le_p(h, cmd); stl_le_p(h + 4, 0xc0001000); stl_le_p(h + 8, len);
    put_be32(h + 12, mrvl_crc32(h, 12));
}

/* A host command on the I/O port; returns the response the card queues (after UP_LD). */
static const uint8_t *command(uint16_t code, const uint8_t *body, uint16_t blen)
{
    static uint8_t p[320], r[640];
    memset(p, 0, sizeof(p));
    stw_le_p(p, 12 + blen); stw_le_p(p + 2, MRVL_TYPE_CMD);
    stw_le_p(p + 4, code); stw_le_p(p + 6, 8 + blen);
    memcpy(p + 12, body, blen);
    mrvl_card_write(c, p, sizeof(p));
    uint8_t st = mrvl_card_readb(c, MRVL_REG_HINT_STATUS);
    assert(st & MRVL_HINT_UP_LD);
    mrvl_card_writeb(c, MRVL_REG_HINT_STATUS, st & ~(MRVL_HINT_UP_LD | MRVL_HINT_DN_LD));
    assert(!(mrvl_card_readb(c, MRVL_REG_HINT_STATUS) & 3));          /* write 0 clears */
    uint16_t n = reg16(MRVL_REG_SCRATCH);
    mrvl_card_read(c, r, sizeof(r));
    assert(lduw_le_p(r) == n && lduw_le_p(r + 2) == MRVL_TYPE_CMD);
    return r + 4;
}

/* A scan request: BSS type, BSSID, then a channel list TLV with one channel. */
static const uint8_t *scan(uint8_t channel)
{
    uint8_t b[7 + 4 + MRVL_CHANLIST_ENTRY] = {3};
    stw_le_p(b + 7, MRVL_TLV_CHANLIST); stw_le_p(b + 9, MRVL_CHANLIST_ENTRY);
    b[12] = channel; b[16] = 110;
    return command(MRVL_CMD_802_11_SCAN, b, sizeof(b));
}

int main(void)
{
    uint8_t buf[0x400], h[16];
    static const uint8_t UNIT[6] = {0x02, 0x22, 0x0b, 0x10, 0x77, 0x02}, NONE[6];
    static const uint8_t PLACEHOLDER[6] = {0x00, 0x1b, 0x63, 0x45, 0x1e, 0x01};
    memcpy(c->bssid, AP, 6);
    mrvl_card_set_mac(c, NONE);                    /* no machine wifi-mac */
    assert(!memcmp(c->mac, PLACEHOLDER, 6));
    mrvl_card_set_mac(c, UNIT);                    /* the unit identity's */
    mrvl_card_reset(c);
    assert(mrvl_card_readb(c, MRVL_REG_CARD_STATUS) & MRVL_STATUS_IO_RDY);

    /* The helper: {length, data} chunks ending with a zero length. */
    memset(buf, 0, sizeof(buf)); stl_le_p(buf, 60);
    mrvl_card_write(c, buf, 64);
    memset(buf, 0, 64);
    mrvl_card_write(c, buf, 64);
    assert(c->stage == MRVL_STAGE_HELPER && reg16(MRVL_REG_RD_BASE) == MRVL_FW_HDR_LEN);

    /* The driver's EEPROM read (GETMEM 512 bytes at 0): key 2 is the unit's MAC. */
    memset(h, 0, 16); stl_le_p(h, MRVL_HELPER_GETMEM); stw_le_p(h + 6, 512);
    mrvl_card_write(c, h, 16);
    assert(reg16(MRVL_REG_SCRATCH) == 512);
    mrvl_card_read(c, buf, 512);
    assert(buf[8] == 0 && buf[9] == 2 && !memcmp(buf + 12, UNIT, 6));

    /* A good block: header, then data whose last word is its CRC. */
    header(h, MRVL_FW_CMD_DATA, 0x200);
    mrvl_card_write(c, h, 16);
    assert(reg16(MRVL_REG_RD_BASE) == 0x200);
    for (int i = 0; i < 0x1fc; i++) buf[i] = i * 7;
    put_be32(buf + 0x1fc, mrvl_crc32(buf, 0x1fc));
    mrvl_card_write(c, buf, 0x200);
    assert(reg16(MRVL_REG_RD_BASE) == MRVL_FW_HDR_LEN);

    /* A damaged block: the helper asks again with the error bit. */
    mrvl_card_write(c, h, 16);
    buf[3] ^= 1;
    mrvl_card_write(c, buf, 0x200);
    assert(reg16(MRVL_REG_RD_BASE) == (MRVL_FW_HDR_LEN | 1));
    /* ... and a damaged header likewise. */
    header(h, MRVL_FW_CMD_DATA, 0x200); h[5] ^= 1;
    mrvl_card_write(c, h, 16);
    assert(reg16(MRVL_REG_RD_BASE) == (MRVL_FW_HDR_LEN | 1) && c->stage == MRVL_STAGE_HELPER);

    /* The last header: the firmware is up. */
    header(h, MRVL_FW_CMD_LAST, 0);
    mrvl_card_write(c, h, 16);
    assert(c->stage == MRVL_STAGE_FIRMWARE && reg16(MRVL_REG_SCRATCH) == MRVL_FIRMWARE_OK);
    mrvl_card_writeb(c, MRVL_REG_HINT_MASK, 3);
    /* GET_HW_SPEC reports the same permanent address. */
    const uint8_t *hw = command(MRVL_CMD_GET_HW_SPEC, NULL, 0);
    assert(!memcmp(hw + 8 + 8, UNIT, 6));

    /* Channel 1 hears nothing; channel 6 hears the access point. */
    const uint8_t *r = scan(1);
    assert(lduw_le_p(r) == (MRVL_CMD_802_11_SCAN | MRVL_CMD_RESP) && !r[8 + 2]);
    r = scan(MRVL_AP_CHANNEL);
    assert(r[8 + 2] == 1 && !memcmp(r + 8 + 3 + 2, AP, 6));
    assert(r[8 + 3 + 21] == 0 && r[8 + 3 + 22] == strlen(MRVL_AP_SSID) &&
           !memcmp(r + 8 + 3 + 23, MRVL_AP_SSID, strlen(MRVL_AP_SSID)));

    /* Associate (0x0050): answered as 0x8012, status 0, AID 1. */
    uint8_t a[6];
    memcpy(a, AP, 6);
    r = command(MRVL_CMD_802_11_ASSOCIATE, a, 6);
    assert(lduw_le_p(r) == (MRVL_CMD_802_11_ASSOCIATE_OLD | MRVL_CMD_RESP));
    assert(lduw_le_p(r + 6) == 0 && lduw_le_p(r + 10) == 0 && lduw_le_p(r + 12) == 0xc001);
    assert(c->associated);
    puts("PASS: the unit MAC in the EEPROM and GET_HW_SPEC (placeholder without one), helper and firmware download with block CRCs, FIRMWARE_OK, W0C interrupt status, "
         "scan on the AP's channel only, associate answered as 0x8012");
}
'''
with tempfile.TemporaryDirectory() as d:
    path = Path(d) / 'check.c'
    path.write_text(code)
    subprocess.run(['clang', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Wno-unused-function',
                    str(path), '-o', d + '/check'], check=True)
    subprocess.run([d + '/check'], check=True)
