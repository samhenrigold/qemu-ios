/* The shared Ethernet/BDC path used by SDIO and USB.
 * SLICE include/hw/net/bcm_fullmac.h define BDC_|TOE_|BCM_FULLMAC_
 * SLICE hw/arm/bcm_fullmac.c fn bcm_fws_tag bcm_fws_complete sdio_bdc_hdrlen hexdump_frame bcm_fullmac_tx_data sdio_net_receive sdio_net_can_receive
 * PKG glib-2.0
 */
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define trace_sdio(...) ((void)0)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CSUM_ALL 0xffff
static uint16_t lduw_be_p(const void *p)
{
    const uint8_t *b = p;
    return b[0] << 8 | b[1];
}
static uint32_t ldl_le_p(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
           (uint32_t)b[3] << 24;
}
static void stl_le_p(void *p, uint32_t x)
{
    uint8_t *b = p;
    for (unsigned i = 0; i < 4; i++)
        b[i] = x >> (8 * i);
}
typedef int NetClientState;
typedef struct {
    unsigned bdc_hdrlen, tx_log, host_rx_log;
    bool dongle_started;
    void *nic;
    uint32_t toe, toe_ol;
    bool (*can_receive)(void *);
    void *opaque;
    bool fws_supported;
    uint32_t fws_tlv;
} BCMFullmacState;
static BCMFullmacState state;
static uint8_t host[4096], guest[4096];
static unsigned hostlen, guestlen, channel, checksums;
static BCMFullmacState *qemu_get_nic_opaque(NetClientState *n)
{
    return &state;
}
static NetClientState *qemu_get_queue(void *n)
{
    return n;
}
static void qemu_send_packet(NetClientState *n, const uint8_t *p, unsigned len)
{
    assert(len <= sizeof(host));
    memcpy(host, p, len);
    hostlen = len;
}
static void net_checksum_calculate(uint8_t *p, unsigned n, unsigned flags)
{
    assert(flags == CSUM_ALL);
    checksums++;
    p[n - 1] = 0xa5;
}
static void bcm_fullmac_send(BCMFullmacState *s, unsigned c, const uint8_t *p,
                             unsigned n)
{
    assert(n <= sizeof(guest));
    memcpy(guest, p, n);
    guestlen = n;
    channel = c;
}
static bool ready(void *p)
{
    return *(bool *)p;
}
#include "slice.h"
int main(void)
{
    uint8_t eth[60];
    memset(eth, 0x5a, sizeof(eth));
    eth[12] = 8;
    eth[13] = 0;
    NetClientState net = 0;
    state.nic = &net;
    for (unsigned bdc = 4; bdc <= 6; bdc += 2) {
        uint8_t frame[80] = {0x20};
        memcpy(frame + bdc, eth, sizeof(eth));
        state.bdc_hdrlen = bdc;
        hostlen = 0;
        bcm_fullmac_tx_data(&state, frame, bdc + sizeof(eth));
        assert(hostlen == sizeof(eth) && !memcmp(host, eth, sizeof(eth)));
        state.dongle_started = true;
        guestlen = 0;
        assert(sdio_net_receive(&net, eth, sizeof(eth)) == sizeof(eth));
        assert(channel == BCM_FULLMAC_DATA && guestlen == bdc + sizeof(eth) &&
               guest[0] == 0x20 && !memcmp(guest + bdc, eth, sizeof(eth)));
    }
    bool accept = false;
    state.can_receive = ready;
    state.opaque = &accept;
    assert(!sdio_net_can_receive(&net));
    accept = true;
    assert(sdio_net_can_receive(&net));
    state.dongle_started = false;
    assert(!sdio_net_can_receive(&net));
    guestlen = 0;
    sdio_net_receive(&net, eth, sizeof(eth));
    assert(!guestlen);
    state.dongle_started = true;
    sdio_net_receive(&net, eth, 13);
    assert(!guestlen);
    uint8_t frame[64] = {0x20};
    memcpy(frame + 4, eth, sizeof(eth));
    state.bdc_hdrlen = 4;
    state.toe = 1;
    state.toe_ol = TOE_TX_CSUM_OL;
    bcm_fullmac_tx_data(&state, frame, sizeof(frame));
    assert(checksums == 1 && hostlen == 60 && host[59] == 0xa5 &&
           frame[63] == 0x5a);
    hostlen = 0;
    bcm_fullmac_tx_data(&state, frame, 17);
    assert(!hostlen);
    /* With signalling negotiated, dataOffset is authoritative: don't guess
  * an Ethernet header inside the metadata. Verify tag identity and one
  * credit for the exact FIFO, with no Ethernet bytes in the completion. */
    state.fws_supported = true;
    state.fws_tlv = 4;
    state.toe = 0;
    uint8_t tagged[72] = {0x20, 0, 0, 2, 5, 4};
    uint32_t tag = 0x9b123456; /* generation, FIFO 3, slot, counter */
    stl_le_p(tagged + 6, tag);
    tagged[10] = tagged[11] = 255;
    memcpy(tagged + 12, eth, 60);
    hostlen = guestlen = 0;
    bcm_fullmac_tx_data(&state, tagged, sizeof(tagged));
    assert(hostlen == 60 && !memcmp(host, eth, 60));
    assert(guestlen == 20 && guest[3] == 4 && guest[4] == 4 && guest[5] == 4);
    assert(ldl_le_p(guest + 6) == (tag & ~0x78000000U));
    assert(guest[10] == 11 && guest[11] == 6 && guest[18] == 255 &&
           guest[19] == 255);
    for (unsigned i = 0; i < 6; i++)
        assert(guest[12 + i] == (i == 3));
    /* Reject truncated TLVs, duplicate tags, invalid FIFO and oversized offset. */
    const unsigned bad_offsets[] = {1, 3, 255};
    for (unsigned i = 0; i < 3; i++) {
        tagged[3] = bad_offsets[i];
        hostlen = guestlen = 0;
        if (i == 1) {
            tagged[10] = 5;
            tagged[11] = 4;
            stl_le_p(tagged + 12, tag);
        }
        bcm_fullmac_tx_data(&state, tagged, sizeof(tagged));
        assert(!hostlen && !guestlen);
    }
    tagged[3] = 2;
    tagged[10] = tagged[11] = 255;
    stl_le_p(tagged + 6, 0x06000000);
    hostlen = guestlen = 0;
    bcm_fullmac_tx_data(&state, tagged, sizeof(tagged));
    assert(!hostlen && !guestlen);
    puts(
        "PASS: shared BDC 4/6 framing, Ethernet payloads, receive gating, TX checksum isolation and bounded firmware credit completion");
}
