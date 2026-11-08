/* Extended scan response bytes and cancellation, shared across transports.
 * SLICE include/hw/net/bcm_fullmac.h define BDC_|BCMETH_|ETHER_TYPE_BRCM|WL_EVENT_MSG_LEN|WLC_|CDC_|WL_CNT_|ISCAN_|ESCAN_|BSS_INFO_|BSS_CAP_|SCAN_COMPLETE|BCM_FULLMAC_
 * SLICE hw/arm/bcm_fullmac.c define FAKE_SSID
 * SLICE hw/arm/bcm_fullmac.c fn sdio_bdc_hdrlen bcm_fullmac_event_data sdpcm_send_event bcm_fullmac_credit_map sdio_send_assoc_events fill_bss_info sdio_scan_complete bcm_escan_request cdc_hdrlen bcm_fullmac_cdc
 * PKG glib-2.0
 */
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define trace_sdio(...) ((void)0)
#define QEMU_CLOCK_VIRTUAL 0
static int64_t qemu_clock_get_ns(int unused)
{
    return 1000000;
}
typedef struct {
    int64_t due;
    unsigned arms;
} QEMUTimer;
static void timer_mod(QEMUTimer *t, int64_t when)
{
    t->due = when;
    t->arms++;
}
static uint16_t lduw_le_p(const void *v)
{
    const uint8_t *p = v;
    return p[0] | p[1] << 8;
}
static uint32_t ldl_le_p(const void *v)
{
    const uint8_t *p = v;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static void stw_le_p(void *v, uint16_t x)
{
    uint8_t *p = v;
    p[0] = x;
    p[1] = x >> 8;
}
static void stl_le_p(void *v, uint32_t x)
{
    uint8_t *p = v;
    for (int i = 0; i < 4; i++)
        p[i] = x >> (8 * i);
}
static void stw_be_p(void *v, uint16_t x)
{
    uint8_t *p = v;
    p[0] = x >> 8;
    p[1] = x;
}
static void stl_be_p(void *v, uint32_t x)
{
    uint8_t *p = v;
    for (int i = 0; i < 4; i++)
        p[i] = x >> (8 * (3 - i));
}
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
           p[3];
}
typedef struct {
    unsigned bdc_hdrlen, cdc_hdrlen;
    bool associated, host_netif;
    const char *fw_version;
    uint32_t toe, toe_ol;
    uint8_t mac[6], bssid[6];
    char country[3];
    bool iscan_reported, escan_pending;
    uint16_t escan_sync_id;
    uint32_t escan_status;
    QEMUTimer *scan_timer, *join_timer;
    bool fws_supported;
    uint32_t fws_tlv;
} BCMFullmacState;
static GPtrArray *events;
static GBytes *control_reply;
static void bcm_fullmac_send(BCMFullmacState *s, unsigned ch, const uint8_t *p,
                             uint32_t n)
{
    if (ch == 0) {
        g_clear_pointer(&control_reply, g_bytes_unref);
        control_reply = g_bytes_new(p, n);
    } else {
        assert(ch == 1);
        g_ptr_array_add(events, g_bytes_new(p, n));
    }
}
static void fill_iscan_results(BCMFullmacState *s, uint8_t *p)
{
    assert(!"unexpected legacy scan");
}
static void sdio_arm_autojoin(BCMFullmacState *s)
{
    assert(!"unexpected autojoin");
}
static void sdio_handle_set_ssid(BCMFullmacState *s, const uint8_t *p,
                                 uint32_t n)
{
    assert(!"unexpected SSID");
}
#include "slice.h"
static const uint8_t *event(unsigned i, unsigned status, unsigned count,
                            uint16_t sync)
{
    gsize n;
    const uint8_t *p = g_bytes_get_data(g_ptr_array_index(events, i), &n);
    assert(n >= 88 && p[0] == 0x20 && be32(p + 32) == WLC_E_ESCAN_RESULT &&
           be32(p + 36) == status);
    unsigned len = be32(p + 48);
    assert(n == 80 + len && n > 76 + len);
    for (unsigned k = 0; k < 4; k++)
        assert(p[76 + len + k] == 0);
    const uint8_t *r = p + 76;
    assert(ldl_le_p(r) == len && ldl_le_p(r + 4) == 1 &&
           lduw_le_p(r + 8) == sync && lduw_le_p(r + 10) == count);
    return r;
}
int main(void)
{
    events = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
    QEMUTimer timer = {0};
    BCMFullmacState s = {.bdc_hdrlen = 4,
                         .country = "US",
                         .scan_timer = &timer,
                         .iscan_reported = true};
    uint8_t req[8] = {0};
    stl_le_p(req, 1);
    stw_le_p(req + 4, 1);
    stw_le_p(req + 6, 0x1023);
    assert(bcm_escan_request(&s, req, sizeof(req)) && s.escan_pending &&
           !s.iscan_reported && timer.arms == 1 &&
           timer.due == 1000000 + SCAN_COMPLETE_DELAY_NS);
    assert(events->len == 0);
    sdio_scan_complete(&s);
    assert(events->len == 2 && !s.escan_pending && s.iscan_reported);
    const uint8_t *r = event(0, ESCAN_STATUS_PARTIAL, 1, 0x1023);
    assert(ldl_le_p(r + 12) == BSS_INFO_VERSION &&
           ldl_le_p(r + 16) == BSS_INFO_TOTAL);
    assert(r[12 + BSS_INFO_OFF_SSID_LEN] == 8 &&
           !memcmp(r + 12 + BSS_INFO_OFF_SSID, "qemu-ios", 8));
    event(1, 0, 0, 0x1023);
    g_ptr_array_set_size(events, 0);
    assert(bcm_escan_request(&s, req, 8));
    stw_le_p(req + 4, 3);
    stw_le_p(req + 6, 0xffff);
    assert(bcm_escan_request(&s, req, 8));
    sdio_scan_complete(&s);
    assert(events->len == 1);
    event(0, ESCAN_STATUS_ABORT, 0, 0x1023);
    g_ptr_array_set_size(events, 0);
    assert(bcm_escan_request(&s, req, 8) && !s.escan_pending); /* idle abort */
    assert(!bcm_escan_request(&s, req, 7));
    stl_le_p(req, 2);
    assert(!bcm_escan_request(&s, req, 8));
    stl_le_p(req, 1);
    stw_le_p(req + 4, 0xffff);
    assert(!bcm_escan_request(&s, req, 8) && !s.escan_pending);
    /* Send escan through the actual CDC decoder, including malformed SET. */
    uint8_t command[30] = {0};
    stl_le_p(command, WLC_SET_VAR);
    stl_le_p(command + 4, 14);
    stl_le_p(command + 8, 0x33440002);
    memcpy(command + 16, "escan", 6);
    stl_le_p(command + 22, 1);
    stw_le_p(command + 26, 1);
    stw_le_p(command + 28, 0x7777);
    bcm_fullmac_cdc(&s, command, sizeof(command), 16);
    assert(s.escan_pending && events->len == 0);
    gsize n;
    const uint8_t *ack = g_bytes_get_data(control_reply, &n);
    assert(n == 30 && ldl_le_p(ack + 8) == 0x33440002 &&
           ldl_le_p(ack + 12) == 0);
    sdio_scan_complete(&s);
    assert(events->len == 2);
    event(0, 8, 1, 0x7777);
    event(1, 0, 0, 0x7777);
    g_ptr_array_set_size(events, 0);
    unsigned arms = timer.arms;
    stl_le_p(command + 4, 13);
    bcm_fullmac_cdc(&s, command, 29, 16);
    ack = g_bytes_get_data(control_reply, &n);
    assert((ldl_le_p(ack + 8) & CDC_DCMD_ERROR) &&
           ldl_le_p(ack + 12) == (uint32_t)-2 && !s.escan_pending &&
           timer.arms == arms);
    /* Legacy transports do not acquire a new credit protocol. The USB
  * firmware issues one map on negotiation, not on every repeated SET. */
    uint8_t tlv[24] = {0};
    stl_le_p(tlv, WLC_SET_VAR);
    stl_le_p(tlv + 4, 8);
    stl_le_p(tlv + 8, 2);
    memcpy(tlv + 16, "tlv", 4);
    stl_le_p(tlv + 20, 15);
    bcm_fullmac_cdc(&s, tlv, sizeof(tlv), 16);
    assert(events->len == 0 && !s.fws_tlv);
    s.fws_supported = true;
    bcm_fullmac_cdc(&s, tlv, sizeof(tlv), 16);
    assert(s.fws_tlv == 15 && events->len == 1);
    const uint8_t *map = g_bytes_get_data(g_ptr_array_index(events, 0), &n);
    assert(n == 86 && be32(map + 32) == 74 && be32(map + 48) == 6 &&
           n > 76 + be32(map + 48));
    for (unsigned i = 0; i < 6; i++)
        assert(map[76 + i] == 8);
    bcm_fullmac_cdc(&s, tlv, sizeof(tlv), 16);
    assert(events->len == 1);
    stl_le_p(tlv + 20, 0);
    bcm_fullmac_cdc(&s, tlv, sizeof(tlv), 16);
    assert(!s.fws_tlv && events->len == 1);
    stl_le_p(tlv + 20, 4);
    bcm_fullmac_cdc(&s, tlv, sizeof(tlv), 16);
    assert(events->len == 2);
    g_ptr_array_set_size(events, 0);
    sdio_send_assoc_events(&s);
    assert(events->len == 4);
    g_bytes_unref(control_reply);
    g_ptr_array_free(events, true);
    puts(
        "PASS: escan v1 delayed partial/final result, event payload offsets/lengths/BSS, sync identity, abort and malformed request bounds");
}
