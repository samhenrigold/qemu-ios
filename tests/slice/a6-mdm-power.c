/* Board supply/reset must govern the peer and its handshake, not host-ready.
 * SLICE hw/usb/dev-mdm9x15.c define MDM_NAS_CLIENTS|MDM_INTERFACES
 * SLICE hw/usb/dev-mdm9x15.c fn mdm_link_connect mdm_link_update mdm_power_filter_reset mdm_bsp_file_reset mdm_pds_reset mdm_reset mdm_update_power mdm_power_input mdm_reset_input mdm_host_ready_input mdm_release_reset_detect
 * SLICE:NAS hw/usb/dev-mdm9x15.c typedef MdmNasClient
 * SLICE:CALL include/hw/misc/ios_baseband_core.h range /* +XCALLSTAT states | typedef struct IosBbAtChan
 * SLICE:WDS hw/usb/dev-mdm9x15.c typedef MdmWdsClient
 * SLICE:BSP hw/usb/dev-mdm9x15.c typedef MdmBspFile
 * SLICE:SMS hw/usb/dev-mdm9x15.c typedef MdmWmsInbox
 * PKG glib-2.0
 */
#include <glib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define IOS_BB_MAX_CALLS 4
#include "CALL.h"
#include "NAS.h"
#include "WDS.h"
#include "BSP.h"
typedef struct {
    bool pending;
    int64_t deadline;
} QEMUTimer;
#define QEMU_CLOCK_VIRTUAL 0
static int64_t qemu_clock_get_ms(int clock)
{
    return 0;
}
static void timer_mod(QEMUTimer *t, int64_t deadline)
{
    t->pending = true;
    t->deadline = deadline;
}
typedef struct USBDevice {
    bool attached;
    const void *usb_desc;
} USBDevice;
#define MDM_INTERFACES 13
#define IOS_BB_SMS_STORE 4
#include "SMS.h"
typedef struct MDM9x15State {
    USBDevice parent;
    GQueue rx[13];
    GByteArray *tx[13];
    MdmWdsClient wds[14][256];
    MdmBspFile bsp_files[5];
    GBytes *qmi_power_filter[14][256];
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
    void *qmi_event_timer;
    uint16_t voice_events[14][256];
    bool voice_client[14][256], voice_dirty[14][256];
    IosBbCall voice_ended[4];
    uint8_t next_cid[256];
    uint16_t qmi_protocol[14];
    uint32_t qmi_power_save[14], qmi_uim_events[14];
    unsigned logs, data_logs;
    bool qmi_ready_sent;
    bool powered, reset_asserted, ready;
    bool host_ready, link_started;
    bool managed_power;
    QEMUTimer *link_timer;
    void *download;
    void *reconnect_timer;
    bool reconnect_pending, reconnect_detached;
    int *device_ready, *reset_detect;
} MDM9x15State;
#define MDM9X15(dev) ((MDM9x15State *)(dev))
#define USB_DEVICE(s) (&(s)->parent)
static void *error_fatal;
static unsigned attaches, detaches;
static void usb_device_attach(USBDevice *dev, void *errp)
{
    assert(!dev->attached);
    dev->attached = true;
    attaches++;
}
static void usb_device_detach(USBDevice *dev)
{
    assert(dev->attached);
    dev->attached = false;
    detaches++;
}
static void qemu_set_irq(int *irq, int value)
{
    *irq = value;
}
static void mdm_log(MDM9x15State *s, const char *kind, unsigned a, unsigned b,
                    unsigned c)
{
}
static void mdm_download_free(void *p)
{
    g_free(p);
}
static void timer_del(QEMUTimer *t)
{
    if (t)
        t->pending = false;
}
static const int mdm_desc, mdm_runtime_desc;
static void mdm_serial_reset(MDM9x15State *s)
{
}
#include "slice.h"

int main(void)
{
    int ready = 0, reset = 1;
    MDM9x15State s = {.device_ready = &ready, .reset_detect = &reset};
    for (unsigned i = 0; i < 13; i++) {
        s.tx[i] = g_byte_array_new();
    }
    mdm_power_input(&s, 0, 0);
    assert(!s.ready && !s.parent.attached && !attaches && !detaches);
    mdm_power_input(&s, 0, 1);
    assert(s.ready && s.parent.attached && ready && !reset && attaches == 1);
    reset = 1;
    mdm_release_reset_detect(&s);
    assert(!reset && ready && s.ready);
    mdm_power_input(&s, 0, 1);
    assert(attaches == 1);
    mdm_host_ready_input(&s, 0, 0);
    assert(ready && !reset); /* initial announcement before the first session */
    mdm_host_ready_input(&s, 0, 1);
    assert(ready && !reset);
    mdm_host_ready_input(&s, 0, 0);
    assert(!ready && !reset && s.ready && s.parent.attached);
    mdm_host_ready_input(&s, 0, 1);
    assert(ready && !reset && attaches == 1 && !detaches);
    s.next_cid[2] = 7;
    g_queue_push_tail(&s.rx[3], g_bytes_new_static("response", 8));
    g_byte_array_append(s.tx[3], (const uint8_t *)"partial", 7);
    s.download = g_malloc0(16);
    mdm_reset_input(&s, 0, 1);
    assert(!s.ready && !s.parent.attached && !ready && reset && detaches == 1);
    mdm_release_reset_detect(&s);
    assert(reset && !ready); /* Release cannot hide a physically held reset. */
    assert(g_queue_is_empty(&s.rx[3]) && !s.tx[3]->len && !s.next_cid[2]);
    assert(!s.download);
    mdm_power_input(&s, 0, 0);
    mdm_power_input(&s, 0, 1);
    assert(!s.ready && attaches == 1); /* reset still asserted */
    mdm_reset_input(&s, 0, 0);
    assert(s.ready && ready && !reset && attaches == 2);
    s.download = g_malloc0(16);
    void *download = s.download;
    s.wds[0][7] = (MdmWdsClient){
        .family = 6, .events = 0x428, .extended_ip_events = true};
    s.wds[13][7] = (MdmWdsClient){
        .family = 8, .events = 0x428, .extended_ip_events = true};
    s.voice_events[0][7] = s.voice_events[13][7] = 24;
    s.wms_inbox.pending[0][7] = s.wms_inbox.pending[13][7] = 1;
    s.wms_inbox.awaiting_ack[0][7] = s.wms_inbox.awaiting_ack[13][7] = 1;
    s.wms_inbox.valid[0] = true;
    s.wms_inbox.next_transaction = 123;
    mdm_reset(
        USB_DEVICE(&s)); /* a USB link reset preserves physical readiness */
    assert(s.ready && s.parent.attached && ready && !reset && detaches == 1);
    assert(!s.voice_events[0][7] && s.voice_events[13][7] == 24);
    assert(!s.wms_inbox.pending[0][7] && !s.wms_inbox.awaiting_ack[0][7]);
    assert(s.wms_inbox.pending[13][7] && s.wms_inbox.awaiting_ack[13][7]);
    assert(s.wms_inbox.valid[0] && s.wms_inbox.next_transaction == 123);
    assert(s.download == download); /* boot SRAM survives USB bus reset */
    assert(!s.wds[0][7].family && !s.wds[0][7].events &&
           !s.wds[0][7].extended_ip_events);
    assert(s.wds[13][7].family == 8 && s.wds[13][7].events == 0x428 &&
           s.wds[13][7].extended_ip_events);
    mdm_power_input(&s, 0, 0);
    mdm_power_input(&s, 0, 0);
    assert(!s.ready && !s.parent.attached && !ready && reset && detaches == 2);
    assert(!s.download);
    assert(!s.voice_events[13][7]);
    assert(!s.wms_inbox.pending[13][7] && !s.wms_inbox.awaiting_ack[13][7]);
    assert(!s.wms_inbox.valid[0] && s.wms_inbox.next_transaction == 123);
    assert(!s.wds[13][7].family && !s.wds[13][7].events &&
           !s.wds[13][7].extended_ip_events);
    /* A powered managed modem announces ready before its USB link exists. */
    QEMUTimer link = {0};
    s.managed_power = true;
    s.link_timer = &link;
    s.host_ready = false;
    mdm_power_input(&s, 0, 1);
    assert(ready && !s.parent.attached && !link.pending);
    mdm_host_ready_input(&s, 0, 1);
    assert(link.pending && link.deadline == 10 && !s.parent.attached);
    mdm_host_ready_input(&s, 0, 0);
    assert(!link.pending && !s.parent.attached);
    mdm_link_connect(&s); /* stale callback after host withdraws readiness */
    assert(!s.parent.attached);
    mdm_host_ready_input(&s, 0, 1);
    mdm_link_connect(&s);
    link.pending = false;
    assert(s.parent.attached && attaches == 3);
    mdm_host_ready_input(&s, 0, 0);
    assert(s.parent.attached && s.ready && !ready); /* suspend retains link */
    mdm_power_input(&s, 0, 0);
    mdm_host_ready_input(&s, 0, 1);
    mdm_power_input(&s, 0, 1);
    assert(link.pending && link.deadline == 10 && !s.parent.attached);
    mdm_reset_input(&s, 0, 1);
    assert(!link.pending && !ready && reset);
    mdm_link_connect(&s); /* stale callback after physical reset */
    assert(!s.parent.attached);
    for (unsigned i = 0; i < 13; i++) {
        g_byte_array_unref(s.tx[i]);
    }
    puts(
        "PASS: modem supply/reset, physical ready/reset-detect, idempotent transitions, stale QMI cleanup and independent USB link reset");
}
