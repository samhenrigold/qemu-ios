/* Shared production CDC backend through the USB transport (no SDIO framing).
 * SLICE include/hw/net/bcm_fullmac.h define CDC_|BDC_|WLC_|BSS_INFO_|ISCAN_|WL_CNT_|SCAN_COMPLETE|BCM_FULLMAC_
 * SLICE hw/arm/bcm_fullmac.c define FAKE_SSID
 * SLICE hw/arm/bcm_fullmac.c fn cdc_hdrlen sdio_arm_autojoin bcm_fullmac_cdc
 * SLICE hw/usb/dev-bcm-hsic.c fn bcm_header_sum bcm_make_header bcm_fullmac_output bcm_fullmac_ready bcm_notification bcm_cdc_control bcm_bulk_input bcm_bulk_read
 * PKG glib-2.0
 */
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#define trace_sdio(...) ((void)0)
#define qemu_log_mask(...) ((void)0)
#define NANOSECONDS_PER_SECOND 1000000000LL
#define QEMU_CLOCK_VIRTUAL 0
#define USB_TOKEN_IN 1
static int64_t qemu_clock_get_ns(int c)
{
    return 0;
}
static void timer_mod(void *p, int64_t t)
{
    assert(!"unexpected timer");
}
static void *usb_ep_get(void *dev, int pid, int ep)
{
    return dev;
}
static void usb_wakeup(void *ep, int unused)
{
}
static uint32_t ldl_le_p(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
           (uint32_t)b[3] << 24;
}
static uint16_t lduw_le_p(const void *p)
{
    const uint8_t *b = p;
    return b[0] | b[1] << 8;
}
static void stw_le_p(void *p, uint16_t v)
{
    uint8_t *b = p;
    b[0] = v;
    b[1] = v >> 8;
}
static void stl_le_p(void *p, uint32_t v)
{
    uint8_t *b = p;
    for (int i = 0; i < 4; i++)
        b[i] = v >> (8 * i);
}
typedef struct {
    unsigned cdc_hdrlen, bdc_hdrlen;
    void *nic;
    bool associated, iscan_reported, host_netif, escan_pending;
    uint32_t toe, toe_ol;
    void *scan_timer, *join_timer;
    const char *fw_version;
    uint8_t bssid[6], mac[6];
    bool fws_supported;
    uint32_t fws_tlv;
} BCMFullmacState;
typedef struct {
    int parent_obj;
    bool running, control_notified, control_prefixed;
    uint8_t control_prefix[16];
    uint16_t interrupt_seq, bulk_rx_seq, bulk_tx_seq;
    size_t bulk_rx_offset;
    BCMFullmacState fullmac;
    GQueue *control_rx, *data_rx;
    int *remote_wake;
} BCMHSICState;
static BCMHSICState *transport;
static void bcm_fullmac_output(void *, unsigned, const uint8_t *, uint32_t);
static void bcm_fullmac_send(BCMFullmacState *s, unsigned ch, const uint8_t *p,
                             uint32_t n)
{
    assert(s == &transport->fullmac);
    bcm_fullmac_output(transport, ch, p, n);
}
static void fill_bss_info(BCMFullmacState *s, uint8_t *p)
{
    assert(!"unexpected bss");
}
static void fill_iscan_results(BCMFullmacState *s, uint8_t *p)
{
    assert(!"unexpected scan");
}
static void sdio_handle_set_ssid(BCMFullmacState *s, const uint8_t *p,
                                 uint32_t n)
{
    assert(!"unexpected join");
}
static uint8_t tx_bdc[256];
static unsigned tx_bdc_len;
static void bcm_fullmac_tx_data(BCMFullmacState *s, const uint8_t *p,
                                uint32_t n)
{
    assert(n <= sizeof(tx_bdc));
    memcpy(tx_bdc, p, n);
    tx_bdc_len = n;
}
typedef struct {
    struct {
        size_t size;
    } iov;
    size_t actual_length;
    uint8_t data[128];
} USBPacket;
static void usb_packet_copy(USBPacket *p, void *data, size_t size)
{
    assert(size <= p->iov.size);
    memcpy(p->data + p->actual_length, data, size);
    p->actual_length += size;
}
static unsigned flushed;
static void *qemu_get_queue(void *nic)
{
    return nic;
}
static void qemu_flush_queued_packets(void *nc)
{
    assert(nc);
    flushed++;
}
static bool bcm_escan_request(BCMFullmacState *s, const uint8_t *p, uint32_t n)
{
    assert(!"unexpected escan");
    return false;
}
static void bcm_fullmac_event_data(BCMFullmacState *s, uint32_t e,
                                   uint32_t status, uint16_t flags,
                                   const uint8_t *p, uint32_t n)
{
    assert(!"unexpected firmware event");
}
static void bcm_fullmac_credit_map(BCMFullmacState *s)
{
    assert(!s->fws_supported);
}
static void qemu_irq_raise(int *irq)
{
    if (irq) {
        *irq = 1;
    }
}
#include "slice.h"
int main(void)
{
    BCMHSICState s = {
        .running = true, .control_rx = g_queue_new(), .data_rx = g_queue_new()};
    transport = &s;
    s.fullmac.fw_version = "qemu-ios virtual FullMAC";
    uint8_t data[CDC_MAX_PAYLOAD + 32];
    memset(data, 0xcc, sizeof(data));
    uint8_t ver[20] = {0};
    stl_le_p(ver, WLC_GET_VAR);
    stl_le_p(ver + 4, 508);
    stl_le_p(ver + 8, 0x12340000);
    memcpy(ver + 16, "ver", 4);
    assert(bcm_cdc_control(&s, 0x2100, 0, 0, 20, ver) == 20);
    assert(g_queue_get_length(s.control_rx) == 1 && !s.control_notified);
    assert(bcm_cdc_control(&s, 0x2100, 0, 0, 20, ver) == -1); /* unread reply */
    assert(bcm_cdc_control(&s, 0xa101, 0, 0, 512, data) == 512);
    assert(ldl_le_p(data) == WLC_GET_VAR && ldl_le_p(data + 8) == 0x12340000);
    assert(!strcmp((char *)data + 16, s.fullmac.fw_version));
    assert(data[512] == 0xcc);
    assert(g_queue_is_empty(s.control_rx));
    assert(bcm_cdc_control(&s, 0xa101, 0, 0, 512, data) == 0);
    assert(bcm_cdc_control(&s, 0x2100, 0, 0, 15, ver) == -1);
    assert(bcm_cdc_control(&s, 0x2100, 0, 1, 20, ver) == -1);
    assert(bcm_cdc_control(&s, 0x2102, 0, 0, 20, ver) == -1);
    s.running = false;
    assert(bcm_cdc_control(&s, 0x2100, 0, 0, 20, ver) == -1);
    s.running = true;
    stl_le_p(ver + 4, 0xffff);
    assert(bcm_cdc_control(&s, 0x2100, 0, 0, 20, ver) == 20);
    assert(bcm_cdc_control(&s, 0xa101, 0, 0, sizeof(data) - 1, data) ==
           CDC_MAX_PAYLOAD + 16);
    /* Native 11A465 uses an Apple HSIC prefix and class-IN request zero. */
    uint8_t wrapped[36] = {0};
    stw_le_p(wrapped, 0x8001);
    stw_le_p(wrapped + 4, 36);
    stw_le_p(wrapped + 6, 42);
    stw_le_p(wrapped + 2, ~bcm_header_sum(wrapped));
    memcpy(wrapped + 16, ver, sizeof(ver));
    assert(bcm_cdc_control(&s, 0x2100, 0, 0, 36, wrapped) == 36);
    memset(data, 0xcc, sizeof(data));
    assert(bcm_cdc_control(&s, 0xa100, 0, 0, 512, data) == 512);
    assert(lduw_le_p(data) == 0x8001 && lduw_le_p(data + 4) == 512 &&
           lduw_le_p(data + 6) == 42 && bcm_header_sum(data) == 0xffff);
    assert(ldl_le_p(data + 16) == WLC_GET_VAR &&
           !strcmp((char *)data + 32, s.fullmac.fw_version) &&
           data[512] == 0xcc);
    wrapped[2] ^= 1;
    assert(bcm_cdc_control(&s, 0x2100, 0, 0, 36, wrapped) == -1);
    assert(bcm_cdc_control(&s, 0x2100, 0, 0, 31, wrapped) == -1);
    /* Asynchronous notification has channel 1 and an independent sequence. */
    uint8_t note[18];
    memset(note, 0xcc, sizeof(note));
    s.interrupt_seq = 1;
    assert(bcm_notification(&s, note + 1) == 16 && note[0] == 0xcc &&
           note[17] == 0xcc);
    assert(lduw_le_p(note + 1) == 0x9001 && lduw_le_p(note + 5) == 16 &&
           lduw_le_p(note + 7) == 1 && bcm_header_sum(note + 1) == 0xffff);
    assert(lduw_le_p(s.control_prefix + 6) ==
           42); /* no CDC sequence mutation */
    s.control_prefixed = false;
    memset(note, 0xcc, sizeof(note));
    assert(bcm_notification(&s, note + 1) == 8);
    assert(ldl_le_p(note + 1) == 1 && ldl_le_p(note + 5) == 0 &&
           note[9] == 0xcc);
    /* A short USB GET carries four input bytes, asks for eight output.
  * Inferring len - output picks a false 12-byte CDC / 6-byte BDC form. */
    stl_le_p(ver + 4, 8);
    assert(bcm_cdc_control(&s, 0x2100, 0, 0, sizeof(ver), ver) == sizeof(ver));
    assert(s.fullmac.cdc_hdrlen == 16 && s.fullmac.bdc_hdrlen == 4);
    assert(bcm_cdc_control(&s, 0xa100, 0, 0, sizeof(data), data) == 24);
    assert(!memcmp(data + 16, s.fullmac.fw_version, 7) && data[23] == 0);
    /* Native provisioning changes the firmware MAC through the same iovar. */
    uint8_t macreq[36] = {0};
    stl_le_p(macreq, WLC_SET_VAR);
    stl_le_p(macreq + 4, 20);
    stl_le_p(macreq + 8, CDC_DCMD_SET);
    memcpy(macreq + 16, "cur_etheraddr", 14);
    uint8_t mac[6] = {2, 0x18, 0xb6, 0xdd, 0x78, 0x0a};
    memcpy(macreq + 30, mac, 6);
    assert(bcm_cdc_control(&s, 0x2100, 0, 0, sizeof(macreq), macreq) ==
           sizeof(macreq));
    assert(bcm_cdc_control(&s, 0xa101, 0, 0, sizeof(data), data) == 36);
    assert(!memcmp(s.fullmac.mac, mac, 6));
    stl_le_p(macreq, WLC_GET_VAR);
    stl_le_p(macreq + 4, 6);
    stl_le_p(macreq + 8, 0);
    assert(bcm_cdc_control(&s, 0x2100, 0, 0, sizeof(macreq), macreq) ==
           sizeof(macreq));
    assert(bcm_cdc_control(&s, 0xa101, 0, 0, sizeof(data), data) == 22);
    assert(!memcmp(data + 16, mac, 6));
    /* Bulk framing is separate from both CDC and interrupt sequences. */
    s.control_prefixed = true;
    uint8_t bulk[40];
    bcm_make_header(bulk, 2, 0, sizeof(bulk));
    memset(bulk + 16, 0x5a, 24);
    assert(bcm_bulk_input(&s, bulk, sizeof(bulk)) && tx_bdc_len == 24 &&
           tx_bdc[0] == 0x5a && s.bulk_tx_seq == 1);
    assert(!bcm_bulk_input(&s, bulk, sizeof(bulk))); /* replay */
    bcm_make_header(bulk, 2, 1, sizeof(bulk));
    bulk[2] ^= 1;
    assert(!bcm_bulk_input(&s, bulk, sizeof(bulk)));
    bcm_make_header(bulk, 2, 1, sizeof(bulk) + 1);
    assert(!bcm_bulk_input(&s, bulk, sizeof(bulk)));
    bcm_make_header(bulk, 1, 1, sizeof(bulk));
    assert(!bcm_bulk_input(&s, bulk, sizeof(bulk)));
    bcm_fullmac_output(&s, BCM_FULLMAC_EVENT, bulk + 16, 24);
    GBytes *event = g_queue_pop_head(s.data_rx);
    gsize eventlen;
    const uint8_t *eventp = g_bytes_get_data(event, &eventlen);
    assert(eventlen == 40 && lduw_le_p(eventp) == 0xa001 &&
           lduw_le_p(eventp + 4) == 40 && lduw_le_p(eventp + 6) == 0 &&
           bcm_header_sum(eventp) == 0xffff);
    assert(!memcmp(eventp + 16, bulk + 16, 24));
    g_bytes_unref(event);
    assert(s.bulk_rx_seq == 1 && s.interrupt_seq == 1 &&
           lduw_le_p(s.control_prefix + 6) == 42);
    /* Short host reads preserve the remainder and flush backend backpressure
  * only after the complete packet has been consumed. */
    bcm_fullmac_output(&s, BCM_FULLMAC_DATA, bulk + 16, 24);
    s.fullmac.nic = &s;
    USBPacket chunk = {.iov = {7}};
    assert(bcm_bulk_read(&s, &chunk) && chunk.actual_length == 7 &&
           s.bulk_rx_offset == 7 && g_queue_get_length(s.data_rx) == 1 &&
           !flushed);
    uint8_t assembled[40];
    memcpy(assembled, chunk.data, 7);
    memset(&chunk, 0, sizeof(chunk));
    chunk.iov.size = 10;
    assert(bcm_bulk_read(&s, &chunk) && chunk.actual_length == 10 &&
           s.bulk_rx_offset == 17 && !flushed);
    memcpy(assembled + 7, chunk.data, 10);
    memset(&chunk, 0, sizeof(chunk));
    chunk.iov.size = 64;
    assert(bcm_bulk_read(&s, &chunk) && chunk.actual_length == 23 &&
           !s.bulk_rx_offset && flushed == 1 && g_queue_is_empty(s.data_rx));
    memcpy(assembled + 17, chunk.data, 23);
    assert(lduw_le_p(assembled + 6) == 1 &&
           bcm_header_sum(assembled) == 0xffff &&
           !memcmp(assembled + 16, bulk + 16, 24));
    assert(!bcm_bulk_read(&s, &chunk));

    s.control_prefixed = false;
    /* Data/events have a distinct pipe; control reads must not consume them. */
    bcm_fullmac_output(&s, BCM_FULLMAC_EVENT, ver, sizeof(ver));
    assert(g_queue_get_length(s.data_rx) == 1 && bcm_fullmac_ready(&s));
    assert(bcm_cdc_control(&s, 0xa101, 0, 0, 10, data) == 0 &&
           g_queue_get_length(s.data_rx) == 1);
    for (int i = 1; i < 256; i++)
        bcm_fullmac_output(&s, BCM_FULLMAC_DATA, ver, sizeof(ver));
    assert(!bcm_fullmac_ready(&s));
    bcm_fullmac_output(&s, BCM_FULLMAC_DATA, ver, sizeof(ver));
    assert(g_queue_get_length(s.data_rx) == 256);
    g_queue_free_full(s.control_rx, (GDestroyNotify)g_bytes_unref);
    g_queue_free_full(s.data_rx, (GDestroyNotify)g_bytes_unref);
    puts(
        "PASS: native USB ver request through shared CDC, request IDs, short reads, malformed commands, bounded independent queues");
}
