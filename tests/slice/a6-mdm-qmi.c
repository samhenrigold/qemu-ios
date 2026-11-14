/* QMUX transport framing and result isolation.
 * SLICE hw/usb/dev-mdm9x15.c define MDM_INTERFACES|MDM_MAX_FRAME|MDM_BOOT_SRAM_BASE|MDM_BOOT_SRAM_SIZE|MDM_MAX_IMAGE|MDM_NAS_CLIENTS|MDM_NAS_SERVING|MDM_NAS_SYSTEM|MDM_PDS_SV_IN_FIX
 * SLICE:NAS hw/usb/dev-mdm9x15.c typedef MdmNasClient
 * SLICE hw/usb/dev-mdm9x15.c typedef MdmDownloadState
 * SLICE hw/usb/dev-mdm9x15.c fn mdm_power_filter_reset mdm_power_filter_allows mdm_fuse_serial tlv find_tlv mdm_pds_reset mdm_wake mdm_qmi_ready mdm_sim_pin_state mdm_registered mdm_uim_card_status mdm_uim_file mdm_nas_plmn mdm_nas_serving mdm_nas_system_info mdm_nas_client mdm_nas_register mdm_qmi_trace mdm_qmi_output mdm_pri_version mdm_pri_start mdm_pri_text mdm_pri_end mdm_bsp_parse_pri mdm_bsp_file_write mdm_bsp_file_reset mdm_bsp_file_indicate mdm_nas_indicate mdm_dms_mode mdm_dms_indicate mdm_voice_state mdm_voice_changed mdm_voice_info mdm_voice_indicate mdm_pds_tracking_state mdm_pds_indicate mdm_pds_float mdm_pds_double mdm_pds_position mdm_pds_fix_indicate mdm_wms_smsc mdm_wms_smsc_indicate mdm_wms_indicate mdm_wms_pdu mdm_wms_forget_slot mdm_wms_received mdm_wms_receive_indicate mdm_uim_indicate mdm_qmi_events mdm_qmi_reply mdm_qmi mdm_qmux_receive mdm_dload_crc mdm_hdlc_encode mdm_dload_reply mdm_download_free mdm_sahara_packet mdm_sahara_begin mdm_sahara_read_image mdm_sahara_image_header mdm_sahara_image_data mdm_sahara_finish_image mdm_sahara_receive mdm_dload_boot mdm_dload_command mdm_dload_receive mdm_receive mdm_read
 * SLICE hw/misc/ios_baseband_core.c range static const char *const gsm7_main | static bool gsm7_encodable
 * SLICE hw/misc/ios_baseband_core.c fn digits_to_semi ios_bb_sim_read_file
 * SLICE:CALL include/hw/misc/ios_baseband_core.h range /* +XCALLSTAT states | typedef struct IosBbAtChan
 * SLICE:WDS hw/usb/dev-mdm9x15.c typedef MdmWdsClient
 * SLICE:BSP hw/usb/dev-mdm9x15.c typedef MdmBspFile
 * SLICE:PRI hw/usb/dev-mdm9x15.c typedef MdmPriParser
 * SLICE:SMSTYPE include/hw/misc/ios_baseband_core.h typedef IosBbSms
 * SLICE:GPS include/hw/misc/ios_baseband_gps.h typedef IosBbGps
 * SLICE hw/misc/ios_baseband_gps.c fn nmea_coord ios_bb_gps_nmea ios_bb_gps_set ios_bb_gps_reset
 * SLICE:SMS hw/usb/dev-mdm9x15.c typedef MdmWmsInbox
 * PKG glib-2.0
 */
#include <glib.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "GPS.h"
#define USB_DEVICE(x) (x)
#define LOG_UNIMP 1
#define qemu_log_mask(...) ((void)0)
#define trace_event_get_state_backends(...) false
#define trace_usb_mdm9x15_qmi(...) ((void)0)
#define trace_usb_mdm9x15_registration(...) ((void)0)
#define USB_TOKEN_IN 1
#define IOS_BB_MAX_CALLS 4
#include "CALL.h"
#include "SMSTYPE.h"
typedef struct {
    IosBbSms store[4];
    char sca[24];
    char imei[16], imsi[16], iccid[21], operator_long[33], plmn[7];
    bool off, sim_present, registered, pin_on, pin_ok, cops_detached;
    int cfun, pin_left, puk_left, signal_dbm;
    int64_t now_ms;
    char last_dialed[32];
    IosBbCall calls[4];
    unsigned lac, ci;
    IosBbGps gps;
    int64_t wall_offset_ms;
} IosBbCore;
typedef struct {
    IosBbCore bb;
} IosBasebandState;
typedef struct MdmDownloadState MdmDownloadState;
#include "NAS.h"
#include "WDS.h"
#include "BSP.h"
#include "PRI.h"
#define MDM_INTERFACES 13
#define IOS_BB_SMS_STORE 4
#include "SMS.h"
typedef struct {
    bool ready;
    void *qmi_event_timer;
    uint16_t voice_events[14][256];
    bool voice_client[14][256], voice_dirty[14][256];
    IosBbCall voice_ended[4];
    MdmWdsClient wds[14][256];
    MdmBspFile bsp_files[5];
    MdmNasClient nas[32];
    bool pds_force_off, pds_enabled, pds_autotracking;
    uint8_t pds_nmea[3], pds_tracking_config[10];
    uint32_t pds_events[14][256];
    MdmWmsInbox wms_inbox;
    uint8_t wms_events[14][256], wms_notify[14][256];
    bool wms_client[14][256], wms_smsc_sent[14][256];
    char wms_last_smsc[24];
    uint8_t wms_registration;
    bool uim_client[14][256], uim_dirty[14][256];
    uint8_t uim_last_status[64], uim_last_len;
    uint8_t wms_routes[2 + 32 * 4];
    uint16_t wms_routes_len;
    bool wms_status_reports, wms_broadcast[2];
    uint32_t wms_timers[3];
    bool pds_client[14][256], pds_dirty[14][256];
    uint8_t pds_last_state[2];
    int64_t pds_fix_due[14][256];
    uint8_t pds_report_mode, pds_xtra_auto[3], pds_xtra_embedded[2];
    bool pds_xtra_pending, pds_dpo, pds_dpo_persistent;
    uint32_t pds_cell_database, pds_navigation[14], pds_navigation_mask;
    bool dms_mode_events[14][256], dms_mode_notified[14][256];
    uint8_t dms_last_mode;
    bool qmi_ready_sent;
    unsigned data_logs, qmi_logs;
    void *reconnect_timer;
    bool reconnect_pending;
    MdmDownloadState *download;
    IosBasebandState *baseband;
    GQueue rx[13];
    GByteArray *tx[13];
    uint8_t next_cid[256];
    GBytes *qmi_power_filter[14][256];
    uint16_t qmi_protocol[14];
    uint32_t qmi_power_save[14], qmi_uim_events[14];
    int *remote_wake;
} MDM9x15State;
size_t ios_bb_sim_read_file(const IosBbCore *, uint16_t, uint8_t *, size_t);
static GByteArray *uart_reply;
static bool uart_routed = true;
static bool mdm_uart_qmi_routed(const MDM9x15State *s)
{
    return uart_routed;
}
static unsigned reset_detect_releases;
static void mdm_release_reset_detect(MDM9x15State *s)
{
    reset_detect_releases++;
}
static bool ios_bb_h5_send(IosBbCore *bb, const uint8_t *p, size_t n)
{
    g_byte_array_append(uart_reply, p, n);
    return true;
}
static void mdm_log(MDM9x15State *s, const char *k, unsigned a, unsigned b,
                    unsigned c)
{
}
static void *usb_ep_get(void *d, int p, int n)
{
    return d;
}
static void usb_wakeup(void *p, int n)
{
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
    for (unsigned i = 0; i < 4; i++)
        b[i] = v >> (8 * i);
}
static void stq_le_p(void *p, uint64_t v)
{
    uint8_t *b = p;
    for (unsigned i = 0; i < 8; i++)
        b[i] = v >> (8 * i);
}
static uint32_t ldl_be_p(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | b[2] << 8 | b[3];
}
static uint16_t lduw_be_p(const void *p)
{
    const uint8_t *b = p;
    return b[0] << 8 | b[1];
}
static uint32_t ldl_le_p(const void *p)
{
    const uint8_t *b = p;
    return b[0] | b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
#define QEMU_CLOCK_VIRTUAL 0
static int64_t test_clock;
static int64_t qemu_clock_get_ms(int clock)
{
    return test_clock;
}
static void timer_mod(void *timer, int64_t deadline)
{
}
static unsigned sms_submissions;
static bool ios_bb_submit_sms_pdu(IosBbCore *bb, const uint8_t *pdu, size_t len,
                                  uint8_t *reference)
{
    if (len < 5)
        return false;
    sms_submissions++;
    *reference = 7;
    return true;
}

static bool ios_bb_set_radio_mode(IosBbCore *bb, int mode)
{
    bb->cfun = mode;
    return true;
}
static int ios_bb_network_registration(IosBbCore *bb, int64_t now)
{
    return bb->sim_present && !bb->off && bb->cfun == 1 &&
                   (!bb->pin_on || bb->pin_ok)
               ? (bb->registered ? 1 : 2)
               : 0;
}
static int ios_bb_dial(IosBbCore *bb, const char *number)
{
    snprintf(bb->last_dialed, sizeof(bb->last_dialed), "%s", number);
    return 42;
}
static bool ios_bb_hangup(IosBbCore *bb, int id)
{
    return id == 42;
}
static unsigned answers;
static bool ios_bb_answer(IosBbCore *bb, int id)
{
    answers++;
    return id == 42;
}
static void ios_bb_calls_tick(IosBbCore *bb, int64_t now)
{
}
static bool ios_bb_release_active(IosBbCore *bb)
{
    return true;
}
bool ios_bb_gps_set(IosBbGps *, const char *);
void ios_bb_gps_reset(IosBbGps *);
int ios_bb_gps_nmea(const IosBbGps *, int64_t, const char *, char *, size_t);
static void qemu_irq_raise(int *irq)
{
    if (irq) {
        *irq = 1;
    }
}
#include "slice.h"
static void test_pri_metadata(MDM9x15State *s)
{
    const char xml[] =
        "<?xml version=\"1.0\"?><!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\"><plist version=\"1.0\"><dict><key>Maverick</key><dict><key>Carrier ID</key><string></string><key>PRI Revision</key><string>0.1.081</string></dict></dict></plist>";
    uint16_t pri[3];
    assert(mdm_bsp_parse_pri((const uint8_t *)xml, strlen(xml), pri));
    assert(pri[0] == 0 && pri[1] == 1 && pri[2] == 81);
    assert(!mdm_pri_version("65536.1.2", pri));
    assert(!mdm_pri_version("256.1.2", pri));
    assert(!mdm_pri_version("1.256.2", pri));
    assert(mdm_pri_version("255.255.65535", pri));
    assert(!mdm_pri_version("0.1", pri));
    assert(!mdm_pri_version("-1.2.3", pri));
    assert(!mdm_pri_version("0.1.81junk", pri));
    const char wrong[] =
        "<plist><dict><key>Other</key><dict><key>PRI Revision</key><string>9.8.7</string></dict></dict></plist>";
    assert(!mdm_bsp_parse_pri((const uint8_t *)wrong, strlen(wrong), pri));
    assert(!mdm_bsp_parse_pri((const uint8_t *)xml, strlen(xml) - 8, pri));
    uint8_t part[1033] = {1, 0, 0, 2, 0, 0, 0, 0, 0};
    size_t chunk = strlen(xml) > 2048 ? 1024 : strlen(xml) / 2;
    unsigned count = (strlen(xml) + chunk - 1) / chunk;
    stw_le_p(part + 7, count);
    for (unsigned index = 0; index < count; index++) {
        size_t bytes = MIN(chunk, strlen(xml) - index * chunk);
        stw_le_p(part + 5, index);
        stw_le_p(part + 1, 6 + bytes);
        memcpy(part + 9, xml + index * chunk, bytes);
        assert(!mdm_bsp_file_write(s, 5, 7, part, 9 + bytes));
        if (index + 1 < count)
            assert(!s->bsp_files[2].pri_valid);
    }
    assert(s->bsp_files[2].pri_valid);
    mdm_bsp_file_reset(s, true);
    const uint8_t query[] = {1, 12, 0, 0, 0xe2, 1, 0, 1, 0, 3, 0xb0, 0, 0};
    GByteArray *reply = mdm_qmi_reply(s, 11, query, sizeof(query));
    size_t len = 0;
    const uint8_t *info = find_tlv(reply->data + 13, reply->len - 13, 4, &len);
    const uint8_t expected[] = {0,   1,   81,  0,   0,   0, '0',
                                '0', '0', '0', '0', '1', 0};
    assert(info && len == 13 && !memcmp(info, expected, sizeof(expected)));
    g_byte_array_unref(reply);
    /* Unrecognized metadata must not erase the last applied revision. */
    stw_le_p(part + 5, 0);
    stw_le_p(part + 7, 1);
    stw_le_p(part + 1, 6 + strlen(wrong));
    memcpy(part + 9, wrong, strlen(wrong));
    assert(!mdm_bsp_file_write(s, 5, 7, part, 9 + strlen(wrong)));
    assert(s->bsp_files[2].pri_valid && s->bsp_files[2].pri[2] == 81);
    g_clear_pointer(&s->bsp_files[2].committed, g_byte_array_unref);
}
static GByteArray *sms_request(MDM9x15State *s, unsigned iface, uint8_t cid,
                               unsigned message, const uint8_t *value,
                               size_t len)
{
    uint8_t h[13] = {1, 0, 0, 0, 5, cid, 0, 1, 0, 0, 0, 0, 0};
    GByteArray *p = g_byte_array_new();
    stw_le_p(h + 9, message);
    g_byte_array_append(p, h, 13);
    tlv(p, 1, value, len);
    stw_le_p(p->data + 1, p->len - 1);
    stw_le_p(p->data + 11, p->len - 13);
    GByteArray *r = mdm_qmi_reply(s, iface, p->data, p->len);
    g_byte_array_unref(p);
    assert(r);
    return r;
}

static bool sms_result_ok(const GByteArray *r)
{
    size_t len = 0;
    const uint8_t *v = find_tlv(r->data + 13, r->len - 13, 2, &len);
    assert(v && len == 4);
    return lduw_le_p(v) == 0;
}

static void test_incoming_sms(void)
{
    MDM9x15State *s = g_new0(MDM9x15State, 1);
    IosBasebandState backend = {0};
    s->baseband = &backend;
    IosBbSms *sms = &backend.bb.store[0];
    sms->used = true;
    strcpy(sms->pdu, "0000049121430000625060010203000141");
    s->wms_routes_len = 6;
    stw_le_p(s->wms_routes, 1);
    s->wms_routes[2] = 0;
    s->wms_routes[3] = 4;
    s->wms_routes[4] = 255;
    s->wms_routes[5] = 2;
    s->wms_client[9][3] = true;
    s->wms_events[9][3] = 1;
    s->wms_client[11][4] = true; /* No subscription, no delivery. */
    mdm_wms_received(s, 0);
    assert(s->wms_inbox.pending[9][3] == 1 && !s->wms_inbox.pending[11][4]);
    assert(!s->wms_inbox.awaiting_ack[9][3]);
    assert(mdm_wms_receive_indicate(s, 9, 3, 0));
    uint8_t out[512];
    size_t len = 0;
    unsigned n = mdm_read(s, 9, out, sizeof(out));
    const uint8_t *v = find_tlv(out + 13, n - 13, 0x11, &len);
    assert(v && len == 24 && v[0] == 0 && v[5] == 6 && lduw_le_p(v + 6) == 16);
    assert(v[8] == 0 && v[9] == 4 && v[11] == 0x21 && v[23] == 0x41);
    uint32_t token = ldl_le_p(v + 1);
    assert(token);
    assert(s->wms_inbox.awaiting_ack[9][3] == 1);
    uint8_t ack[6] = {0, 0, 0, 0, 1, 1};
    stl_le_p(ack, token);
    GByteArray *r = sms_request(s, 9, 4, 0x37, ack, 6);
    assert(!sms_result_ok(r));
    g_byte_array_unref(r); /* Wrong client. */
    ack[4] = 0;
    r = sms_request(s, 9, 3, 0x37, ack, 6);
    assert(!sms_result_ok(r));
    g_byte_array_unref(r);
    ack[4] = 1;
    r = sms_request(s, 9, 3, 0x37, ack, 6);
    assert(sms_result_ok(r) && !s->wms_inbox.awaiting_ack[9][3]);
    g_byte_array_unref(r);
    r = sms_request(s, 9, 3, 0x37, ack, 6);
    assert(!sms_result_ok(r));
    g_byte_array_unref(r);
    /* A reused ring slot invalidates a previously delivered ACK token. */
    mdm_wms_received(s, 0);
    assert(mdm_wms_receive_indicate(s, 9, 3, 0));
    n = mdm_read(s, 9, out, sizeof(out));
    v = find_tlv(out + 13, n - 13, 0x11, &len);
    token = ldl_le_p(v + 1);
    mdm_wms_received(s, 0);
    assert(s->wms_inbox.transaction[0] != token &&
           !s->wms_inbox.awaiting_ack[9][3]);
    stl_le_p(ack, token);
    r = sms_request(s, 9, 3, 0x37, ack, 6);
    assert(!sms_result_ok(r));
    g_byte_array_unref(r);
    /* Suspend policy defers delivery without inventing an outstanding ACK. */
    s->qmi_power_save[9] = 1;
    s->qmi_power_filter[9][5] = g_bytes_new_static("", 0);
    assert(!mdm_wms_receive_indicate(s, 9, 3, 0));
    assert(!s->wms_inbox.awaiting_ack[9][3] && g_queue_is_empty(&s->rx[9]));
    s->qmi_power_save[9] = 0;
    g_clear_pointer(&s->qmi_power_filter[9][5], g_bytes_unref);
    for (unsigned i = 0; i < 64; i++)
        g_queue_push_tail(&s->rx[9], g_bytes_new_static("x", 1));
    assert(!mdm_wms_receive_indicate(s, 9, 3, 0) &&
           !s->wms_inbox.awaiting_ack[9][3]);
    g_queue_clear_full(&s->rx[9], (GDestroyNotify)g_bytes_unref);
    assert(mdm_wms_receive_indicate(s, 9, 3, 0));
    mdm_read(s, 9, out, sizeof(out));
    /* TransferAndAcknowledge is explicitly different from TransferOnly. */
    strcpy(sms->pdu, "07910100000000F000049121430000625060010203000141");
    s->wms_routes[5] = 3;
    mdm_wms_received(s, 0);
    assert(mdm_wms_receive_indicate(s, 9, 3, 0));
    n = mdm_read(s, 9, out, sizeof(out));
    v = find_tlv(out + 13, n - 13, 0x11, &len);
    assert(v && v[0] == 1 && len == 24 && lduw_le_p(v + 6) == 16);
    assert(!s->wms_inbox.awaiting_ack[9][3]);
    /* Stored notification and RawRead share bytes, with unread/read tags. */
    s->wms_routes[4] = 1;
    s->wms_routes[5] = 1;
    mdm_wms_received(s, 0);
    assert(mdm_wms_receive_indicate(s, 9, 3, 0));
    n = mdm_read(s, 9, out, sizeof(out));
    v = find_tlv(out + 13, n - 13, 0x10, &len);
    assert(v && len == 5 && v[0] == 1 && !ldl_le_p(v + 1));
    uint8_t index[5] = {1, 0, 0, 0, 0};
    r = sms_request(s, 9, 3, 0x22, index, 5);
    assert(sms_result_ok(r));
    v = find_tlv(r->data + 13, r->len - 13, 1, &len);
    assert(v && v[0] == 1 && v[1] == 6 && len == 28);
    g_byte_array_unref(r);
    r = sms_request(s, 9, 3, 0x22, index, 5);
    v = find_tlv(r->data + 13, r->len - 13, 1, &len);
    assert(v && !v[0]);
    g_byte_array_unref(r);
    index[0] = 0;
    r = sms_request(s, 9, 3, 0x22, index, 5);
    assert(!sms_result_ok(r));
    g_byte_array_unref(r);
    /* CTL release clears exactly that client's pending/ack state. */
    s->wms_inbox.pending[9][3] = 1;
    s->wms_inbox.awaiting_ack[9][3] = 1;
    uint8_t release[] = {1, 15, 0, 0, 0, 0, 0, 1, 0x23, 0, 5, 0, 1, 2, 0, 5, 3};
    stw_le_p(release + 1, sizeof(release) - 1);
    r = mdm_qmi_reply(s, 9, release, sizeof(release));
    assert(r);
    g_byte_array_unref(r);
    assert(!s->wms_inbox.pending[9][3] && !s->wms_inbox.awaiting_ack[9][3]);
    /* Explicit discard and malformed PDU produce no message indication. */
    s->wms_routes[5] = 0;
    mdm_wms_received(s, 0);
    assert(!s->wms_inbox.valid[0]);
    strcpy(sms->pdu, "00zz");
    mdm_wms_received(s, 0);
    assert(!s->wms_inbox.valid[0]);
    g_free(s);
}

int main(void)
{
    test_incoming_sms();
    uart_reply = g_byte_array_new();
    MDM9x15State s = {0};
    IosBasebandState backend = {0};
    s.baseband = &backend;
    uart_routed = false;
    assert(!mdm_qmi_output(&s, MDM_INTERFACES, (const uint8_t[]){1}, 1));
    assert(!uart_reply->len);
    uart_routed = true;
    for (unsigned i = 0; i < 13; i++)
        s.tx[i] = g_byte_array_new();
    uint8_t version[] = {1, 11, 0, 0, 0, 0, 0, 0x31, 0x21, 0, 0, 0}, out[1024];
    mdm_receive(&s, 3, version, 2);
    assert(g_queue_is_empty(&s.rx[3]));
    mdm_receive(&s, 3, version + 2, sizeof version - 2);
    unsigned n = mdm_read(&s, 3, out, sizeof out);
    assert(n > 12 && out[3] == 0x80 && out[6] == 1 && out[7] == 0x31);
    assert(lduw_le_p(out + 1) == n - 1 && lduw_le_p(out + 10) == n - 12);
    size_t size = 0;
    const uint8_t *result = find_tlv(out + 12, n - 12, 2, &size);
    assert(result && size == 4 && !result[0]);
    assert(g_queue_is_empty(&s.rx[0]));
    uint8_t alloc[] = {1, 15, 0, 0, 0, 0, 0, 9, 0x22, 0, 4, 0, 1, 1, 0, 2};
    mdm_receive(&s, 3, alloc, sizeof alloc);
    n = mdm_read(&s, 3, out, sizeof out);
    result = find_tlv(out + 12, n - 12, 1, &size);
    assert(result && size == 2 && result[0] == 2 && result[1] == 1);
    mdm_receive(&s, 3, alloc, sizeof alloc);
    n = mdm_read(&s, 3, out, sizeof out);
    result = find_tlv(out + 12, n - 12, 1, &size);
    assert(result && result[1] == 2);
    /* Allocated VOICE listeners receive incoming status without a VOICE RPC. */
    MDM9x15State *listeners = g_new0(MDM9x15State, 1);
    IosBasebandState listener_backend = {0};
    listeners->baseband = &listener_backend;
    listeners->ready = true;
    MdmDownloadState listener_download = {.runtime = true};
    listeners->download = &listener_download;
    uint8_t voice_alloc[] = {1,    15, 0, 0, 0, 0, 0, 9,
                             0x22, 0,  4, 0, 1, 1, 0, 9};
    GByteArray *allocated =
        mdm_qmi_reply(listeners, 5, voice_alloc, sizeof(voice_alloc));
    assert(allocated);
    g_byte_array_unref(allocated);
    assert(listeners->voice_client[5][1] && listeners->voice_dirty[5][1]);
    assert(!listeners->voice_client[3][1]);
    listener_backend.bb.calls[0] = (IosBbCall){
        .used = true, .mt = true, .id = 42, .stat = IOS_BB_CALL_INCOMING};
    snprintf(listener_backend.bb.calls[0].number,
             sizeof(listener_backend.bb.calls[0].number), "+12025550199");
    mdm_voice_changed(listeners, &listener_backend.bb.calls[0]);
    mdm_qmi_events(listeners);
    n = mdm_read(listeners, 5, out, sizeof(out));
    assert(n > 13 && out[4] == 9 && out[5] == 1 && lduw_le_p(out + 9) == 0x2e);
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 8 && result[0] == 1 && result[1] == 42 &&
           result[2] == 2 && result[4] == 2);
    allocated = mdm_qmi_reply(listeners, 13, voice_alloc, sizeof(voice_alloc));
    assert(allocated);
    g_byte_array_unref(allocated);
    assert(listeners->voice_client[13][2] && !listeners->voice_client[5][2]);
    uint8_t voice_release[] = {1, 16, 0, 0, 0, 0, 0, 10, 0x23,
                               0, 5,  0, 1, 2, 0, 9, 1};
    allocated =
        mdm_qmi_reply(listeners, 5, voice_release, sizeof(voice_release));
    assert(allocated);
    g_byte_array_unref(allocated);
    assert(!listeners->voice_client[5][1] && !listeners->voice_dirty[5][1] &&
           !listeners->voice_events[5][1] && listeners->voice_client[13][2]);
    /* Native iOS8 client requests: disable Call Notification, enable Handover.
  * Settings are per transport/client and absent TLVs retain the old values. */
    uint8_t voice_reg[] = {1, 16, 0, 0, 9,    2, 0, 1, 0,
                           3, 0,  4, 0, 0x13, 1, 0, 0};
    allocated = mdm_qmi_reply(listeners, 13, voice_reg, sizeof(voice_reg));
    assert(allocated);
    assert(sms_result_ok(allocated));
    g_byte_array_unref(allocated);
    assert(!(listeners->voice_events[13][2] & 8) &&
           !listeners->voice_dirty[13][2]);
    mdm_voice_changed(listeners, &listener_backend.bb.calls[0]);
    assert(!listeners->voice_dirty[13][2]);
    voice_reg[13] = 0x14;
    voice_reg[16] = 1;
    allocated = mdm_qmi_reply(listeners, 13, voice_reg, sizeof(voice_reg));
    assert(allocated);
    assert(sms_result_ok(allocated));
    g_byte_array_unref(allocated);
    assert(listeners->voice_events[13][2] == 16);
    /* A bad boolean must not partially change subscription state. */
    voice_reg[13] = 0x13;
    voice_reg[16] = 2;
    allocated = mdm_qmi_reply(listeners, 13, voice_reg, sizeof(voice_reg));
    assert(allocated);
    assert(!sms_result_ok(allocated));
    g_byte_array_unref(allocated);
    assert(listeners->voice_events[13][2] == 16);
    voice_reg[16] = 1;
    allocated = mdm_qmi_reply(listeners, 13, voice_reg, sizeof(voice_reg));
    assert(allocated);
    assert(sms_result_ok(allocated));
    g_byte_array_unref(allocated);
    assert(listeners->voice_events[13][2] == 24 &&
           listeners->voice_dirty[13][2]);
    g_free(listeners);
    alloc[13] = 3;
    mdm_receive(&s, 3, alloc, sizeof alloc);
    assert(g_queue_is_empty(&s.rx[3]));
    uint8_t unknown[] = {1, 12, 0, 0, 0x7f, 1, 0, 0x34, 0x12, 0xff, 0xff, 0, 0};
    mdm_receive(&s, 1, unknown, sizeof unknown);
    n = mdm_read(&s, 1, out, sizeof out);
    assert(out[6] == 2 && out[7] == 0x34 && out[8] == 0x12);
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] == 1 && result[2] == 0x5e);
    uint8_t format[] = {1, 20, 0, 0, 0, 0,    0, 11, 0x26, 0, 9,
                        0, 1,  1, 0, 0, 0x10, 2, 0,  2,    0};
    mdm_receive(&s, 1, format, sizeof format);
    n = mdm_read(&s, 1, out, sizeof out);
    result = find_tlv(out + 12, n - 12, 0x10, &size);
    assert(result && size == 2 && lduw_le_p(result) == 2);
    assert(s.qmi_protocol[1] == 2 && !s.qmi_protocol[3]);
    format[15] = 1;
    mdm_receive(&s, 1, format, sizeof format);
    n = mdm_read(&s, 1, out, sizeof out);
    result = find_tlv(out + 12, n - 12, 2, &size);
    assert(result && result[0] == 1 && s.qmi_protocol[1] == 2);
    uint8_t power[] = {1, 18, 0, 0, 0, 0, 0, 12, 0x2a, 0,
                       7, 0,  1, 4, 0, 1, 0, 0,  0};
    mdm_receive(&s, 3, power, sizeof power);
    n = mdm_read(&s, 3, out, sizeof out);
    assert(n == 19 && s.qmi_power_save[3] == 1 && !s.qmi_power_save[1]);
    power[8] = 0x2b;
    power[1] = 11;
    power[10] = 0;
    mdm_receive(&s, 3, power, 12);
    n = mdm_read(&s, 3, out, sizeof out);
    result = find_tlv(out + 12, n - 12, 1, &size);
    assert(result && size == 4 && ldl_le_p(result) == 1);
    /* iOS8 CTL0x29: profile1/service9 and uncounted u16 indication IDs. */
    uint8_t filter[] = {1, 24, 0, 0, 0, 0, 0, 13,   0x29, 0, 13,   0, 1,
                        5, 0,  1, 0, 0, 0, 9, 0x11, 2,    0, 0x2e, 0};
    mdm_receive(&s, 3, filter, sizeof(filter));
    n = mdm_read(&s, 3, out, sizeof(out));
    result = find_tlv(out + 12, n - 12, 2, &size);
    assert(result && !result[0]);
    uint8_t indication[13] = {1, 12, 0, 0x80, 9, 1, 4, 0, 0, 0x2e, 0, 0, 0};
    assert(mdm_power_filter_allows(&s, 3, indication, sizeof(indication)));
    indication[9] = 0x22;
    assert(!mdm_power_filter_allows(&s, 3, indication, sizeof(indication)));
    assert(mdm_qmi_output(&s, 3, indication, sizeof(indication)));
    assert(g_queue_is_empty(&s.rx[3]));
    assert(mdm_power_filter_allows(&s, 1, indication, sizeof(indication)));
    s.qmi_power_save[3] = 0;
    assert(mdm_power_filter_allows(&s, 3, indication, sizeof(indication)));
    s.qmi_power_save[3] = 1;
    indication[6] = 2;
    assert(mdm_power_filter_allows(&s, 3, indication, sizeof(indication)));
    indication[6] = 4;
    /* Invalid list length/profile preserve the previous filter. */
    filter[1] = 23;
    filter[10] = 12;
    filter[21] = 1;
    allocated = mdm_qmi_reply(&s, 3, filter, sizeof(filter) - 1);
    result = find_tlv(allocated->data + 12, allocated->len - 12, 2, &size);
    assert(result && result[0] && result[2] == 0x30);
    g_byte_array_unref(allocated);
    filter[1] = 24;
    filter[10] = 13;
    filter[21] = 2;
    filter[15] = 2;
    allocated = mdm_qmi_reply(&s, 3, filter, sizeof(filter));
    result = find_tlv(allocated->data + 12, allocated->len - 12, 2, &size);
    assert(result && result[0] && result[2] == 0x5e);
    g_byte_array_unref(allocated);
    assert(!mdm_power_filter_allows(&s, 3, indication, sizeof(indication)));
    /* Empty list blocks service indications. USB reset preserves UART policy. */
    filter[15] = 1;
    filter[1] = 22;
    filter[10] = 11;
    filter[21] = 0;
    allocated = mdm_qmi_reply(&s, 13, filter, sizeof(filter) - 2);
    assert(allocated);
    g_byte_array_unref(allocated);
    s.qmi_power_save[13] = 1;
    assert(!mdm_power_filter_allows(&s, 13, indication, sizeof(indication)));
    mdm_power_filter_reset(&s, 13);
    assert(!s.qmi_power_filter[3][9] && s.qmi_power_filter[13][9]);
    mdm_power_filter_reset(&s, 14);
    assert(!s.qmi_power_filter[13][9]);
    version[10] = 1;
    mdm_receive(&s, 3, version, sizeof version);
    assert(g_queue_is_empty(&s.rx[3]));
    /* Same native CTL Sync over split reliable UART payloads. */
    GByteArray *stream = g_byte_array_new();
    const uint8_t sync[] = {1, 11, 0, 0, 0, 0, 0, 0x0a, 0x27, 0, 0, 0};
    mdm_qmux_receive(&s, stream, MDM_INTERFACES, sync, 5);
    assert(!uart_reply->len);
    mdm_qmux_receive(&s, stream, MDM_INTERFACES, sync + 5, sizeof(sync) - 5);
    assert(!stream->len && uart_reply->len == 19 &&
           uart_reply->data[7] == 0x0a);
    assert(lduw_le_p(uart_reply->data + 8) == 0x27);
    result = find_tlv(uart_reply->data + 12, uart_reply->len - 12, 2, &size);
    assert(result && size == 4 && !result[0]);
    for (unsigned i = 0; i < 12; i++)
        assert(!mdm_qmi_reply(&s, MDM_INTERFACES, sync, i));
    assert(g_queue_is_empty(&s.rx[MDM_INTERFACES - 1]));
    g_byte_array_unref(stream);
    mdm_qmi_ready(&s);
    const unsigned controls[] = {1, 3, 5, 7, 9, 11};
    for (unsigned j = 0; j < 6; j++) {
        unsigned i = controls[j];
        n = mdm_read(&s, i, out, sizeof out);
        assert(n == 12 && out[3] == 0x80 && out[4] == 0 && out[5] == 0 &&
               out[6] == 2 && out[7] == 0);
        assert(lduw_le_p(out + 8) == 0x27 && lduw_le_p(out + 10) == 0);
    }
    mdm_qmi_ready(&s);
    for (unsigned j = 0; j < 6; j++)
        assert(g_queue_is_empty(&s.rx[controls[j]]));
    backend.bb.sim_present = true;
    backend.bb.registered = true;
    backend.bb.cfun = 1;
    backend.bb.pin_left = 3;
    backend.bb.puk_left = 10;
    backend.bb.signal_dbm = -65;
    strcpy(backend.bb.plmn, "00101");
    strcpy(backend.bb.operator_long, "Light Touch");
    strcpy(backend.bb.imsi, "001010000000001");
    strcpy(backend.bb.iccid, "89001010000000000001");
    uint8_t service_req[] = {1, 12, 0, 0, 11, 1, 0, 1, 0, 0x2f, 0, 0, 0};
    mdm_receive(&s, 5, service_req, sizeof service_req);
    n = mdm_read(&s, 5, out, sizeof out);
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size == 29 && result[9] == 1 && result[14] == 1 &&
           result[16] == 7 && result[23] == 3);
    backend.bb.pin_on = true;
    mdm_receive(&s, 5, service_req, sizeof service_req);
    n = mdm_read(&s, 5, out, sizeof out);
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && result[16] == 2 && result[23] == 1);
    backend.bb.sim_present = false;
    mdm_receive(&s, 5, service_req, sizeof service_req);
    n = mdm_read(&s, 5, out, sizeof out);
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size == 15 && !result[9] && !result[14]);
    backend.bb.sim_present = true;
    backend.bb.pin_on = false;
    service_req[4] = 3;
    service_req[9] = 0x24;
    mdm_receive(&s, 5, service_req, sizeof service_req);
    n = mdm_read(&s, 5, out, sizeof out);
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 6 && result[0] == 1 && result[5] == 4);
    result = find_tlv(out + 13, n - 13, 0x12, &size);
    assert(result && size == 16 && lduw_le_p(result) == 1 &&
           lduw_le_p(result + 2) == 1 && result[4] == 11);
    backend.bb.registered = false;
    mdm_receive(&s, 5, service_req, sizeof service_req);
    n = mdm_read(&s, 5, out, sizeof out);
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 5 && !result[0] && !result[4]);
    service_req[4] = 2;
    service_req[9] = 0x3c;
    mdm_receive(&s, 5, service_req, sizeof service_req);
    n = mdm_read(&s, 5, out, sizeof out);
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 20 && !memcmp(result, backend.bb.iccid, size));
    /* Original native EF_IMSI ReadTransparent, then offset and path guards. */
    uint8_t file_req[] = {1, 34,   0,    0,    11, 1, 0, 9, 0, 0x20, 0,    22,
                          0, 1,    2,    0,    0,  0, 2, 7, 0, 7,    0x6f, 4,
                          0, 0x3f, 0x20, 0x7f, 3,  4, 0, 0, 0, 0,    0};
    mdm_receive(&s, 5, file_req, sizeof(file_req));
    n = mdm_read(&s, 5, out, sizeof out);
    result = find_tlv(out + 13, n - 13, 0x11, &size);
    const uint8_t imsi_file[] = {8, 9, 0x10, 0x10, 0, 0, 0, 0, 0x10};
    assert(result && size == 11 && lduw_le_p(result) == 9 &&
           !memcmp(result + 2, imsi_file, 9));
    file_req[31] = 2;
    file_req[33] = 3;
    mdm_receive(&s, 5, file_req, sizeof(file_req));
    n = mdm_read(&s, 5, out, sizeof out);
    result = find_tlv(out + 13, n - 13, 0x11, &size);
    assert(result && size == 5 && !memcmp(result + 2, imsi_file + 2, 3));
    file_req[31] = 20;
    mdm_receive(&s, 5, file_req, sizeof(file_req));
    n = mdm_read(&s, 5, out, sizeof out);
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] == 1 && result[2] == 48);
    file_req[31] = 0;
    file_req[33] = 0;
    file_req[26] = 0x10;
    mdm_receive(&s, 5, file_req, sizeof(file_req));
    n = mdm_read(&s, 5, out, sizeof out);
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] == 1 && result[2] == 80);
    s.download = g_new0(MdmDownloadState, 1);
    s.download->runtime = true;
    version[10] = 0;
    mdm_receive(&s, 1, version, sizeof version);
    n = mdm_read(&s, 1, out, sizeof out);
    assert(n > 12 && out[6] == 1 && out[7] == 0x31 &&
           lduw_le_p(out + 8) == 0x21);
    /* Native UART NAS client5 enables SystemInfo, not ServingSystem. */
    s.ready = true;
    backend.bb.registered = true;
    uint8_t reg[] = {1, 16, 0, 0, 3, 5, 0, 1, 0, 3, 0, 4, 0, 0x18, 1, 0, 1};
    mdm_qmi(&s, 13, reg, sizeof(reg));
    g_byte_array_set_size(uart_reply, 0);
    mdm_qmi_events(&s);
    assert(uart_reply->len > 13 && uart_reply->data[4] == 3 &&
           uart_reply->data[5] == 5 && uart_reply->data[6] == 4);
    assert(lduw_le_p(uart_reply->data + 9) == 0x4e);
    result = find_tlv(uart_reply->data + 13, uart_reply->len - 13, 0x12, &size);
    assert(result && size == 3 && result[0] == 2 && result[1] == 2 &&
           result[2] == 1);
    result = find_tlv(uart_reply->data + 13, uart_reply->len - 13, 0x17, &size);
    assert(result && size == 30 && !memcmp(result + 20, "00101", 5) &&
           result[25] == 0xff);
    unsigned event_bytes = uart_reply->len;
    mdm_qmi_events(&s);
    assert(uart_reply->len == event_bytes);
    backend.bb.registered = false;
    mdm_qmi_events(&s);
    assert(uart_reply->len > event_bytes);
    const uint8_t *lost = uart_reply->data + event_bytes;
    result =
        find_tlv(lost + 13, uart_reply->len - event_bytes - 13, 0x12, &size);
    assert(result && !result[0] && !result[1] && !result[2]);
    g_byte_array_set_size(uart_reply, 0);
    backend.bb.registered = true;
    mdm_qmi_events(&s);
    assert(uart_reply->len > 13);
    reg[16] = 0;
    mdm_qmi(&s, 13, reg, sizeof(reg));
    g_byte_array_set_size(uart_reply, 0);
    backend.bb.registered = false;
    mdm_qmi_events(&s);
    assert(!uart_reply->len);
    /* Raw Send's GSM PDU is submitted to the shared model, not just ACKed. */
    uint8_t sms[] = {1,    34,   0,    0, 5, 1,    0, 1, 0, 0x20, 0,   22,
                     0,    1,    19,   0, 6, 16,   0, 0, 1, 0,    11,  0x81,
                     0x41, 0x51, 0x55, 5, 1, 0xf0, 0, 0, 2, 0xc8, 0x34};
    mdm_receive(&s, 5, sms, sizeof(sms));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 2 && lduw_le_p(result) == 7 &&
           sms_submissions == 1);
    sms[17] = 15;
    mdm_receive(&s, 5, sms, sizeof(sms));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] == 1 && result[2] == 48 && sms_submissions == 1);
    /* A genuine PDS session reset, required by native locationd startup. */
    uint8_t sv_report[] = {1,    16,   0, 0, 6, 1, 0, 1, 0,
                           0x57, 0x55, 4, 0, 1, 1, 0, 1};
    mdm_receive(&s, 5, sv_report, sizeof(sv_report));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && !result[0] && (s.pds_events[5][1] & MDM_PDS_SV_IN_FIX));
    assert(!s.pds_events[3][1] && !s.pds_events[5][2]);
    sv_report[16] = 2;
    mdm_receive(&s, 5, sv_report, sizeof(sv_report));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] && result[2] == 48 &&
           (s.pds_events[5][1] & MDM_PDS_SV_IN_FIX));
    sv_report[16] = 0;
    mdm_receive(&s, 5, sv_report, sizeof(sv_report));
    mdm_read(&s, 5, out, sizeof(out));
    assert(!(s.pds_events[5][1] & MDM_PDS_SV_IN_FIX));
    backend.bb.gps.ch = 7;
    service_req[4] = 6;
    service_req[9] = 0;
    mdm_receive(&s, 5, service_req, sizeof(service_req));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && !result[0] && backend.bb.gps.ch == -1);
    service_req[9] = 0x36;
    mdm_receive(&s, 5, service_req, sizeof(service_req));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 2 && result[0] == 1 && result[1] == 0);
    service_req[9] = 0x07;
    service_req[10] = 0xaa;
    mdm_receive(&s, 5, service_req, sizeof(service_req));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] == 1 && result[2] == 71);
    uint8_t force[] = {1, 16, 0, 0, 6, 1, 0, 1, 0, 0x49, 0, 4, 0, 1, 1, 0, 1};
    backend.bb.gps.ch = 7;
    mdm_receive(&s, 5, force, sizeof(force));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && !result[0] && s.pds_force_off && backend.bb.gps.ch == -1);
    force[9] = 0x21;
    mdm_receive(&s, 5, force, sizeof(force));
    mdm_read(&s, 5, out, sizeof(out));
    assert(s.pds_enabled && !s.pds_force_off);
    service_req[9] = 0x20;
    service_req[10] = 0;
    mdm_receive(&s, 5, service_req, sizeof(service_req));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 2 && result[0] == 1 && result[1] == 1);
    force[9] = 0x31;
    mdm_receive(&s, 5, force, sizeof(force));
    mdm_read(&s, 5, out, sizeof(out));
    mdm_receive(&s, 5, service_req, sizeof(service_req));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 2 && result[0] == 1 && result[1] == 2);
    force[9] = 0x21;
    force[16] = 0;
    mdm_receive(&s, 5, force, sizeof(force));
    mdm_read(&s, 5, out, sizeof(out));
    mdm_receive(&s, 5, service_req, sizeof(service_req));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 2 && !result[0] && result[1] == 1);
    uint8_t tracking[] = {1, 25, 0, 0, 6,    1, 0, 1, 0, 0x2a, 0, 13, 0,
                          1, 10, 0, 0, 0xff, 2, 0, 0, 0, 0x90, 1, 0,  0};
    mdm_receive(&s, 5, tracking, sizeof(tracking));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && !result[0]);
    service_req[9] = 0x29;
    mdm_receive(&s, 5, service_req, sizeof(service_req));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 10 && !memcmp(result, tracking + 16, 10));
    tracking[18] = 0;
    mdm_receive(&s, 5, tracking, sizeof(tracking));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] && result[2] == 48 &&
           s.pds_tracking_config[2] == 2);
    force[9] = 0x49;
    force[16] = 2;
    mdm_receive(&s, 5, force, sizeof(force));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] == 1 && result[2] == 48 && !s.pds_force_off);
    uint8_t nav[] = {1, 23,   0, 0, 6, 1, 0,    1,    0,    0x57, 0, 11,
                     0, 0x10, 4, 0, 0, 0, 0x80, 0x3d, 0x13, 1,    0, 0};
    mdm_receive(&s, 5, nav, sizeof(nav));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && !result[0]);
    service_req[9] = 0x56;
    mdm_receive(&s, 5, service_req, sizeof(service_req));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size == 4 && ldl_le_p(result) == 0x3d800000);
    nav[13] = 0x13; /* Invalid scalar size; the prior settings must survive. */
    mdm_receive(&s, 5, nav, sizeof(nav));
    n = mdm_read(&s, 5, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] == 1 && s.pds_navigation[0] == 0x3d800000);
    service_req[9] = 0x2d;
    mdm_receive(&s, 5, service_req, sizeof(service_req));
    mdm_read(&s, 5, out, sizeof(out));
    assert(s.pds_xtra_pending);
    mdm_pds_reset(&s);
    assert(!s.pds_navigation_mask && !s.pds_xtra_pending && !s.pds_force_off);
    /* Operating mode report and set online/low-power use shared CFUN state. */
    backend.bb.registered = true;
    uint8_t event_req[] = {1, 16, 0, 0, 2,    1, 0, 1, 0,
                           1, 0,  4, 0, 0x14, 1, 0, 1};
    mdm_qmi(&s, 5, event_req, sizeof(event_req));
    mdm_read(&s, 5, out, sizeof(out));
    mdm_qmi_events(&s);
    n = mdm_read(&s, 5, out, sizeof(out));
    assert(n == 17 && out[6] == 4 && out[9] == 1 && out[13] == 0x14 &&
           out[16] == 0);
    uint8_t mode_req[] = {1,    16, 0, 0, 2, 1, 0, 2, 0,
                          0x2e, 0,  4, 0, 1, 1, 0, 1};
    mdm_qmi(&s, 5, mode_req, sizeof(mode_req));
    mdm_read(&s, 5, out, sizeof(out));
    assert(backend.bb.cfun == 4);
    mdm_qmi_events(&s);
    n = mdm_read(&s, 5, out, sizeof(out));
    assert(n == 17 && out[16] == 1);
    mode_req[16] = 0;
    mdm_qmi(&s, 5, mode_req, sizeof(mode_req));
    mdm_read(&s, 5, out, sizeof(out));
    assert(backend.bb.cfun == 1);
    mdm_qmi_events(&s);
    n = mdm_read(&s, 5, out, sizeof(out));
    assert(n == 17 && out[16] == 0);
    /* Voice uses standard TLV10 call IDs and publishes QMI states, not AT. */
    uint8_t dial[] = {1, 22, 0, 0, 9,   4,   0,   1,   0,   0x20, 0,  10,
                      0, 1,  7, 0, '8', '6', '7', '5', '3', '0',  '9'};
    mdm_qmi(&s, 7, dial, sizeof(dial));
    n = mdm_read(&s, 7, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size == 1 && *result == 42);
    assert(!strcmp(backend.bb.last_dialed, "8675309"));
    dial[16] = 0;
    mdm_qmi(&s, 7, dial, sizeof(dial));
    n = mdm_read(&s, 7, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] == 1 && result[2] == 48);
    backend.bb.calls[0] =
        (IosBbCall){.used = true, .id = 42, .stat = IOS_BB_CALL_ACTIVE};
    strcpy(backend.bb.calls[0].number, "8675309");
    mdm_voice_changed(&s, &backend.bb.calls[0]);
    assert(s.voice_dirty[7][4]);
    assert(mdm_voice_indicate(&s, 7, 4));
    n = mdm_read(&s, 7, out, sizeof(out));
    const uint8_t expected[] = {1, 42, 3, 0, 1, 2, 0, 0};
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(out[4] == 9 && out[5] == 4 && out[6] == 4 &&
           lduw_le_p(out + 9) == 0x2e);
    assert(result && size == sizeof(expected) &&
           !memcmp(result, expected, size));
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    const uint8_t expected_number[] = {1,   42,  0,   7,   '8', '6',
                                       '7', '5', '3', '0', '9'};
    assert(result && size == sizeof(expected_number) &&
           !memcmp(result, expected_number, size));
    backend.bb.calls[0].stat = IOS_BB_CALL_RELEASED;
    mdm_voice_changed(&s, &backend.bb.calls[0]);
    backend.bb.calls[0].used = false;
    assert(s.voice_ended[0].used);
    assert(mdm_voice_indicate(&s, 7, 4));
    n = mdm_read(&s, 7, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 8 && result[2] == 9);
    uint8_t end[] = {1, 16, 0, 0, 9, 4, 0, 2, 0, 0x21, 0, 4, 0, 1, 1, 0, 42};
    end[9] = 0x22;
    mdm_qmi(&s, 7, end, sizeof(end));
    n = mdm_read(&s, 7, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size == 1 && *result == 42 && answers == 1);
    end[16] = 43;
    mdm_qmi(&s, 7, end, sizeof(end));
    n = mdm_read(&s, 7, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] && result[2] == 47 && answers == 2);
    end[13] = 3;
    mdm_qmi(&s, 7, end, sizeof(end));
    n = mdm_read(&s, 7, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] && result[2] == 48 && answers == 2);
    end[13] = 1;
    end[16] = 42;
    end[9] = 0x21;
    mdm_qmi(&s, 7, end, sizeof(end));
    n = mdm_read(&s, 7, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size == 1 && *result == 42);
    end[16] = 43;
    mdm_qmi(&s, 7, end, sizeof(end));
    n = mdm_read(&s, 7, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] && result[2] == 47);
    uint8_t manage[] = {1, 16, 0, 0, 9, 4, 0, 3, 0, 0x31, 0, 4, 0, 1, 1, 0, 2};
    mdm_qmi(&s, 7, manage, sizeof(manage));
    n = mdm_read(&s, 7, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && !result[0]);
    manage[16] = 7;
    mdm_qmi(&s, 7, manage, sizeof(manage));
    n = mdm_read(&s, 7, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] && result[2] == 94);
    manage[16] = 9;
    mdm_qmi(&s, 7, manage, sizeof(manage));
    n = mdm_read(&s, 7, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] && result[2] == 48);
    /* Query and unsolicited PDS state must agree; iOS checks both. */
    s.pds_enabled = true;
    s.pds_autotracking = true;
    assert(mdm_pds_indicate(&s, 11, 3));
    n = mdm_read(&s, 11, out, sizeof(out));
    const uint8_t pds_active[] = {1,    17, 0, 0x80, 6, 3, 4, 0, 0,
                                  0x20, 0,  5, 0,    1, 2, 0, 1, 2};
    assert(n == sizeof(pds_active) && !memcmp(out, pds_active, n));
    g_byte_array_set_size(uart_reply, 0);
    assert(mdm_pds_indicate(&s, MDM_INTERFACES, 3));
    assert(uart_reply->len == sizeof(pds_active) &&
           !memcmp(uart_reply->data, pds_active, sizeof(pds_active)));
    s.pds_autotracking = false;
    assert(mdm_pds_indicate(&s, 11, 3));
    n = mdm_read(&s, 11, out, sizeof(out));
    assert(n == 18 && out[16] == 1 && out[17] == 1);
    for (unsigned i = 0; i < 64; i++)
        g_queue_push_tail(&s.rx[11], g_bytes_new_static("x", 1));
    assert(!mdm_pds_indicate(&s, 11, 3));
    g_queue_clear_full(&s.rx[11], (GDestroyNotify)g_bytes_unref);
    MDM9x15State *notify = g_new0(MDM9x15State, 1);
    notify->baseband = &backend;
    notify->download = s.download;
    notify->ready = true;
    notify->pds_client[11][3] = true;
    notify->pds_enabled = true;
    notify->pds_autotracking = true;
    mdm_qmi_events(notify);
    n = mdm_read(notify, 11, out, sizeof(out));
    assert(n == 18 && out[16] == 1 && out[17] == 2 &&
           !notify->pds_dirty[11][3]);
    mdm_qmi_events(notify);
    assert(g_queue_is_empty(&notify->rx[11]));
    for (unsigned i = 0; i < 64; i++)
        g_queue_push_tail(&notify->rx[11], g_bytes_new_static("x", 1));
    notify->pds_autotracking = false;
    mdm_qmi_events(notify);
    assert(notify->pds_dirty[11][3]);
    g_queue_clear_full(&notify->rx[11], (GDestroyNotify)g_bytes_unref);
    mdm_qmi_events(notify);
    n = mdm_read(notify, 11, out, sizeof(out));
    assert(n == 18 && out[17] == 1 && !notify->pds_dirty[11][3]);
    /* Parsed position and NMEA originate from the same shared fix. */
    notify->pds_autotracking = true;
    notify->pds_events[11][3] = 5;
    backend.bb.gps.present = true;
    assert(ios_bb_gps_set(&backend.bb.gps, "37.3349,-122.009,30,1.4,90,5"));
    backend.bb.wall_offset_ms = 123;
    stl_le_p(notify->pds_tracking_config + 2, 2);
    mdm_qmi_events(notify);
    n = mdm_read(notify, 11, out, sizeof(out));
    assert(n == 18); /* state transition precedes the fix */
    n = mdm_read(notify, 11, out, sizeof(out));
    assert(n > 13 && out[4] == 6 && out[5] == 3 && out[6] == 4 &&
           lduw_le_p(out + 9) == 1);
    result = find_tlv(out + 13, n - 13, 0x12, &size);
    assert(result && size == 1 &&
           *result == 0); /* success is required for a publishable fix */
    result = find_tlv(out + 13, n - 13, 0x13, &size);
    assert(result && size == 105 && lduw_le_p(result + 4) == 1970 &&
           result[6] == 0 && result[8] == 1);
    assert(lduw_le_p(result + 12) == 123 && ldl_le_p(result + 15) == 123);
    uint8_t position_expected[8];
    double latitude = 37.3349, longitude = -122.009;
    uint64_t bits;
    memcpy(&bits, &latitude, 8);
    stq_le_p(position_expected, bits);
    assert(!memcmp(result + 27, position_expected, 8));
    memcpy(&bits, &longitude, 8);
    stq_le_p(position_expected, bits);
    assert(!memcmp(result + 35, position_expected, 8));
    assert((ldl_le_p(result) & (1u << 10)) && result[91] == 68 &&
           result[104] == 0);
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size > 100 &&
           g_strstr_len((const char *)result, size, "$GPGGA,000000.123") &&
           g_strstr_len((const char *)result, size, "$GPRMC,000000.123"));
    mdm_qmi_events(notify);
    assert(g_queue_is_empty(&notify->rx[11]));
    test_clock = 1999;
    mdm_qmi_events(notify);
    assert(g_queue_is_empty(&notify->rx[11]));
    test_clock = 2000;
    mdm_qmi_events(notify);
    assert(mdm_read(notify, 11, out, sizeof(out)) > 100);
    /* Backpressure must retry, without advancing the report deadline. */
    for (unsigned i = 0; i < 64; i++)
        g_queue_push_tail(&notify->rx[11], g_bytes_new_static("x", 1));
    test_clock = 4000;
    mdm_qmi_events(notify);
    assert(notify->pds_fix_due[11][3] == 4000);
    g_queue_clear_full(&notify->rx[11], (GDestroyNotify)g_bytes_unref);
    mdm_qmi_events(notify);
    assert(mdm_read(notify, 11, out, sizeof(out)) > 100 &&
           notify->pds_fix_due[11][3] == 6000);
    /* A UART client receives only its selected report form. */
    notify->pds_events[13][9] = 2;
    g_byte_array_set_size(uart_reply, 0);
    assert(mdm_pds_fix_indicate(notify, 13, 9));
    result = find_tlv(uart_reply->data + 13, uart_reply->len - 13, 0x11, &size);
    assert(result && size > 100 && !result[0] &&
           lduw_le_p(result + 1) == size - 3);
    assert(!find_tlv(uart_reply->data + 13, uart_reply->len - 13, 0x13, &size));
    assert(!find_tlv(uart_reply->data + 13, uart_reply->len - 13, 0x10, &size));
    assert(g_queue_is_empty(&notify->rx[5]));
    /* SV reports use the native 555a indication with mandatory timestamp and
  * counted measurements. This receiver has no RF measurements to report. */
    notify->pds_events[11][3] = MDM_PDS_SV_IN_FIX;
    assert(mdm_pds_fix_indicate(notify, 11, 3));
    n = mdm_read(notify, 11, out, sizeof(out));
    assert(n == 28 && out[4] == 6 && out[5] == 3 && out[6] == 4 &&
           lduw_le_p(out + 9) == 0x555a);
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 8 && ldl_le_p(result) == 4123);
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && size == 1 && !result[0]);
    assert(g_queue_is_empty(&notify->rx[11]));
    notify->pds_events[11][3] = 5;
    assert(ios_bb_gps_set(&backend.bb.gps, ""));
    test_clock = 6000;
    mdm_qmi_events(notify);
    assert(g_queue_is_empty(&notify->rx[11]));
    assert(!notify->pds_fix_due[11][3]);
    backend.bb.gps.present = false;
    test_clock = 0;
    g_free(notify);
    backend.bb.signal_dbm = -51;
    backend.bb.registered = true;
    backend.bb.cfun = 1;
    uint8_t strength[] = {1,    17, 0, 0, 3,    3, 0, 1,    0,
                          0x20, 0,  5, 0, 0x10, 2, 0, 0xeb, 0x10};
    mdm_qmi(&s, 11, strength, sizeof(strength));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 2 && (int8_t)result[0] == -51 && result[1] == 4);
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size == 4 && lduw_le_p(result) == 1 &&
           (int8_t)result[2] == -51 && result[3] == 4);
    result = find_tlv(out + 13, n - 13, 0x11, &size);
    assert(result && size == 4 && result[2] == 51 && result[3] == 4);
    /* Captured native WMS setup: config is retained and read back. */
    uint8_t routes[] = {1,  41, 0,   0,   5, 1, 0,   1,   0, 0x32, 0,
                        29, 0,  1,   26,  0, 6, 0,   0,   0, 255,  2,
                        0,  1,  255, 2,   0, 2, 255, 2,   0, 3,    255,
                        2,  0,  4,   255, 2, 0, 5,   255, 2};
    mdm_qmi(&s, 11, routes, sizeof(routes));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && !result[0] && s.wms_routes_len == 26);
    uint8_t wget[] = {1, 12, 0, 0, 5, 1, 0, 2, 0, 0x33, 0, 0, 0};
    mdm_qmi(&s, 11, wget, sizeof(wget));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 26 && !memcmp(result, routes + 16, 26));
    routes[21] = 9;
    mdm_qmi(&s, 11, routes, sizeof(routes));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] && result[2] == 48 && s.wms_routes[5] == 2);
    uint8_t wt[] = {1, 19, 0, 0, 5, 1, 0, 3, 0, 0x3a,
                    0, 7,  0, 1, 4, 0, 6, 0, 0, 0};
    mdm_qmi(&s, 11, wt, sizeof(wt));
    mdm_read(&s, 11, out, sizeof(out));
    wget[9] = 0x55;
    mdm_qmi(&s, 11, wget, sizeof(wget));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 4 && ldl_le_p(result) == 6);
    wget[9] = 0x43;
    mdm_qmi(&s, 11, wget, sizeof(wget));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 1 && !result[0]);
    uint8_t we[] = {1, 20, 0,    0, 5, 1, 0,    4, 0, 0x47, 0,
                    8, 0,  0x10, 1, 0, 1, 0x11, 1, 0, 1};
    mdm_qmi(&s, 11, we, sizeof(we));
    mdm_read(&s, 11, out, sizeof(out));
    assert(s.wms_events[11][1] == 6 && !s.wms_events[5][1]);
    wget[9] = 0x4d;
    mdm_qmi(&s, 11, wget, sizeof(wget));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 0x11, &size);
    assert(result && size == 1 && *result == 1);
    we[20] = 2;
    mdm_qmi(&s, 11, we, sizeof(we));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] && result[2] == 48 && s.wms_events[11][1] == 6);
    assert(mdm_wms_indicate(&s, 11, 1, 0x49));
    n = mdm_read(&s, 11, out, sizeof(out));
    const uint8_t wms_noims[] = {1,    16, 0, 0x80, 5, 1, 4, 0, 0,
                                 0x49, 0,  4, 0,    1, 1, 0, 0};
    assert(n == sizeof(wms_noims) && !memcmp(out, wms_noims, n));
    assert(mdm_wms_indicate(&s, 11, 1, 0x4b));
    n = mdm_read(&s, 11, out, sizeof(out));
    assert(n == 17 && out[9] == 0x4b && out[16] == 4);
    wget[9] = 0x4a;
    mdm_qmi(&s, 11, wget, sizeof(wget));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size == 1 && *result == 4);
    backend.bb.registered = false;
    assert(mdm_wms_indicate(&s, 11, 1, 0x4b));
    n = mdm_read(&s, 11, out, sizeof(out));
    assert(n == 17 && out[16] == 0);
    wget[9] = 0x4a;
    mdm_qmi(&s, 11, wget, sizeof(wget));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size == 1 && *result == 0);
    backend.bb.registered = true;
    /* Captured native WDS startup: family and subscriptions belong to one
  * client on one link; malformed multi-field writes are atomic. */
    uint8_t wds_family[] = {1,    16, 0, 0, 1, 9, 0, 3, 0,
                            0x4d, 0,  4, 0, 1, 1, 0, 4};
    mdm_qmi(&s, 9, wds_family, sizeof(wds_family));
    n = mdm_read(&s, 9, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && !result[0] && s.wds[9][9].family == 4);
    wds_family[16] = 6;
    mdm_qmi(&s, 3, wds_family, sizeof(wds_family));
    n = mdm_read(&s, 3, out, sizeof(out));
    assert(n && s.wds[3][9].family == 6 && s.wds[9][9].family == 4);
    wds_family[5] = 10;
    wds_family[16] = 8;
    mdm_qmi(&s, 9, wds_family, sizeof(wds_family));
    n = mdm_read(&s, 9, out, sizeof(out));
    assert(n && s.wds[9][10].family == 8 && s.wds[9][9].family == 4);
    wds_family[16] = 5;
    mdm_qmi(&s, 9, wds_family, sizeof(wds_family));
    n = mdm_read(&s, 9, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] && result[2] == 48 && s.wds[9][10].family == 8);
    uint8_t wds_bad_family[] = {1,    17, 0, 0, 1, 10, 0, 3, 0,
                                0x4d, 0,  5, 0, 1, 2,  0, 4, 0};
    mdm_qmi(&s, 9, wds_bad_family, sizeof(wds_bad_family));
    n = mdm_read(&s, 9, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(n && result && result[0] && result[2] == 48 &&
           s.wds[9][10].family == 8);
    uint8_t wds_events[] = {1,    24, 0, 0, 1,    9, 0, 3, 0,    1, 0, 12, 0,
                            0x13, 1,  0, 1, 0x15, 1, 0, 1, 0x1a, 1, 0, 1};
    mdm_qmi(&s, 9, wds_events, sizeof(wds_events));
    n = mdm_read(&s, 9, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && !result[0] && s.wds[9][9].events == 0x428);
    wds_events[16] = 0;
    wds_events[24] = 2;
    mdm_qmi(&s, 9, wds_events, sizeof(wds_events));
    n = mdm_read(&s, 9, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] && result[2] == 48 &&
           s.wds[9][9].events == 0x428);
    wds_events[21] = 0x16;
    wds_events[24] = 1;
    mdm_qmi(&s, 9, wds_events, sizeof(wds_events));
    n = mdm_read(&s, 9, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] && result[2] == 94 &&
           s.wds[9][9].events == 0x428);
    uint8_t wds_register[] = {1, 16, 0, 0, 1,    9, 0, 2, 0,
                              3, 0,  4, 0, 0x12, 1, 0, 1};
    mdm_qmi(&s, 9, wds_register, sizeof(wds_register));
    n = mdm_read(&s, 9, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && !result[0] && s.wds[9][9].extended_ip_events);
    uint8_t wds_reset[] = {1, 12, 0, 0, 1, 9, 0, 1, 0, 0, 0, 0, 0};
    mdm_qmi(&s, 9, wds_reset, sizeof(wds_reset));
    n = mdm_read(&s, 9, out, sizeof(out));
    assert(n && s.wds[9][9].family == 4 && !s.wds[9][9].events &&
           !s.wds[9][9].extended_ip_events && s.wds[9][10].family == 8);
    uint8_t wds_release[] = {1, 16, 0, 0, 0, 1, 0, 1, 0x23,
                             0, 5,  0, 1, 2, 0, 1, 10};
    mdm_qmi(&s, 9, wds_release, sizeof(wds_release));
    n = mdm_read(&s, 9, out, sizeof(out));
    assert(n && !s.wds[9][10].family && s.wds[3][9].family == 6 &&
           s.wds[9][9].family == 4);
    /* The successful setup reveals native queries for a packet call that
  * does not exist. Report that state without inventing a bearer/profile. */
    uint8_t wds_query[] = {1, 12, 0, 0, 1, 9, 0, 1, 0, 0x30, 0, 0, 0};
    mdm_qmi(&s, 9, wds_query, sizeof(wds_query));
    n = mdm_read(&s, 9, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] && result[2] == 15);
    assert(!find_tlv(out + 13, n - 13, 1, &size));
    wds_query[9] = 0x44;
    mdm_qmi(&s, 9, wds_query, sizeof(wds_query));
    n = mdm_read(&s, 9, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] && result[2] == 15);
    assert(!find_tlv(out + 13, n - 13, 1, &size) &&
           !find_tlv(out + 13, n - 13, 0x10, &size));
    wds_query[9] = 0x6b;
    mdm_qmi(&s, 9, wds_query, sizeof(wds_query));
    n = mdm_read(&s, 9, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && !result[0]);
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size == 2 && !result[0] && !result[1]);
    uint8_t mip_profile[] = {1,    16, 0, 0, 1, 9, 0, 1, 0,
                             0x3e, 0,  4, 0, 1, 1, 0, 0};
    mdm_qmi(&s, 9, mip_profile, sizeof(mip_profile));
    n = mdm_read(&s, 9, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] && result[2] == 10);
    assert(s.wds[9][9].family == 4 && s.wds[3][9].family == 6);
    /* Carrier files commit only after all segments; retries are idempotent.
  * A001 completion includes the requester; separate BSP clients are tested below. */
    uint8_t file0[] = {1, 8, 0, 2, 0, 0, 0, 2, 0, 'a', 'b'};
    assert(!mdm_bsp_file_write(&s, 13, 9, file0, sizeof(file0)));
    assert(!s.bsp_files[2].committed && s.bsp_files[2].staging->len == 2);
    file0[4] = 1; /* transport retry */
    assert(!mdm_bsp_file_write(&s, 13, 9, file0, sizeof(file0)));
    assert(s.bsp_files[2].staging->len == 2);
    file0[9] = 'x';
    assert(mdm_bsp_file_write(&s, 13, 9, file0, sizeof(file0)) == 48);
    file0[9] = 'a';
    assert(mdm_bsp_file_write(&s, 5, 9, file0, sizeof(file0)) == 48);
    file0[4] = 0;
    file0[5] = 1;
    file0[9] = 'c';
    file0[10] = 'd';
    assert(!mdm_bsp_file_write(&s, 13, 9, file0, sizeof(file0)));
    assert(!s.bsp_files[2].staging && s.bsp_files[2].pending);
    assert(s.bsp_files[2].committed->len == 4 &&
           !memcmp(s.bsp_files[2].committed->data, "abcd", 4));
    g_byte_array_set_size(uart_reply, 0);
    assert(mdm_bsp_file_indicate(&s, 2));
    assert(uart_reply->len == 18 && uart_reply->data[4] == 0xe2 &&
           uart_reply->data[5] == 9 &&
           lduw_le_p(uart_reply->data + 9) == 0xa001);
    assert(uart_reply->data[16] == 2 && uart_reply->data[17] == 0);
    mdm_bsp_file_reset(&s, false);
    assert(s.bsp_files[2].pending); /* UART survives USB reset */
    mdm_bsp_file_reset(&s, true);
    assert(!s.bsp_files[2].pending && s.bsp_files[2].committed->len == 4);
    file0[5] = 0;
    file0[7] = 1;
    file0[9] = 'e';
    file0[10] = 'f';
    assert(!mdm_bsp_file_write(&s, 5, 7, file0, sizeof(file0)));
    assert(s.bsp_files[2].committed->len == 2 &&
           !memcmp(s.bsp_files[2].committed->data, "ef", 2));
    file0[7] = 2;
    file0[9] = 'g';
    assert(!mdm_bsp_file_write(&s, 5, 7, file0, sizeof(file0)));
    mdm_bsp_file_reset(
        &s, true); /* partial replacement must not destroy committed data */
    assert(!s.bsp_files[2].staging &&
           !memcmp(s.bsp_files[2].committed->data, "ef", 2));
    file0[0] = 2;
    assert(mdm_bsp_file_write(&s, 5, 7, file0, 2) == 48);
    file0[0] = 1;
    file0[7] = 0;
    assert(mdm_bsp_file_write(&s, 5, 7, file0, sizeof(file0)) == 48);
    g_clear_pointer(&s.bsp_files[2].committed, g_byte_array_unref);
    uint8_t file_wire[] = {1, 23, 0, 0, 0xe2, 3, 0, 1, 0, 0, 0xa0, 11,
                           0, 1,  8, 0, 1,    0, 0, 0, 2, 0, 'z',  '!'};
    mdm_qmi(&s, 11, file_wire, sizeof(file_wire));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && size == 4 && !result[0] && !result[2]);
    result = find_tlv(out + 13, n - 13, 0x40, &size);
    assert(result && size == 2 && lduw_le_p(result) == 1);
    assert(!s.bsp_files[1].pending && s.bsp_files[1].staging->len == 2);
    file_wire[17] = 1;
    mdm_qmi(&s, 11, file_wire, sizeof(file_wire));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 0x40, &size);
    assert(result && size == 2 && lduw_le_p(result) == 1);
    assert(s.bsp_files[1].staging->len ==
           2); /* idempotent first-segment retry */
    file_wire[17] = 0;
    file_wire[18] = 1;
    file_wire[22] = 'Y';
    file_wire[23] = '?';
    mdm_qmi(&s, 11, file_wire, sizeof(file_wire));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 0x40, &size);
    assert(result && size == 2 && lduw_le_p(result) == 2);
    assert(s.bsp_files[1].pending && s.bsp_files[1].committed->len == 4);
    file_wire[18] = 3;
    mdm_qmi(&s, 11, file_wire, sizeof(file_wire));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] && result[2] == 48);
    assert(!find_tlv(out + 13, n - 13, 0x40, &size) &&
           s.bsp_files[1].next == 2);
    assert(mdm_bsp_file_indicate(&s, 1));
    n = mdm_read(&s, 11, out, sizeof(out));
    assert(n == 18 && out[4] == 0xe2 && out[5] == 3 &&
           lduw_le_p(out + 9) == 0xa001 && out[16] == 1 && !out[17]);
    /* Releasing a client discards pending notifications, retaining its file. */
    uint8_t release_bsp_ctl[] = {1, 16, 0, 0, 0, 1, 0,    1, 0x23,
                                 0, 5,  0, 1, 2, 0, 0xe2, 3};
    mdm_qmi(&s, 11, release_bsp_ctl, sizeof(release_bsp_ctl));
    n = mdm_read(&s, 11, out, sizeof(out));
    assert(n && s.bsp_files[1].committed && !s.bsp_files[1].pending);
    mdm_bsp_file_reset(&s, true);
    g_clear_pointer(&s.bsp_files[1].committed, g_byte_array_unref);
    uint8_t bsp[] = {1, 12, 0, 0, 0xe2, 1, 0, 1, 0, 1, 0xb0, 0, 0};
    uint8_t release_detect[] = {1, 12, 0, 0, 0xe2, 1, 0, 1, 0, 2, 0x88, 0, 0};
    mdm_qmi(&s, 11, release_detect, sizeof(release_detect));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && size == 4 && !result[0] && !result[2] &&
           reset_detect_releases == 1);
    uint8_t bad_release_detect[] = {1, 16,   0, 0, 0xe2, 1, 0, 1, 0,
                                    2, 0x88, 4, 0, 1,    1, 0, 0};
    mdm_qmi(&s, 11, bad_release_detect, sizeof(bad_release_detect));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] == 1 && result[2] == 48 &&
           reset_detect_releases == 1);
    uint8_t bsp_config[] = {1, 20, 0,    0, 0xe2, 1, 0,    1, 0, 0x0d, 0xe0,
                            8, 0,  0x10, 1, 0,    0, 0x11, 1, 0, 0};
    mdm_qmi(&s, 11, bsp_config, sizeof(bsp_config));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && size == 4 && !result[0] && !result[2]);
    bsp_config[16] = 1;
    mdm_qmi(&s, 11, bsp_config, sizeof(bsp_config));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] == 1 && result[2] == 94);
    bsp_config[16] = 0;
    bsp_config[20] = 1;
    mdm_qmi(&s, 11, bsp_config, sizeof(bsp_config));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] == 1 && result[2] == 94);
    bsp_config[20] = 2;
    mdm_qmi(&s, 11, bsp_config, sizeof(bsp_config));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] == 1 && result[2] == 48);
    /* Captured native BSP Getconfig, including after rejected Setconfig writes:
  * both fixed defaults must remain disabled and each is exactly one byte. */
    uint8_t bsp_get_config[] = {1, 12, 0,    0,    0xe2, 1, 0,
                                1, 0,  0x0e, 0xe0, 0,    0};
    mdm_qmi(&s, 11, bsp_get_config, sizeof(bsp_get_config));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && size == 4 && !result[0] && !result[2]);
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size == 1 && !result[0]);
    result = find_tlv(out + 13, n - 13, 0x11, &size);
    assert(result && size == 1 && !result[0]);
    uint8_t bad_get_config[] = {1,    16,   0, 0, 0xe2, 1, 0, 1, 0,
                                0x0e, 0xe0, 4, 0, 0x10, 1, 0, 1};
    mdm_qmi(&s, 11, bad_get_config, sizeof(bad_get_config));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] == 1 && result[2] == 48);
    assert(!find_tlv(out + 13, n - 13, 0x10, &size) &&
           !find_tlv(out + 13, n - 13, 0x11, &size));
    mdm_qmi(&s, 11, bsp, sizeof(bsp));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == 8 && ldl_le_p(result) == 0x006f10e1 &&
           ldl_le_p(result + 4) == mdm_fuse_serial(backend.bb.imei));
    backend.bb.sim_present = true;
    backend.bb.pin_on = false;
    backend.bb.pin_left = 3;
    backend.bb.puk_left = 10;
    uint8_t provision[] = {1, 12, 0, 0, 11, 1, 0, 1, 0, 0x56, 0x55, 0, 0};
    mdm_qmi(&s, 11, provision, sizeof(provision));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size == 2 && result[0] == 2 && result[1] == 3);
    backend.bb.sim_present = false;
    mdm_qmi(&s, 11, provision, sizeof(provision));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size == 2 && !result[0] && result[1] == 3);
    backend.bb.sim_present = true;
    backend.bb.pin_on = true;
    backend.bb.pin_ok = false;
    mdm_qmi(&s, 11, provision, sizeof(provision));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size == 2 && !result[0]);
    backend.bb.pin_on = false;
    strcpy(backend.bb.imei, "012345678901234");
    strcpy(backend.bb.iccid, "89010000000000000000");
    strcpy(backend.bb.imsi, "001010123456789");
    bsp[9] = 3;
    mdm_qmi(&s, 11, bsp, sizeof(bsp));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 3, &size);
    assert(result && size == 4 && !memcmp(result, "\0\0\0\0", 4));
    const uint8_t bsp_activation[] = {1,   0,   0,   0,   0,   0, '0',
                                      '0', '0', '0', '0', '1', 0};
    result = find_tlv(out + 13, n - 13, 4, &size);
    assert(result && size == 13 && !memcmp(result, bsp_activation, 13));
    result = find_tlv(out + 13, n - 13, 0x11, &size);
    const uint8_t bsp_imei[] = {0x0a, 0x21, 0x43, 0x65, 0x87, 0x09, 0x21, 0x43};
    assert(result && size == 8 && !memcmp(result, bsp_imei, 8));
    result = find_tlv(out + 13, n - 13, 0x16, &size);
    const uint8_t bsp_iccid[] = {0x89, 0x01, 0, 0, 0, 0, 0, 0, 0, 0};
    assert(result && size == 10 && !memcmp(result, bsp_iccid, 10));
    result = find_tlv(out + 13, n - 13, 0x13, &size);
    const uint8_t bsp_imsi[] = {0x00, 0x10, 0x10, 0x12, 0x34, 0x56, 0x78, 0x9f};
    assert(result && size == 8 && !memcmp(result, bsp_imsi, 8));
    assert(!find_tlv(out + 13, n - 13, 0x19, &size));
    test_pri_metadata(&s);
    backend.bb.sim_present = false;
    mdm_qmi(&s, 11, bsp, sizeof(bsp));
    n = mdm_read(&s, 11, out, sizeof(out));
    assert(!find_tlv(out + 13, n - 13, 0x16, &size) &&
           !find_tlv(out + 13, n - 13, 0x13, &size));
    backend.bb.sim_present = true;
    uint8_t bsp_bad[] = {1, 16,   0, 0, 0xe2, 1, 0, 1, 0,
                         3, 0xb0, 4, 0, 1,    1, 0, 0};
    mdm_qmi(&s, 11, bsp_bad, sizeof(bsp_bad));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] == 1 && result[2] == 48);
    bsp[9] = 4;
    mdm_qmi(&s, 11, bsp, sizeof(bsp));
    n = mdm_read(&s, 11, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 2, &size);
    assert(result && result[0] == 1 && result[2] == 94);
    strcpy(backend.bb.sca, "+10000000000");
    wget[9] = 0x34;
    mdm_qmi(&s, 11, wget, sizeof(wget));
    n = mdm_read(&s, 11, out, sizeof(out));
    const uint8_t smsc[] = {'1', '4', '5', 12,  '+', '1', '0', '0',
                            '0', '0', '0', '0', '0', '0', '0', '0'};
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == sizeof(smsc) && !memcmp(result, smsc, size));
    assert(mdm_wms_smsc_indicate(&s, 11, 1));
    n = mdm_read(&s, 11, out, sizeof(out));
    assert(out[4] == 5 && out[6] == 4 && out[9] == 0x46);
    result = find_tlv(out + 13, n - 13, 1, &size);
    assert(result && size == sizeof(smsc) && !memcmp(result, smsc, size));
    /* UIM subscription reports initial status, suppresses duplicates and sends
  * changes only to its owning client/interface. */
    while (mdm_read(&s, 9, out, sizeof(out))) {
    }
    uint8_t ue[] = {1, 19, 0, 0, 11, 7, 0, 1, 0, 0x2e,
                    0, 7,  0, 1, 4,  0, 1, 0, 0, 0};
    mdm_qmi(&s, 9, ue, sizeof(ue));
    mdm_read(&s, 9, out, sizeof(out));
    assert(s.uim_client[9][7] && !s.uim_client[5][7]);
    mdm_qmi_events(&s);
    n = mdm_read(&s, 9, out, sizeof(out));
    assert(n == 45 && out[4] == 11 && out[5] == 7 && out[6] == 4 &&
           out[9] == 0x32);
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size == 29 && result[9] == 1);
    mdm_qmi_events(&s);
    assert(!mdm_read(&s, 9, out, sizeof(out)));
    backend.bb.sim_present = false;
    mdm_qmi_events(&s);
    n = mdm_read(&s, 9, out, sizeof(out));
    result = find_tlv(out + 13, n - 13, 0x10, &size);
    assert(result && size == 15 && result[9] == 0);
    ue[16] = 0;
    mdm_qmi(&s, 9, ue, sizeof(ue));
    mdm_read(&s, 9, out, sizeof(out));
    backend.bb.sim_present = true;
    mdm_qmi_events(&s);
    assert(!mdm_read(&s, 9, out, sizeof(out)));
    g_byte_array_unref(uart_reply);
    mdm_download_free(s.download);
    for (unsigned i = 0; i < 13; i++) {
        g_queue_clear_full(&s.rx[i], (GDestroyNotify)g_bytes_unref);
        g_byte_array_unref(s.tx[i]);
    }
    puts(
        "PASS: split QMUX, transaction IDs, client allocation, malformed TLVs, unsupported services, power-save filtering and interface isolation");
}
