/* Runtime USB transition must preserve firmware and drain the final reply.
 * SLICE hw/usb/dev-mdm9x15.c define MDM_NAS_CLIENTS|MDM_INTERFACES|MDM_BOOT_SRAM_SIZE
 * SLICE hw/usb/dev-mdm9x15.c typedef MdmDownloadState
 * SLICE hw/usb/dev-mdm9x15.c fn mdm_link_connect mdm_link_update mdm_power_filter_reset mdm_bsp_file_reset mdm_pds_reset mdm_is_notification mdm_download_free mdm_read mdm_reset mdm_reconnect mdm_host_ready_input mdm_update_power
 * SLICE:NAS hw/usb/dev-mdm9x15.c typedef MdmNasClient
 * SLICE:CALL include/hw/misc/ios_baseband_core.h range /* +XCALLSTAT states | typedef struct IosBbAtChan
 * SLICE:WDS hw/usb/dev-mdm9x15.c typedef MdmWdsClient
 * SLICE:BSP hw/usb/dev-mdm9x15.c typedef MdmBspFile
 * SLICE:SMS hw/usb/dev-mdm9x15.c typedef MdmWmsInbox
 * PKG glib-2.0
 */
#include <glib.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define IOS_BB_MAX_CALLS 4
#include "CALL.h"
#include "NAS.h"
#include "WDS.h"
#include "BSP.h"
typedef struct MdmDownloadState MdmDownloadState;
typedef struct {
    bool attached;
    const void *usb_desc;
} USBDevice;
typedef struct {
    bool pending;
    int64_t deadline;
} QEMUTimer;
#define MDM_INTERFACES 13
#define IOS_BB_SMS_STORE 4
#include "SMS.h"
typedef struct {
    USBDevice parent;
    MdmDownloadState *download;
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
    QEMUTimer *reconnect_timer;
    bool reconnect_pending, reconnect_detached, qmi_ready_sent;
    bool managed_power, ready, powered, reset_asserted, host_ready,
        link_started;
    QEMUTimer *link_timer;
    int *device_ready, *reset_detect;
} MDM9x15State;
#define USB_DEVICE(s) (&(s)->parent)
#define MDM9X15(dev) ((MDM9x15State *)(dev))
#define QEMU_CLOCK_VIRTUAL 0
static unsigned attaches, detaches;
static void *error_fatal;
static int64_t qemu_clock_get_ms(int clock)
{
    return 0;
}
static void timer_mod(QEMUTimer *t, int64_t deadline)
{
    t->pending = true;
    t->deadline = deadline;
}
static void timer_del(QEMUTimer *t)
{
    if (t)
        t->pending = false;
}
static void usb_device_attach(USBDevice *d, void *err)
{
    assert(!d->attached);
    d->attached = true;
    attaches++;
}
static void usb_device_detach(USBDevice *d)
{
    assert(d->attached);
    d->attached = false;
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
static const int mdm_desc, mdm_runtime_desc;
static void mdm_serial_reset(MDM9x15State *s)
{
}
#include "slice.h"

int main(void)
{
    int ready = 1, reset = 0;
    QEMUTimer timer = {0}, qmi_timer = {0}, link_timer = {0};
    MDM9x15State s = {.parent.attached = true,
                      .managed_power = true,
                      .ready = true,
                      .powered = true,
                      .link_timer = &link_timer,
                      .device_ready = &ready,
                      .reset_detect = &reset,
                      .reconnect_timer = &timer,
                      .qmi_event_timer = &qmi_timer};
    for (unsigned i = 0; i < 13; i++) {
        s.tx[i] = g_byte_array_new();
    }
    s.download = g_new0(MdmDownloadState, 1);
    s.download->runtime = true;
    s.download->loaded_roles = 7;
    MdmDownloadState *loaded = s.download;
    assert(!mdm_is_notification(USB_DEVICE(&s), 1));
    assert(!mdm_is_notification(USB_DEVICE(&s), 2));
    assert(mdm_is_notification(USB_DEVICE(&s), 4));
    assert(!mdm_is_notification(USB_DEVICE(&s), 5));
    uint8_t done[12] = {6, 0, 0, 0, 12, 0, 0, 0, 1}, out[16];
    g_queue_push_tail(&s.rx[0], g_bytes_new(done, sizeof(done)));
    s.reconnect_pending = true;
    assert(mdm_read(&s, 0, out, 5) == 5);
    assert(!timer.pending && s.parent.attached && s.reconnect_pending);
    assert(mdm_read(&s, 0, out, sizeof(out)) == 7);
    assert(timer.pending && timer.deadline == 1 && !s.reconnect_pending);
    mdm_reconnect(&s);
    assert(!s.parent.attached && s.reconnect_detached && detaches == 1);
    assert(timer.deadline == 100 && !ready && !reset && s.download == loaded);
    mdm_host_ready_input(&s, 0, 0);
    assert(!ready);
    mdm_reconnect(&s);
    assert(ready && !s.parent.attached && !s.reconnect_detached);
    assert(!link_timer.pending); /* announced before the host restarts HSIC */
    mdm_host_ready_input(&s, 0, 1);
    assert(link_timer.pending && link_timer.deadline == 10 &&
           !s.parent.attached);
    mdm_link_connect(&s);
    link_timer.pending = false;
    assert(s.parent.attached && attaches == 1);
    assert(s.parent.usb_desc == &mdm_runtime_desc);
    assert(!mdm_is_notification(USB_DEVICE(&s), 1));
    assert(mdm_is_notification(USB_DEVICE(&s), 2));
    assert(s.download == loaded && s.download->loaded_roles == 7);
    s.nas[0] = (MdmNasClient){.used = true, .iface = 5, .mask = 2};
    s.nas[1] = (MdmNasClient){.used = true, .iface = 13, .mask = 2};
    s.next_cid[3] = 7;
    mdm_reset(USB_DEVICE(&s));
    assert(!s.nas[0].used && s.nas[1].used && s.next_cid[3] == 7);
    assert(s.download == loaded && s.download->runtime);
    mdm_reconnect(&s); /* physical power loss during the detached interval */
    s.powered = false;
    mdm_update_power(&s);
    assert(!timer.pending && !qmi_timer.pending && !s.download && !s.ready &&
           !ready && reset);
    assert(!s.nas[1].used && !s.next_cid[3]);
    assert(s.parent.usb_desc == &mdm_desc);
    assert(!mdm_is_notification(USB_DEVICE(&s), 1));
    assert(!s.reconnect_detached && !s.reconnect_pending);
    mdm_reconnect(&s); /* stale callback cannot resurrect a powered-off modem */
    assert(!s.parent.attached && attaches == 1);
    for (unsigned i = 0; i < 13; i++) {
        g_byte_array_unref(s.tx[i]);
    }
    puts(
        "PASS: completion reply drained, deferred USB reconnect, firmware preservation and power-off cancellation");
}
