/* The BCM43xx dongle model's auto-join keeps the hardware's order: no join before the host's interface.
 *
 * A real dongle reports AUTH/ASSOC/SET_SSID/LINK only after the host asked to join, and the host asks through
 * its network interface. AppleBCMWLAN (4.3, 8F190) sends WLC_UP in the middle of initFirmware and attaches its
 * IO80211Interface only later; a join reported in between reaches AppleBCMWLAN::setLinkState with no interface
 * and panics (fault_addr 0xc4, pc 0x80656f7c; LightTouchMac matrix 09-29, 8F190/8G4 run 2 under host load).
 * The first host command that needs the interface is its multicast filter (set_var mcast_list).
 *
 * Drives the production sdpcm_handle_cdc / sdio_arm_autojoin / sdio_autojoin with fake timers and the 8F190
 * driver's own command order, no emulator:
 *   - WLC_UP, then a minute of guest time with the rest of initFirmware still running: no join events
 *   - set_var mcast_list: the join follows after the auto-join delay, AUTH ASSOC SET_SSID LINK, once
 *   - a firmware re-init (WLC_UP again, no machine reset, interface still attached) joins again
 *   - IT_WIFI_AUTOJOIN=0: never
 *
 * Mutation (named, must fail this test): arm the clock at WLC_UP whatever the host's state, i.e. in
 * sdpcm_handle_cdc replace
 *        if (s->host_netif) {
 *            sdio_arm_autojoin(s);
 *        }
 * with
 *        sdio_arm_autojoin(s);
 * (the code before the fix).
 *
 * SLICE include/hw/arm/ipod_touch_sdio.h define CDC_|BDC_|WLC_|BSS_INFO_|ISCAN_|WL_CNT_|SCAN_COMPLETE|SDPCM_CONTROL
 * SLICE hw/arm/ipod_touch_sdio.c define FAKE_SSID
 * SLICE hw/arm/ipod_touch_sdio.c fn cdc_hdrlen sdio_send_assoc_events sdio_handle_set_ssid sdio_autojoin sdio_arm_autojoin sdpcm_handle_cdc
 * PKG glib-2.0
 */
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define trace_sdio(...) ((void)0)
#define NANOSECONDS_PER_SECOND 1000000000LL
#define QEMU_CLOCK_VIRTUAL 0
typedef struct { int64_t expire; int armed; void (*cb)(void *); void *opaque; } QEMUTimer;
static int64_t now;
static int64_t qemu_clock_get_ns(int c) { (void)c; return now; }
static void timer_mod(QEMUTimer *t, int64_t when) { t->expire = when; t->armed = 1; }
static uint32_t ldl_le_p(const void *p) { const uint8_t *b = p; return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static void stl_le_p(void *p, uint32_t v) { uint8_t *b = p; b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
typedef struct { const char *fw_version; } BCMSDIOChip;
typedef struct {
    unsigned cdc_hdrlen, bdc_hdrlen;
    bool associated, iscan_reported, host_netif;
    uint32_t toe, toe_ol;
    QEMUTimer *scan_timer, *join_timer;
    BCMSDIOChip chip;
    uint8_t bssid[6];
} IPodTouchSDIOState;
static int events[64], nevents;
static void sdpcm_send(IPodTouchSDIOState *s, uint8_t ch, const uint8_t *p, uint32_t n) { (void)s; (void)ch; (void)p; (void)n; }
static void sdpcm_send_event(IPodTouchSDIOState *s, uint32_t type, uint32_t status, uint16_t flags)
{ (void)s; (void)status; (void)flags; assert(nevents < 64); events[nevents++] = type; }
static void fill_bss_info(IPodTouchSDIOState *s, uint8_t *p) { (void)s; (void)p; }
static void fill_iscan_results(IPodTouchSDIOState *s, uint8_t *p) { (void)s; (void)p; }
#include "slice.h"
static QEMUTimer scan_t, join_t;
static IPodTouchSDIOState s;
static void scan_complete(void *o) { sdpcm_send_event(o, WLC_E_SCAN_COMPLETE, 0, 0); }
/* Let guest time pass, firing whatever the model armed (the join timer re-arms itself while a scan runs). */
static void advance(int64_t ns)
{
    int64_t end = now + ns;
    for (;;) {
        QEMUTimer *next = NULL;
        QEMUTimer *ts[] = { &scan_t, &join_t };
        for (int i = 0; i < 2; i++)
            if (ts[i]->armed && ts[i]->expire <= end && (!next || ts[i]->expire < next->expire)) next = ts[i];
        if (!next) break;
        now = next->expire; next->armed = 0; next->cb(next->opaque);
    }
    now = end;
}
/* One CDC request with a 16-byte header (AppleBCMWLAN's), as the driver frames it. */
static void cdc(uint32_t cmd, int set, const char *iovar, uint32_t value_len)
{
    static uint32_t id;
    uint8_t f[16 + 64] = { 0 };
    uint32_t plen = (iovar ? strlen(iovar) + 1 : 0) + value_len;
    stl_le_p(f, cmd); stl_le_p(f + 4, plen); stl_le_p(f + 8, (++id << 16) | (set ? CDC_DCMD_SET : 0));
    if (iovar) strcpy((char *)f + 16, iovar);
    sdpcm_handle_cdc(&s, f, 16 + plen);
}
static int joined(void)
{
    static const int want[] = { WLC_E_AUTH, WLC_E_ASSOC, WLC_E_SET_SSID, WLC_E_LINK };
    if (nevents == 0) return 0;
    assert(nevents == 4);
    for (int i = 0; i < 4; i++) assert(events[i] == want[i]);
    return 1;
}
/* initFirmware as 8F190's AppleBCMWLAN issues it (runs/good1 trace): WLC_UP comes early. */
static void init_firmware(void)
{
    cdc(WLC_GET_VAR, 0, "ver", 256);
    cdc(WLC_GET_VAR, 0, "cap", 256);
    cdc(84, 0, NULL, 4);
    cdc(WLC_SET_VAR, 1, "nmode", 4);
    cdc(WLC_SET_VAR, 1, "wme", 4);
    cdc(WLC_UP, 0, NULL, 0);
    cdc(WLC_SET_VAR, 1, "event_msgs", 16);
    cdc(WLC_SET_VAR, 1, "mpc", 4);
    cdc(WLC_SET_VAR, 1, "assoc_retry_max", 4);
    cdc(WLC_SET_VAR, 1, "roam_off", 4);
}
int main(void)
{
    scan_t = (QEMUTimer){ .cb = scan_complete, .opaque = &s };
    join_t = (QEMUTimer){ .cb = sdio_autojoin, .opaque = &s };
    s.scan_timer = &scan_t; s.join_timer = &join_t; s.iscan_reported = true;
    unsetenv("IT_WIFI_AUTOJOIN");

    init_firmware();
    advance(60 * NANOSECONDS_PER_SECOND);   /* a loaded host: the driver is still in initFirmware */
    assert(!joined() && "joined before the host had a network interface");
    cdc(WLC_GET_VAR, 0, "chanspec", 4);
    cdc(WLC_SET_VAR, 1, "allmulti", 4);
    assert(!joined());
    cdc(WLC_SET_VAR, 1, "mcast_list", 10);   /* IOEthernetController::setMulticastList: en0 exists */
    advance(9 * NANOSECONDS_PER_SECOND);
    assert(!joined());
    advance(2 * NANOSECONDS_PER_SECOND);
    assert(joined() && s.associated);
    cdc(WLC_SET_VAR, 1, "mcast_list", 16);   /* more groups later: no second join */
    advance(60 * NANOSECONDS_PER_SECOND);
    assert(nevents == 4);

    nevents = 0;                             /* watchdog re-init: same interface, join again */
    init_firmware();
    advance(11 * NANOSECONDS_PER_SECOND);
    assert(joined());

    memset(&s, 0, sizeof(s)); nevents = 0; join_t.armed = scan_t.armed = 0;
    s.scan_timer = &scan_t; s.join_timer = &join_t; s.iscan_reported = true;
    setenv("IT_WIFI_AUTOJOIN", "0", 1);
    init_firmware();
    cdc(WLC_SET_VAR, 1, "mcast_list", 10);
    advance(120 * NANOSECONDS_PER_SECOND);
    assert(!joined());
    puts("PASS: no join before mcast_list; one join after it; again after a re-init; none with IT_WIFI_AUTOJOIN=0");
    return 0;
}
