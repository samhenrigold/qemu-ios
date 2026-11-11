/* Opt-in Qualcomm MDM9x15 HSIC transport for A6 bring-up.
 * Native N42 runtime uses 05c6:9034, data interfaces4/6/8/10.
 * QMUX/CTL framing follows libqmi's public service definitions. Identity and
 * SIM/network state belong to the existing ios-baseband backend.
 * Unsupported services return QMI NOT_SUPPORTED, never fabricated success.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qemu/iov.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "hw/usb/usb.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/misc/ios_baseband.h"
#include "hw/usb/mdm9x15.h"
#include "desc.h"
#include "trace.h"

#define TYPE_MDM9X15 "usb-mdm9x15"
#define MDM_INTERFACES 13
#define MDM_MAX_FRAME 65536
#define MDM_BOOT_SRAM_BASE 0x2a000000u
#define MDM_BOOT_SRAM_SIZE 0x40000u
#define MDM_MAX_IMAGE (64u * 1024 * 1024)
#define TYPE_CHARDEV_MDM9X15 "chardev-mdm9x15"
#define MDM_SERIAL_MAX_FRAME 4096
#define MDM_ROUTER_CHANNELS 11
typedef struct MdmSerial {
    Chardev *chr;
    QEMUTimer *timer;
    GByteArray *rx;
    GByteArray *tx;
    GByteArray *qmux;
    bool escaped;
    bool discard;
    bool h5;
    bool router_version_negotiated;
    uint32_t inactivity_timer;
    uint8_t routes
        [MDM_ROUTER_CHANNELS]; /* logical channel -> device; ff = unrouted */
} MdmSerial;
typedef struct MdmDownloadState {
    uint8_t ram[MDM_BOOT_SRAM_SIZE];
    uint8_t valid[MDM_BOOT_SRAM_SIZE / 8];
    bool loader_started;
    bool hello_accepted;
    GByteArray *image;
    uint32_t image_total;
    uint32_t image_pending;
    bool image_complete;
    unsigned image_index;
    bool image_size_known;
    bool image_elf;
    uint32_t image_phoff;
    uint16_t image_phnum;
    unsigned loaded_roles;
    uint32_t dsp_entries[3];
    unsigned dsp_count;
    bool runtime;
} MdmDownloadState;
#define MDM_NAS_CLIENTS 32
#define MDM_NAS_SERVING (1u << 0)
#define MDM_NAS_SYSTEM (1u << 1)
#define MDM_PDS_SV_IN_FIX (1u << 31)
typedef struct MdmNasClient {
    bool used, notified, attached;
    uint8_t iface, cid;
    uint32_t mask;
    unsigned lac, ci;
    char plmn[7], operator_long[33];
} MdmNasClient;
typedef struct MdmWdsClient {
    uint8_t family;
    uint16_t events;
    bool extended_ip_events;
} MdmWdsClient;
/* Carrier configuration is modem state, retained over a modem reset. */
typedef struct MdmBspFile {
    GByteArray *staging, *committed;
    uint16_t count, next;
    size_t last_offset, last_size;
    uint8_t iface, cid;
    bool pending;
    bool pri_valid;
    uint16_t pri[3];
} MdmBspFile;

/* The native DM plist names its applied revision at Maverick/PRI Revision.
 * Parse that metadata with GLib, already required by QEMU. Other carrier
 * settings do not change this virtual network's fixed radio capabilities. */
typedef struct MdmPriParser {
    unsigned depth, text_depth, maverick_depth;
    bool found, invalid;
    char *root_key, *maverick_key;
    GString *text;
    uint16_t revision[3];
} MdmPriParser;

static bool mdm_pri_version(const char *text, uint16_t version[3])
{
    for (unsigned i = 0; i < 3; i++) {
        unsigned value = 0;
        if (!g_ascii_isdigit(*text)) {
            return false;
        }
        while (g_ascii_isdigit(*text)) {
            value = value * 10 + *text++ - '0';
            if (value > UINT16_MAX) {
                return false;
            }
        }
        version[i] = value;
        if (i < 2 && *text++ != '.') {
            return false;
        }
    }
    return !*text && version[0] <= UINT8_MAX && version[1] <= UINT8_MAX;
}

static void mdm_pri_start(GMarkupParseContext *context, const char *name,
                          const char **names, const char **values, void *opaque,
                          GError **error)
{
    MdmPriParser *p = opaque;
    if (++p->depth > 32) {
        g_set_error_literal(error, G_MARKUP_ERROR,
                            G_MARKUP_ERROR_INVALID_CONTENT,
                            "Carrier plist nesting exceeds limit");
        return;
    }
    if ((p->depth == 1 && strcmp(name, "plist")) ||
        (p->depth == 2 && strcmp(name, "dict"))) {
        p->invalid = true;
    }
    if (p->depth == 3 && !strcmp(name, "dict") &&
        !g_strcmp0(p->root_key, "Maverick")) {
        p->maverick_depth = p->depth;
    }
    if (!strcmp(name, "key") || !strcmp(name, "string")) {
        p->text_depth = p->depth;
        g_string_truncate(p->text, 0);
    }
}

static void mdm_pri_text(GMarkupParseContext *context, const char *text,
                         size_t len, void *opaque, GError **error)
{
    MdmPriParser *p = opaque;
    if (p->depth == p->text_depth) {
        if (p->text->len + len > 256) {
            p->invalid = true;
        } else {
            g_string_append_len(p->text, text, len);
        }
    }
}

static void mdm_pri_end(GMarkupParseContext *context, const char *name,
                        void *opaque, GError **error)
{
    MdmPriParser *p = opaque;
    if (!strcmp(name, "key")) {
        char **key = p->depth == 3 ? &p->root_key
                     : p->depth == 4 && p->maverick_depth == 3
                         ? &p->maverick_key
                         : NULL;
        if (key) {
            g_free(*key);
            *key = g_strdup(p->text->str);
        }
    } else if (!strcmp(name, "string") && p->depth == 4 &&
               p->maverick_depth == 3 &&
               !g_strcmp0(p->maverick_key, "PRI Revision")) {
        p->found = mdm_pri_version(g_strstrip(p->text->str), p->revision);
    }
    if (p->depth == p->text_depth) {
        p->text_depth = 0;
    }
    if (!strcmp(name, "dict") && p->depth == p->maverick_depth) {
        p->maverick_depth = 0;
    }
    p->depth--;
}

static bool mdm_bsp_parse_pri(const uint8_t *data, size_t size,
                              uint16_t revision[3])
{
    const GMarkupParser parser = {
        .start_element = mdm_pri_start,
        .end_element = mdm_pri_end,
        .text = mdm_pri_text,
    };
    MdmPriParser p = {.text = g_string_new(NULL)};
    GMarkupParseContext *context =
        g_markup_parse_context_new(&parser, 0, &p, NULL);
    GError *error = NULL;
    bool ok = g_markup_parse_context_parse(context, (const char *)data, size,
                                           &error) &&
              g_markup_parse_context_end_parse(context, &error) && p.found &&
              !p.invalid;
    if (ok) {
        memcpy(revision, p.revision, sizeof(p.revision));
    }
    g_clear_error(&error);
    g_markup_parse_context_free(context);
    g_string_free(p.text, true);
    g_free(p.root_key);
    g_free(p.maverick_key);
    return ok;
}
/* Transport metadata only: message bytes remain in the shared AT/QMI inbox. */
typedef struct MdmWmsInbox {
    uint8_t pending[MDM_INTERFACES + 1][256];
    uint8_t awaiting_ack[MDM_INTERFACES + 1][256];
    uint32_t next_transaction;
    uint32_t transaction[IOS_BB_SMS_STORE];
    uint8_t route[IOS_BB_SMS_STORE], storage[IOS_BB_SMS_STORE];
    bool valid[IOS_BB_SMS_STORE], read[IOS_BB_SMS_STORE];
} MdmWmsInbox;

OBJECT_DECLARE_SIMPLE_TYPE(MDM9x15State, MDM9X15)
struct MDM9x15State {
    USBDevice parent_obj;
    IosBasebandState *baseband;
    MdmBspFile bsp_files[5];
    GQueue rx[MDM_INTERFACES];
    GByteArray *tx[MDM_INTERFACES];
    unsigned logs;
    unsigned data_logs;
    unsigned qmi_logs;
    uint8_t next_cid[256];
    MdmWdsClient wds[MDM_INTERFACES + 1][256];
    uint16_t qmi_protocol[MDM_INTERFACES + 1];
    uint32_t qmi_power_save[MDM_INTERFACES + 1];
    GBytes *qmi_power_filter[MDM_INTERFACES + 1][256];
    uint32_t qmi_uim_events[MDM_INTERFACES + 1];
    bool uim_client[MDM_INTERFACES + 1][256];
    bool uim_dirty[MDM_INTERFACES + 1][256];
    uint8_t uim_last_status[64], uim_last_len;
    MdmNasClient nas[MDM_NAS_CLIENTS];
    QEMUTimer *qmi_event_timer;
    uint16_t voice_events[MDM_INTERFACES + 1][256];
    bool voice_client[MDM_INTERFACES + 1][256];
    bool voice_dirty[MDM_INTERFACES + 1][256];
    IosBbCall voice_ended[IOS_BB_MAX_CALLS];
    bool dms_mode_events[MDM_INTERFACES + 1][256];
    bool dms_mode_notified[MDM_INTERFACES + 1][256];
    uint8_t dms_last_mode;
    bool pds_force_off, pds_enabled, pds_autotracking;
    uint8_t pds_nmea[3], pds_tracking_config[10];
    uint8_t pds_report_mode, pds_xtra_auto[3], pds_xtra_embedded[2];
    bool pds_xtra_pending, pds_dpo, pds_dpo_persistent;
    uint32_t pds_cell_database, pds_navigation[14], pds_navigation_mask;
    uint32_t pds_events[MDM_INTERFACES + 1][256];
    bool pds_client[MDM_INTERFACES + 1][256];
    bool pds_dirty[MDM_INTERFACES + 1][256];
    uint8_t pds_last_state[2];
    int64_t pds_fix_due[MDM_INTERFACES + 1][256];

    MdmWmsInbox wms_inbox;
    uint8_t wms_events[MDM_INTERFACES + 1][256];
    uint8_t wms_notify[MDM_INTERFACES + 1][256];
    bool wms_client[MDM_INTERFACES + 1][256];
    bool wms_smsc_sent[MDM_INTERFACES + 1][256];
    char wms_last_smsc[24];
    uint8_t wms_registration;
    uint8_t wms_routes[2 + 32 * 4];
    uint16_t wms_routes_len;
    bool wms_status_reports, wms_broadcast[2];
    uint32_t wms_timers[3];
    bool managed_power;
    bool powered;
    bool reset_asserted;
    bool ready;
    bool host_ready;
    bool link_started;
    QEMUTimer *link_timer;
    MdmDownloadState *download;
    QEMUTimer *reconnect_timer;
    bool reconnect_pending;
    bool reconnect_detached;
    bool qmi_ready_sent;
    MdmSerial *serial;
    qemu_irq device_ready;
    qemu_irq reset_detect;
};

typedef struct MdmChardev {
    Chardev parent;
    MDM9x15State *dev;
} MdmChardev;
DECLARE_INSTANCE_CHECKER(MdmChardev, MDM_CHARDEV, TYPE_CHARDEV_MDM9X15)

#define MDM_IF(n)                                                              \
    {                                                                          \
        .bInterfaceNumber = n, .bNumEndpoints = 2,                             \
        .bInterfaceClass = USB_CLASS_VENDOR_SPEC, .bInterfaceSubClass = 0xff,  \
        .bInterfaceProtocol = 0xff, .eps = (USBDescEndpoint[])                 \
        {                                                                      \
            {.bEndpointAddress = (n) + 1,                                      \
             .bmAttributes = USB_ENDPOINT_XFER_BULK,                           \
             .wMaxPacketSize = 512},                                           \
                {.bEndpointAddress = USB_DIR_IN | ((n) + 1),                   \
                 .bmAttributes = USB_ENDPOINT_XFER_BULK,                       \
                 .wMaxPacketSize = 512}                                        \
        }                                                                      \
    }
#define MDM_NOTIFY_IF(n)                                                                                           \
    {                                                                                                              \
        .bInterfaceNumber = n, .bNumEndpoints = 1,                                                                 \
        .bInterfaceClass = USB_CLASS_VENDOR_SPEC, .bInterfaceSubClass = 0xff,                                      \
        .bInterfaceProtocol = 0xff, .eps = (USBDescEndpoint[])                                                     \
        {                                                                                                          \
      {.bEndpointAddress=USB_DIR_IN|((n)+1),.bmAttributes=USB_ENDPOINT_XFER_INT,.wMaxPacketSize=16,.bInterval=4} } \
    }
static const USBDescDevice mdm_device = {
    .bcdUSB = 0x0200,
    .bMaxPacketSize0 = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]){
        {.bNumInterfaces = MDM_INTERFACES,
         .bConfigurationValue = 1,
         .bmAttributes = USB_CFG_ATT_ONE,
         .bMaxPower = 25,
         .nif = MDM_INTERFACES,
         .ifs = (USBDescIface[]){
             MDM_IF(0), MDM_IF(1), MDM_IF(2), MDM_NOTIFY_IF(3), MDM_IF(4),
             MDM_NOTIFY_IF(5), MDM_IF(6), MDM_NOTIFY_IF(7), MDM_IF(8),
             MDM_NOTIFY_IF(9), MDM_IF(10), MDM_NOTIFY_IF(11), MDM_IF(12)}}}};
static const USBDescStrings mdm_strings = {
    [1] = "Qualcomm, Incorporated", [2] = "Qualcomm CDMA Technologies MSM"};
static const USBDescDevice mdm_runtime_device = {
    .bcdUSB = 0x0200,
    .bMaxPacketSize0 = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]){{
        .bNumInterfaces = MDM_INTERFACES,
        .bConfigurationValue = 1,
        .bmAttributes = USB_CFG_ATT_ONE,
        .bMaxPower = 25,
        .nif = MDM_INTERFACES,
        .ifs =
            (USBDescIface[]){
                MDM_IF(0),
                MDM_NOTIFY_IF(1),
                MDM_IF(2),
                MDM_NOTIFY_IF(3),
                MDM_IF(4),
                MDM_NOTIFY_IF(5),
                MDM_IF(6),
                MDM_NOTIFY_IF(7),
                MDM_IF(8),
                MDM_NOTIFY_IF(9),
                MDM_IF(10),
                MDM_NOTIFY_IF(11),
                MDM_IF(12),
            },
    }},
};
static const USBDesc mdm_desc = {.id = {.idVendor = 0x05c6,
                                        .idProduct = 0x9001,
                                        .bcdDevice = 0,
                                        .iManufacturer = 1,
                                        .iProduct = 2},
                                 .high = &mdm_device,
                                 .str = mdm_strings};
/* Native Mav5 profile (CommCenter product selector671448, family6) uses
 * 9034 for the operational composite, not the older modem's9001 identity. */
static const USBDesc mdm_runtime_desc = {
    .id = {.idVendor = 0x05c6,
           .idProduct = 0x9034,
           .bcdDevice = 0,
           .iManufacturer = 1,
           .iProduct = 2},
    .high = &mdm_runtime_device,
    .str = mdm_strings,
};
static void mdm_log(MDM9x15State *s, const char *kind, unsigned a, unsigned b,
                    unsigned c)
{
    bool traffic = !strcmp(kind, "bulk-out") || !strcmp(kind, "control") ||
                   !strcmp(kind, "dload-write") || !strcmp(kind, "dload");
    unsigned *count = traffic ? &s->data_logs : &s->logs;
    if ((*count)++ < 512) {
        qemu_log_mask(LOG_UNIMP, "MDM9x15 %s %x %x %x\n", kind, a, b, c);
    }
}
static void tlv(GByteArray *a, uint8_t tag, const void *data, uint16_t n)
{
    uint8_t h[3] = {tag, n, n >> 8};
    g_byte_array_append(a, h, 3);
    g_byte_array_append(a, data, n);
}
static const uint8_t *find_tlv(const uint8_t *p, size_t n, uint8_t tag,
                               size_t *size)
{
    while (n >= 3) {
        unsigned len = lduw_le_p(p + 1);
        if (len > n - 3)
            return NULL;
        if (p[0] == tag) {
            *size = len;
            return p + 3;
        }
        p += len + 3;
        n -= len + 3;
    }
    return NULL;
}
static void mdm_qmi_ready(MDM9x15State *s)
{
    /* QMI CTL Sync indication: modem reset invalidates previous clients.
     * libqmi's public CTL schema defines indication27 with no TLVs. CTL uses
     * indication flag2 and its one-byte transaction field, unlike services. */
    static const uint8_t sync[] = {1, 11, 0, 0x80, 0, 0, 2, 0, 0x27, 0, 0, 0};
    if (s->qmi_ready_sent) {
        return;
    }
    s->qmi_ready_sent = true;
    static const unsigned controls[] = {1, 3, 5, 7, 9, 11};
    for (unsigned i = 0; i < G_N_ELEMENTS(controls); i++) {
        unsigned iface = controls[i];
        if (g_queue_get_length(&s->rx[iface]) < 64) {
            g_queue_push_tail(&s->rx[iface],
                              g_bytes_new_static(sync, sizeof(sync)));
            usb_wakeup(usb_ep_get(USB_DEVICE(s), USB_TOKEN_IN, iface + 1), 0);
            mdm_log(s, "qmi-ready", iface, 0x27, 0);
        }
    }
}
/* These are protocol views of the shared SIM/network state, not another SIM. */
static void mdm_pds_reset(MDM9x15State *s)
{
    s->pds_force_off = s->pds_enabled = s->pds_autotracking = false;
    memset(s->pds_client, 0, sizeof(s->pds_client));
    memset(s->pds_dirty, 0, sizeof(s->pds_dirty));
    memset(s->pds_fix_due, 0, sizeof(s->pds_fix_due));
    memset(s->pds_last_state, 0xff, sizeof(s->pds_last_state));
    s->pds_report_mode = 0;
    const uint8_t tracking[] = {0, 0xff, 1, 0, 0, 0, 0x90, 1, 0, 0};
    memcpy(s->pds_tracking_config, tracking, sizeof(tracking));
    s->pds_xtra_pending = false;
    if (!s->pds_dpo_persistent) {
        s->pds_dpo = false;
    }
    s->pds_cell_database = s->pds_navigation_mask = 0;
    memset(s->pds_events, 0, sizeof(s->pds_events));
    memset(s->pds_nmea, 0, sizeof(s->pds_nmea));
    memset(s->pds_xtra_auto, 0, sizeof(s->pds_xtra_auto));
    memset(s->pds_xtra_embedded, 0, sizeof(s->pds_xtra_embedded));
    memset(s->pds_navigation, 0, sizeof(s->pds_navigation));
}

static uint8_t mdm_sim_pin_state(const IosBbCore *bb)
{
    if (!bb->puk_left) {
        return 5;
    }
    if (!bb->pin_left) {
        return 4;
    }
    return !bb->pin_on ? 3 : bb->pin_ok ? 2 : 1;
}

/* One virtual fuse serial across bootstrap and runtime identity queries. */
static uint32_t mdm_fuse_serial(const char *imei)
{
    uint32_t serial = 2166136261u;
    for (size_t i = 0; imei[i]; i++) {
        serial = (serial ^ (uint8_t)imei[i]) * 16777619u;
    }
    return serial;
}

static bool mdm_registered(IosBbCore *bb)
{
    int status =
        ios_bb_network_registration(bb, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));

    /* Diagnostic flags: off, detached, network available, SIM present,
     * PIN enabled, PIN entered (bits 0..5). */
    unsigned flags = bb->off | bb->cops_detached << 1 | bb->registered << 2 |
                     bb->sim_present << 3 | bb->pin_on << 4 | bb->pin_ok << 5;

    trace_usb_mdm9x15_registration(status, bb->now_ms, bb->camp_due_ms,
                                   bb->cfun, flags, bb->pin_left);
    return status == 1;
}

static void mdm_uim_card_status(GByteArray *a, const IosBbCore *bb)
{
    uint8_t pin = mdm_sim_pin_state(bb);
    /* One physical slot, one GSM SIM application, primary GW index0.
     * Unused provisioning indices areffff (slot/app index bytes). */
    uint8_t status[] = {0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 1, 1,
                        3, 3, 10,   0,    1,    1,    7,    2,    0, 0,
                        0, 0, 0,    3,    3,    10,   3,    3,    10};
    if (!bb->sim_present) {
        status[0] = status[1] = 0xff;
        status[9] = 0;
        status[14] = 0;
        tlv(a, 0x10, status, 15);
        return;
    }
    status[16] = pin == 1 ? 2 : pin == 4 ? 3 : pin == 5 ? 5 : 7;
    status[23] = pin;
    status[24] = bb->pin_left;
    status[25] = bb->puk_left;
    tlv(a, 0x10, status, sizeof(status));
}

static void mdm_uim_file(GByteArray *a, const IosBbCore *bb, uint16_t message,
                         const uint8_t *p, size_t n, uint8_t result[4])
{
    size_t slen = 0, flen = 0, rlen = 0;
    const uint8_t *session = find_tlv(p, n, 1, &slen);
    const uint8_t *file = find_tlv(p, n, 2, &flen);
    const uint8_t *read = find_tlv(p, n, 3, &rlen);
    uint8_t data[32], sw[] = {0x90, 0};
    unsigned fid, size;

    if (!session || slen != 2 || session[1] ||
        (session[0] != 0 && session[0] != 4 && session[0] != 6) || !file ||
        flen < 3 || file[2] != flen - 3 || (file[2] & 1) ||
        (message == 0x20 && (!read || rlen != 4))) {
        result[0] = 1;
        result[2] = 0x30;
        return;
    }
    if (!bb->sim_present) {
        result[0] = 1;
        result[2] = 102; /* QMI NO_SIM */
        return;
    }
    fid = lduw_le_p(file);
    /* ICCID is at MF; the supported6Fxx EFs belong to DF_GSM. */
    bool path_ok = fid == 0x2fe2 ? flen == 5 && lduw_le_p(file + 3) == 0x3f00
                                 : flen == 7 && lduw_le_p(file + 3) == 0x3f00 &&
                                       lduw_le_p(file + 5) == 0x7f20;
    size = path_ok ? ios_bb_sim_read_file(bb, fid, data, sizeof(data)) : 0;
    if (!size) {
        result[0] = 1;
        result[2] = 80; /* QMI SIM_FILE_NOT_FOUND */
        tlv(a, 0x10, (const uint8_t[]){0x94, 4}, 2);
        return;
    }
    if (message == 0x20) {
        unsigned offset = lduw_le_p(read), count = lduw_le_p(read + 2);
        uint8_t value[2 + sizeof(data)];
        if (fid != 0x2fe2 && mdm_sim_pin_state(bb) != 2 &&
            mdm_sim_pin_state(bb) != 3) {
            result[0] = 1;
            result[2] = 104; /* PIN_RESTRICTION */
            return;
        }
        if (offset > size || (count && count > size - offset)) {
            result[0] = 1;
            result[2] = 0x30;
            return;
        }
        count = count ?: size - offset;
        stw_le_p(value, count);
        memcpy(value + 2, data + offset, count);
        tlv(a, 0x11, value, count + 2);
    } else { /* Get File Attributes: transparent, read-only EF. */
        uint8_t attr[41] = {0};
        uint8_t *raw = attr + 26;
        stw_le_p(attr, size);
        stw_le_p(attr + 2, fid);
        /* Read always allowed for ICCID; PIN1 protects other files. */
        attr[9] = fid == 0x2fe2 ? 0 : 4;
        stw_le_p(attr + 10, fid == 0x2fe2 ? 0 : 1);
        attr[12] = attr[15] = attr[18] = attr[21] = 1; /* never write */
        stw_le_p(attr + 24, 15);
        raw[2] = size >> 8;
        raw[3] = size;
        raw[4] = fid >> 8;
        raw[5] = fid;
        raw[6] = 4;
        raw[8] = 0x11;
        raw[9] = 0xff;
        raw[10] = 0xbb;
        raw[11] = 1;
        raw[12] = 2;
        tlv(a, 0x11, attr, sizeof(attr));
    }
    tlv(a, 0x10, sw, sizeof(sw));
}

static void mdm_nas_plmn(GByteArray *a, uint8_t tag, const IosBbCore *bb)
{
    uint8_t value[5 + sizeof(bb->operator_long)];
    char mcc[4];
    size_t n = strlen(bb->operator_long);

    memcpy(mcc, bb->plmn, 3);
    mcc[3] = 0;
    stw_le_p(value, atoi(mcc));
    stw_le_p(value + 2, atoi(bb->plmn + 3));
    value[4] = n;
    memcpy(value + 5, bb->operator_long, n);
    tlv(a, tag, value, 5 + n);
}

static void mdm_nas_serving(GByteArray *a, IosBbCore *bb)
{
    bool attached = mdm_registered(bb);
    uint8_t serving[] = {attached ? 1 : 0, attached ? 1 : 2,
                         attached ? 1 : 2, 2,
                         attached ? 1 : 0, 4};
    uint8_t roaming = 1;             /* home */
    uint8_t capabilities[] = {1, 1}; /* GPRS */

    tlv(a, 1, serving, attached ? sizeof(serving) : sizeof(serving) - 1);
    if (attached) {
        tlv(a, 0x10, &roaming, 1);
        tlv(a, 0x11, capabilities, sizeof(capabilities));
        mdm_nas_plmn(a, 0x12, bb);
    }
}

static void mdm_nas_system_info(GByteArray *a, IosBbCore *bb)
{
    bool attached = mdm_registered(bb);
    uint8_t no_service[] = {0, 0, 0};
    uint8_t gsm_status[] = {attached ? 2 : 0, attached ? 2 : 0, attached};
    uint8_t gsm[30] = {1, 3, 1, 3, 1, 0, 1, 0, 1};

    tlv(a, 0x10, no_service, 2); /* CDMA */
    tlv(a, 0x11, no_service, 2); /* HDR */
    tlv(a, 0x12, gsm_status, sizeof(gsm_status));
    tlv(a, 0x13, no_service, 3); /* WCDMA */
    tlv(a, 0x14, no_service, 3); /* LTE */
    if (attached) {
        stw_le_p(gsm + 9, bb->lac);
        gsm[11] = 1;
        stl_le_p(gsm + 12, bb->ci);
        gsm[19] = 1;
        memcpy(gsm + 20, bb->plmn, 3);
        memset(gsm + 23, 0xff, 3);
        memcpy(gsm + 23, bb->plmn + 3, strlen(bb->plmn) - 3);
        gsm[26] = gsm[28] = 1; /* EGPRS/DTM known, unsupported */
        tlv(a, 0x17, gsm, sizeof(gsm));
    }
}

static MdmNasClient *mdm_nas_client(MDM9x15State *s, unsigned iface,
                                    unsigned cid)
{
    MdmNasClient *free_slot = NULL;

    for (unsigned i = 0; i < MDM_NAS_CLIENTS; i++) {
        MdmNasClient *c = &s->nas[i];
        if (c->used && c->iface == iface && c->cid == cid) {
            return c;
        }
        if (!c->used && !free_slot) {
            free_slot = c;
        }
    }
    return free_slot;
}

static void mdm_nas_register(MDM9x15State *s, unsigned iface, unsigned cid,
                             const uint8_t *p, size_t n, uint8_t result[4])
{
    MdmNasClient *c = mdm_nas_client(s, iface, cid);
    uint32_t mask = c && c->used ? c->mask : 0;
    static const uint8_t tags[] = {0x13, 0x18};
    static const uint32_t bits[] = {MDM_NAS_SERVING, MDM_NAS_SYSTEM};

    if (!c) {
        result[0] = 1;
        result[2] = 68; /* insufficient resources */
        return;
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(tags); i++) {
        size_t len = 0;
        const uint8_t *value = find_tlv(p, n, tags[i], &len);
        if (value) {
            if (len != 1 || *value > 1) {
                result[0] = 1;
                result[2] = 48;
                return;
            }
            mask = *value ? mask | bits[i] : mask & ~bits[i];
        }
    }
    c->used = true;
    c->iface = iface;
    c->cid = cid;
    c->mask = mask;
    c->notified = false;
    if (mask) {
        timer_mod(s->qmi_event_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
    }
}

static bool mdm_serial_qmi_routed(const MdmSerial *serial);

static bool mdm_uart_qmi_routed(const MDM9x15State *s)
{
    return s->serial && mdm_serial_qmi_routed(s->serial);
}

static void mdm_release_reset_detect(MDM9x15State *s)
{
    /* Release the startup detector to the physical modem's current level.
     * A powered-off or reset-held peer must continue to report reset. */
    qemu_set_irq(s->reset_detect, !s->ready);
}

/* Native CTL ConfigurePowerSaveMode uses profile u32 + service u8,
 * and an uncounted little-endian u16 indication list (TLV 0x11).
 * Profile 1 is the suspend profile supported by SetPowerSaveMode. */
static void mdm_power_filter_reset(MDM9x15State *s, unsigned count)
{
    for (unsigned iface = 0; iface < count; iface++) {
        for (unsigned service = 0; service < 256; service++) {
            g_clear_pointer(&s->qmi_power_filter[iface][service],
                            g_bytes_unref);
        }
    }
}

static bool mdm_power_filter_allows(MDM9x15State *s, unsigned iface,
                                    const uint8_t *p, size_t n)
{
    if (n < 13 || !p[4] || p[6] != 4 || !s->qmi_power_save[iface]) {
        return true; /* Responses and CTL traffic never enter the filter. */
    }
    GBytes *filter = s->qmi_power_filter[iface][p[4]];
    if (!filter) {
        return true; /* No policy was configured for this service. */
    }
    size_t len;
    const uint8_t *ids = g_bytes_get_data(filter, &len);
    uint16_t message = lduw_le_p(p + 9);
    for (size_t i = 0; i < len; i += 2) {
        if (lduw_le_p(ids + i) == message) {
            return true;
        }
    }
    return false;
}

static void mdm_qmi_trace(unsigned iface, int outcome, const uint8_t *buf,
                          size_t len)
{
    if (trace_event_get_state_backends(TRACE_USB_MDM9X15_QMI)) {
        g_autoptr(GString) hex = g_string_sized_new(MIN(len, 4096) * 2);
        for (size_t i = 0; i < MIN(len, 4096); i++) {
            g_string_append_printf(hex, "%02x", buf[i]);
        }
        trace_usb_mdm9x15_qmi(iface, outcome, len, hex->str);
    }
}

static bool mdm_qmi_output(MDM9x15State *s, unsigned iface, const uint8_t *p,
                           size_t n)
{
    if (!mdm_power_filter_allows(s, iface, p, n)) {
        mdm_qmi_trace(iface, 2, p, n);
        return true; /* Deliberately consumed by the configured suspend policy. */
    }
    if (iface == MDM_INTERFACES) {
        bool accepted =
            mdm_uart_qmi_routed(s) && ios_bb_h5_send(&s->baseband->bb, p, n);
        mdm_qmi_trace(iface, accepted, p, n);
        return accepted;
    }
    if (g_queue_get_length(&s->rx[iface]) >= 64) {
        mdm_qmi_trace(iface, 0, p, n);
        return false;
    }
    g_queue_push_tail(&s->rx[iface], g_bytes_new(p, n));
    usb_wakeup(usb_ep_get(USB_DEVICE(s), USB_TOKEN_IN, iface + 1), 0);
    mdm_qmi_trace(iface, 1, p, n);
    return true;
}

static bool mdm_nas_indicate(MDM9x15State *s, MdmNasClient *c, uint16_t message)
{
    uint8_t header[13] = {1, 0, 0, 0x80, 3, c->cid, 4};
    g_autoptr(GByteArray) a = g_byte_array_new();

    stw_le_p(header + 9, message);
    g_byte_array_append(a, header, sizeof(header));
    if (message == 0x24) {
        mdm_nas_serving(a, &s->baseband->bb);
    } else {
        mdm_nas_system_info(a, &s->baseband->bb);
    }
    stw_le_p(a->data + 1, a->len - 1);
    stw_le_p(a->data + 11, a->len - sizeof(header));
    mdm_log(s, "nas-indication", c->iface, c->cid, message);
    return mdm_qmi_output(s, c->iface, a->data, a->len);
}

static uint8_t mdm_dms_mode(const IosBbCore *bb)
{
    return bb->off ? 6 : bb->cfun == 1 ? 0 : 1;
}

static bool mdm_dms_indicate(MDM9x15State *s, unsigned iface, uint8_t cid)
{
    uint8_t packet[] = {1,
                        16,
                        0,
                        0x80,
                        2,
                        cid,
                        4,
                        0,
                        0,
                        1,
                        0,
                        4,
                        0,
                        0x14,
                        1,
                        0,
                        mdm_dms_mode(&s->baseband->bb)};
    mdm_log(s, "dms-mode-indication", iface, cid, packet[16]);
    return mdm_qmi_output(s, iface, packet, sizeof(packet));
}

/* Qualcomm Gobi 2012 call layouts; all state belongs to the shared core. */
static uint8_t mdm_voice_state(const IosBbCall *c)
{
    switch (c->stat) {
    case IOS_BB_CALL_ACTIVE:
        return 3;
    case IOS_BB_CALL_HELD:
        return 6;
    case IOS_BB_CALL_DIALING:
        return 1;
    case IOS_BB_CALL_ALERTING:
        return 5;
    case IOS_BB_CALL_INCOMING:
        return 2;
    case IOS_BB_CALL_WAITING:
        return 7;
    default:
        return 9;
    }
}

static void mdm_voice_changed(void *opaque, const IosBbCall *call)
{
    MDM9x15State *s = opaque;
    if (call->stat == IOS_BB_CALL_RELEASED) {
        for (unsigned i = 0; i < IOS_BB_MAX_CALLS; i++) {
            if (!s->voice_ended[i].used) {
                s->voice_ended[i] = *call;
                break;
            }
        }
    }
    for (unsigned iface = 0; iface <= MDM_INTERFACES; iface++) {
        for (unsigned cid = 1; cid < 256; cid++) {
            s->voice_dirty[iface][cid] |= s->voice_client[iface][cid] &&
                                          (s->voice_events[iface][cid] & 8);
        }
    }
    mdm_log(s, "voice-state", call->id, call->stat, 0);
    timer_mod(s->qmi_event_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
}

static void mdm_voice_info(GByteArray *a, MDM9x15State *s, bool indication)
{
    uint8_t info[1 + 2 * IOS_BB_MAX_CALLS * 7] = {0};
    uint8_t numbers[1 + 2 * IOS_BB_MAX_CALLS * 35] = {0};
    size_t ni = 1, nn = 1;
    for (unsigned i = 0; i < 2 * IOS_BB_MAX_CALLS; i++) {
        const IosBbCall *c = i < IOS_BB_MAX_CALLS
                                 ? &s->baseband->bb.calls[i]
                                 : &s->voice_ended[i - IOS_BB_MAX_CALLS];
        if (!c->used || (!indication && c->stat == IOS_BB_CALL_RELEASED)) {
            continue;
        }
        uint8_t record[] = {c->id,
                            mdm_voice_state(c),
                            c->emergency ? 9 : 0,
                            c->mt ? 2 : 1,
                            2,
                            0,
                            0}; /* GSM, line1 */
        memcpy(info + ni, record, sizeof(record));
        ni += sizeof(record);
        info[0]++;
        size_t len = strlen(c->number);
        numbers[nn++] = c->id;
        numbers[nn++] = 0; /* presentation allowed */
        numbers[nn++] = len;
        memcpy(numbers + nn, c->number, len);
        nn += len;
        numbers[0]++;
    }
    tlv(a, indication ? 1 : 0x10, info, ni);
    tlv(a, indication ? 0x10 : 0x11, numbers, nn);
}

static bool mdm_voice_indicate(MDM9x15State *s, unsigned iface, uint8_t cid)
{
    uint8_t header[13] = {1, 0, 0, 0x80, 9, cid, 4, 0, 0, 0x2e, 0, 0, 0};
    g_autoptr(GByteArray) a = g_byte_array_new();
    g_byte_array_append(a, header, sizeof(header));
    mdm_voice_info(a, s, true);
    stw_le_p(a->data + 1, a->len - 1);
    stw_le_p(a->data + 11, a->len - sizeof(header));
    return mdm_qmi_output(s, iface, a->data, a->len);
}

static uint8_t mdm_pds_tracking_state(const MDM9x15State *s)
{
    return s->pds_enabled && s->pds_autotracking ? 2 : 1;
}

/* PDS State indication20 has the same TLV1 as the corresponding query.
 * iOS independently checks queried AND reported state; neither replaces the
 * other. Send asynchronously after request replies, with bounded retry. */
static bool mdm_pds_indicate(MDM9x15State *s, unsigned iface, uint8_t cid)
{
    uint8_t packet[] = {1,
                        17,
                        0,
                        0x80,
                        6,
                        cid,
                        4,
                        0,
                        0,
                        0x20,
                        0,
                        5,
                        0,
                        1,
                        2,
                        0,
                        s->pds_enabled,
                        mdm_pds_tracking_state(s)};
    return mdm_qmi_output(s, iface, packet, sizeof(packet));
}

static void mdm_pds_float(uint8_t *out, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    stl_le_p(out, bits);
}

static void mdm_pds_double(uint8_t *out, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    stq_le_p(out, bits);
}

/* PDS EventReport parsed-position TLV13, packed 105-byte wire layout.
 * Serialize explicitly: host structure alignment and endianness must not leak
 * into the modem protocol. Validity bits describe only values we supply. */
static void mdm_pds_position(const IosBbGps *g, int64_t unix_ms,
                             uint8_t out[105])
{
    time_t seconds = unix_ms / 1000;
    struct tm utc;
    uint32_t valid = (1u << 0) | (1u << 1) | (1u << 4) | (1u << 5) | (1u << 6) |
                     (1u << 8) | (1u << 11) | (1u << 15) | (1u << 18) |
                     (1u << 22);
    memset(out, 0, 105);
    gmtime_r(&seconds, &utc);
    stw_le_p(out + 4, utc.tm_year + 1900);
    out[6] = utc.tm_mon; /* PDS calendar months are zero-based. */
    out[7] = utc.tm_wday;
    out[8] = utc.tm_mday;
    out[9] = utc.tm_hour;
    out[10] = utc.tm_min;
    out[11] = utc.tm_sec;
    stw_le_p(out + 12, unix_ms % 1000);
    stq_le_p(out + 15, unix_ms);
    mdm_pds_double(out + 27, g->lat);
    mdm_pds_double(out + 35, g->lon);
    mdm_pds_float(out + 43, g->alt);
    mdm_pds_float(out + 51, g->speed);
    if (g->course >= 0) {
        valid |= 1u << 10;
        mdm_pds_float(out + 59, g->course);
    }
    mdm_pds_float(out + 63, g->accuracy);
    mdm_pds_float(out + 79, g->accuracy * 1.5);
    out[91] = 68; /* one-sigma confidence, as the shared GPS model */
    out[104] = 0; /* standalone operation */
    stl_le_p(out, valid);
}

static bool mdm_pds_fix_indicate(MDM9x15State *s, unsigned iface, uint8_t cid)
{
    IosBbCore *bb = &s->baseband->bb;
    const IosBbGps *g = &bb->gps;
    uint32_t mask = s->pds_events[iface][cid];
    int64_t unix_ms =
        qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + bb->wall_offset_ms;
    uint8_t header[13] = {1, 0, 0, 0x80, 6, cid, 4};
    uint8_t state = 0; /* successful fix; automatic tracking continues */
    g_autoptr(GByteArray) a = g_byte_array_new();
    if (!g->present || !g->fix || !(mask & (7 | MDM_PDS_SV_IN_FIX)) ||
        unix_ms < 0) {
        return false;
    }
    if (mask & MDM_PDS_SV_IN_FIX) {
        g_autoptr(GByteArray) sv = g_byte_array_new();
        uint8_t time[8], count = 0;

        /* Native Apple PDS extension: 5557 subscribes to 555a indications.
         * TLV1 is the fix time; TLV2 is a counted satellite-measurement array.
         * The shared receiver supplies a position without RF measurements,
         * so report an empty array rather than inventing pseudoranges. */
        stw_le_p(header + 9, 0x555a);
        g_byte_array_append(sv, header, sizeof(header));
        stq_le_p(time, unix_ms);
        tlv(sv, 1, time, sizeof(time));
        tlv(sv, 2, &count, 1);
        stw_le_p(sv->data + 1, sv->len - 1);
        stw_le_p(sv->data + 11, sv->len - sizeof(header));
        if (!mdm_qmi_output(s, iface, sv->data, sv->len)) {
            return false;
        }
    }
    if (!(mask & 7)) {
        return true;
    }
    stw_le_p(header + 9, 1);
    g_byte_array_append(a, header, sizeof(header));
    if (mask & 3) {
        char nmea[200];
        int len = ios_bb_gps_nmea(g, unix_ms, "GGA", nmea, sizeof(nmea));
        len +=
            ios_bb_gps_nmea(g, unix_ms, "RMC", nmea + len, sizeof(nmea) - len);
        if (mask & 1) {
            tlv(a, 0x10, nmea, len);
        }
        if (mask & 2) {
            uint8_t extended[203] = {0}; /* standalone, then u16 length */
            stw_le_p(extended + 1, len);
            memcpy(extended + 3, nmea, len);
            tlv(a, 0x11, extended, len + 3);
        }
    }
    tlv(a, 0x12, &state, 1);
    if (mask & 4) {
        uint8_t position[105];
        mdm_pds_position(g, unix_ms, position);
        tlv(a, 0x13, position, sizeof(position));
    }
    stw_le_p(a->data + 1, a->len - 1);
    stw_le_p(a->data + 11, a->len - sizeof(header));
    return mdm_qmi_output(s, iface, a->data, a->len);
}

static void mdm_wms_smsc(GByteArray *a, const IosBbCore *bb)
{
    size_t len = strlen(bb->sca);
    uint8_t value[4 + sizeof(bb->sca)];
    memcpy(value, bb->sca[0] == '+' ? "145" : "129", 3);
    value[3] = len;
    memcpy(value + 4, bb->sca, len);
    tlv(a, 1, value, len + 4);
}

static bool mdm_wms_smsc_indicate(MDM9x15State *s, unsigned iface, uint8_t cid)
{
    uint8_t header[13] = {1, 0, 0, 0x80, 5, cid, 4, 0, 0, 0x46, 0, 0, 0};
    g_autoptr(GByteArray) a = g_byte_array_new();
    g_byte_array_append(a, header, sizeof(header));
    mdm_wms_smsc(a, &s->baseband->bb);
    stw_le_p(a->data + 1, a->len - 1);
    stw_le_p(a->data + 11, a->len - sizeof(header));
    return mdm_qmi_output(s, iface, a->data, a->len);
}

/* IMS transport availability and SMS network registration are independent.
 * Circuit-switched SMS has full service once the shared radio is camped. */
static bool mdm_wms_indicate(MDM9x15State *s, unsigned iface, uint8_t cid,
                             uint16_t message)
{
    uint8_t packet[] = {1,       16, 0, 0x80, 5, cid, 4, 0, 0,
                        message, 0,  4, 0,    1, 1,   0, 0};
    if (message == 0x4b) {
        packet[16] = mdm_registered(&s->baseband->bb) ? 4 : 0;
    }
    return mdm_qmi_output(s, iface, packet, sizeof(packet));
}

/* Decode the shared full PDU, including its SMSC prefix. */
static size_t mdm_wms_pdu(const IosBbSms *sms, uint8_t *raw, size_t capacity)
{
    size_t length = strnlen(sms->pdu, sizeof(sms->pdu));
    if (!sms->used || length == sizeof(sms->pdu) || (length & 1) || !length ||
        length / 2 > capacity) {
        return 0;
    }
    for (size_t i = 0; i < length / 2; i++) {
        int hi = g_ascii_xdigit_value(sms->pdu[2 * i]);
        int lo = g_ascii_xdigit_value(sms->pdu[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        raw[i] = hi * 16 + lo;
    }
    return length / 2;
}

static void mdm_wms_forget_slot(MDM9x15State *s, unsigned slot)
{
    MdmWmsInbox *inbox = &s->wms_inbox;
    inbox->valid[slot] = false;
    inbox->read[slot] = false;
    for (unsigned iface = 0; iface <= MDM_INTERFACES; iface++) {
        for (unsigned cid = 1; cid < 256; cid++) {
            inbox->pending[iface][cid] &= ~(1u << slot);
            inbox->awaiting_ack[iface][cid] &= ~(1u << slot);
        }
    }
}

static void mdm_wms_received(void *opaque, unsigned slot)
{
    MDM9x15State *s = opaque;
    MdmWmsInbox *inbox = &s->wms_inbox;
    uint8_t raw[200];
    size_t length;
    unsigned message_class = 4; /* No message class. */

    if (slot >= IOS_BB_SMS_STORE) {
        return;
    }
    mdm_wms_forget_slot(s, slot); /* Ring reuse invalidates old ACK tokens. */
    length = mdm_wms_pdu(&s->baseband->bb.store[slot], raw, sizeof(raw));
    if (!length || 1u + raw[0] >= length) {
        return;
    }
    size_t tpdu = 1u + raw[0];
    if (tpdu + 3 >= length || (raw[tpdu] & 3)) {
        return; /* This observer receives SMS-DELIVER only. */
    }
    size_t dcs_at = tpdu + 4 + (raw[tpdu + 1] + 1) / 2;
    if (dcs_at >= length) {
        return;
    }
    uint8_t dcs = raw[dcs_at];
    if (((dcs & 0xc0) == 0 && (dcs & 0x10)) || (dcs & 0xf0) == 0xf0) {
        message_class = dcs & 3;
    }
    uint8_t route = 0, storage = 0xff;
    for (size_t i = 2; i + 4 <= s->wms_routes_len; i += 4) {
        if (s->wms_routes[i] == 0 && s->wms_routes[i + 1] == message_class) {
            storage = s->wms_routes[i + 2];
            route = s->wms_routes[i + 3];
            break;
        }
    }
    if (!route || (route == 1 && storage == 0xff)) {
        return; /* Explicit discard, or no usable route. */
    }
    inbox->route[slot] = route;
    inbox->storage[slot] = storage;
    inbox->valid[slot] = true;
    if (!++inbox->next_transaction) {
        ++inbox->next_transaction;
    }
    inbox->transaction[slot] = inbox->next_transaction;
    for (unsigned iface = 0; iface <= MDM_INTERFACES; iface++) {
        for (unsigned cid = 1; cid < 256; cid++) {
            if (s->wms_client[iface][cid] && (s->wms_events[iface][cid] & 1)) {
                inbox->pending[iface][cid] |= 1u << slot;
            }
        }
    }
    timer_mod(s->qmi_event_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
}

static bool mdm_wms_receive_indicate(MDM9x15State *s, unsigned iface,
                                     uint8_t cid, unsigned slot)
{
    MdmWmsInbox *inbox = &s->wms_inbox;
    uint8_t header[13] = {1, 0, 0, 0x80, 5, cid, 4, 0, 0, 1, 0, 0, 0};
    uint8_t mode = 1; /* GSM/WCDMA. */
    g_autoptr(GByteArray) a = g_byte_array_new();
    g_byte_array_append(a, header, sizeof(header));
    if (inbox->route[slot] == 1) {
        uint8_t stored[5] = {inbox->storage[slot]};
        stl_le_p(stored + 1, slot);
        tlv(a, 0x10, stored, sizeof(stored));
    } else {
        uint8_t transfer[208] = {0};
        size_t length = mdm_wms_pdu(&s->baseband->bb.store[slot], transfer + 8,
                                    sizeof(transfer) - 8);
        if (!length || 1u + transfer[8] >= length) {
            return false;
        }
        /* Transfer-route GSM data is a TPDU. Unlike Raw Read, it excludes
         * the SMSC prefix present in the shared full-PDU inbox. */
        size_t smsc_length = 1u + transfer[8];
        length -= smsc_length;
        memmove(transfer + 8, transfer + 8 + smsc_length, length);
        /* AckIndicator: 0 = host must ACK, 1 = modem already acknowledged. */
        transfer[0] = inbox->route[slot] == 3;
        stl_le_p(transfer + 1, inbox->transaction[slot]);
        transfer[5] = 6; /* GSM/WCDMA point-to-point TPDU. */
        stw_le_p(transfer + 6, length);
        tlv(a, 0x11, transfer, 8 + length);
    }
    tlv(a, 0x12, &mode, 1);
    stw_le_p(a->data + 1, a->len - 1);
    stw_le_p(a->data + 11, a->len - sizeof(header));
    /* A suspend-filtered message is deferred, not lost or awaiting an ACK
     * from a client which has never received it. Backpressure also retries. */
    if (!mdm_power_filter_allows(s, iface, a->data, a->len) ||
        !mdm_qmi_output(s, iface, a->data, a->len)) {
        return false;
    }
    if (inbox->route[slot] == 2) {
        inbox->awaiting_ack[iface][cid] |= 1u << slot;
    }
    return true;
}

/* A000 TLV1: type, flags, zero-based segment, segment count, bytes.
 * Keep the previous complete file until the replacement is fully received.
 * Retry of the last segment is idempotent; other out-of-order data fails. */
static unsigned mdm_bsp_file_write(MDM9x15State *s, unsigned iface, uint8_t cid,
                                   const uint8_t *p, size_t n)
{
    size_t len = 0;
    const uint8_t *v = find_tlv(p, n, 1, &len);
    if (!v || len < 6 || v[0] >= G_N_ELEMENTS(s->bsp_files) || (v[1] & ~3)) {
        return 48;
    }
    unsigned index = lduw_le_p(v + 2), count = lduw_le_p(v + 4);
    size_t bytes = len - 6;
    MdmBspFile *f = &s->bsp_files[v[0]];
    if (!count || count > 4096 || index >= count || !bytes || bytes > 1024) {
        return 48;
    }
    bool retry = (index || (v[1] & 1)) && f->count == count &&
                 f->next == index + 1 && f->iface == iface && f->cid == cid;
    if (retry) {
        GByteArray *data = f->staging ? f->staging : f->committed;
        if (!data || f->last_size != bytes ||
            memcmp(data->data + f->last_offset, v + 6, bytes)) {
            return 48;
        }
    } else {
        if (index == 0) {
            if (f->staging && (f->iface != iface || f->cid != cid)) {
                return 48;
            }
            g_clear_pointer(&f->staging, g_byte_array_unref);
            f->staging = g_byte_array_new();
            f->count = count;
            f->next = 0;
            f->iface = iface;
            f->cid = cid;
            f->pending = false;
        }
        if (!f->staging || f->count != count || f->next != index ||
            f->iface != iface || f->cid != cid) {
            return 48;
        }
        f->last_offset = f->staging->len;
        f->last_size = bytes;
        g_byte_array_append(f->staging, v + 6, bytes);
        f->next++;
        if (f->next == count) {
            g_clear_pointer(&f->committed, g_byte_array_unref);
            f->committed = f->staging;
            f->staging = NULL;
            if (v[0] == 2 && mdm_bsp_parse_pri(f->committed->data,
                                               f->committed->len, f->pri)) {
                f->pri_valid = true;
            }
        }
    }
    if (f->next == count) {
        f->pending = true;
        timer_mod(s->qmi_event_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
    }
    return 0;
}

static void mdm_bsp_file_reset(MDM9x15State *s, bool physical)
{
    for (unsigned i = 0; i < G_N_ELEMENTS(s->bsp_files); i++) {
        MdmBspFile *f = &s->bsp_files[i];
        if (physical || f->iface != MDM_INTERFACES) {
            g_clear_pointer(&f->staging, g_byte_array_unref);
            f->next = f->count = 0;
            f->pending = false;
        }
    }
}

static bool mdm_bsp_file_indicate(MDM9x15State *s, unsigned type)
{
    MdmBspFile *f = &s->bsp_files[type];
    uint8_t header[13] = {1, 0, 0, 0x80, 0xe2, f->cid, 4};
    uint8_t status[] = {type, 0};
    g_autoptr(GByteArray) a = g_byte_array_new();
    stw_le_p(header + 9, 0xa001);
    g_byte_array_append(a, header, sizeof(header));
    tlv(a, 1, status, sizeof(status));
    stw_le_p(a->data + 1, a->len - 1);
    stw_le_p(a->data + 11, a->len - sizeof(header));
    return mdm_qmi_output(s, f->iface, a->data, a->len);
}

static bool mdm_uim_indicate(MDM9x15State *s, unsigned iface, uint8_t cid,
                             const GByteArray *status)
{
    uint8_t header[13] = {1, 0, 0, 0x80, 11, cid, 4, 0, 0, 0x32, 0, 0, 0};
    g_autoptr(GByteArray) a = g_byte_array_new();
    g_byte_array_append(a, header, sizeof(header));
    g_byte_array_append(a, status->data, status->len);
    stw_le_p(a->data + 1, a->len - 1);
    stw_le_p(a->data + 11, status->len);
    return mdm_qmi_output(s, iface, a->data, a->len);
}

static void mdm_qmi_events(void *opaque)
{
    MDM9x15State *s = opaque;
    IosBbCore *bb = &s->baseband->bb;
    bool attached = mdm_registered(bb), pending = false;
    uint8_t mode = mdm_dms_mode(bb);
    uint8_t wms_registration = attached ? 4 : 0;
    bool wms_changed = s->wms_registration != wms_registration;
    s->wms_registration = wms_registration;

    if (!s->ready || !s->download || !s->download->runtime) {
        return;
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(s->bsp_files); i++) {
        MdmBspFile *f = &s->bsp_files[i];
        if (f->pending) {
            f->pending = !mdm_bsp_file_indicate(s, i);
            pending |= f->pending;
        }
    }
    g_autoptr(GByteArray) uim_status = g_byte_array_new();
    mdm_uim_card_status(uim_status, bb);
    bool uim_changed =
        s->uim_last_len != uim_status->len ||
        memcmp(s->uim_last_status, uim_status->data, uim_status->len) != 0;
    memcpy(s->uim_last_status, uim_status->data, uim_status->len);
    s->uim_last_len = uim_status->len;
    for (unsigned iface = 0; iface <= MDM_INTERFACES; iface++) {
        for (unsigned cid = 1; cid < 256; cid++) {
            if (!s->uim_client[iface][cid]) {
                continue;
            }
            pending = true;
            s->uim_dirty[iface][cid] |= uim_changed;
            if (s->uim_dirty[iface][cid]) {
                s->uim_dirty[iface][cid] =
                    !mdm_uim_indicate(s, iface, cid, uim_status);
            }
        }
    }
    uint8_t pds_state[] = {s->pds_enabled, mdm_pds_tracking_state(s)};
    bool pds_changed = memcmp(pds_state, s->pds_last_state, 2) != 0;
    if (pds_changed) {
        memcpy(s->pds_last_state, pds_state, 2);
        mdm_log(s, "pds-state", pds_state[0], pds_state[1], 0);
    }
    for (unsigned iface = 0; iface <= MDM_INTERFACES; iface++) {
        for (unsigned cid = 1; cid < 256; cid++) {
            if (!s->pds_client[iface][cid]) {
                continue;
            }
            pending = true;
            s->pds_dirty[iface][cid] |= pds_changed;
            if (s->pds_dirty[iface][cid]) {
                s->pds_dirty[iface][cid] = !mdm_pds_indicate(s, iface, cid);
            }
            if (s->pds_enabled && s->pds_autotracking && !s->pds_force_off &&
                bb->gps.present && bb->gps.fix &&
                (s->pds_events[iface][cid] & (7 | MDM_PDS_SV_IN_FIX))) {
                int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
                if (now >= s->pds_fix_due[iface][cid] &&
                    mdm_pds_fix_indicate(s, iface, cid)) {
                    s->pds_fix_due[iface][cid] =
                        now + MAX(1000LL, (int64_t)ldl_le_p(
                                              s->pds_tracking_config + 2) *
                                              1000);
                }
            } else {
                s->pds_fix_due[iface][cid] = 0;
            }
        }
    }
    if (strcmp(s->wms_last_smsc, bb->sca) || !attached) {
        memcpy(s->wms_last_smsc, bb->sca, sizeof(s->wms_last_smsc));
        memset(s->wms_smsc_sent, 0, sizeof(s->wms_smsc_sent));
    }
    for (unsigned iface = 0; iface <= MDM_INTERFACES; iface++) {
        for (unsigned cid = 1; cid < 256; cid++) {
            if (s->wms_client[iface][cid]) {
                pending = true;
                if (attached && bb->sca[0] && !s->wms_smsc_sent[iface][cid]) {
                    s->wms_smsc_sent[iface][cid] =
                        mdm_wms_smsc_indicate(s, iface, cid);
                }
            }
            if (s->wms_events[iface][cid] & 1) {
                for (unsigned slot = 0; slot < IOS_BB_SMS_STORE; slot++) {
                    if ((s->wms_inbox.pending[iface][cid] & (1u << slot)) &&
                        mdm_wms_receive_indicate(s, iface, cid, slot)) {
                        s->wms_inbox.pending[iface][cid] &= ~(1u << slot);
                    }
                }
            }
            pending |= s->wms_inbox.pending[iface][cid] != 0;
            if (wms_changed) {
                s->wms_notify[iface][cid] |= s->wms_events[iface][cid] & 4;
            }
            uint8_t notify =
                s->wms_notify[iface][cid] & s->wms_events[iface][cid];
            if (notify & 6) {
                pending = true;
            }
            if ((notify & 2) && mdm_wms_indicate(s, iface, cid, 0x49)) {
                s->wms_notify[iface][cid] &= ~2u;
            }
            if ((notify & 4) && mdm_wms_indicate(s, iface, cid, 0x4b)) {
                s->wms_notify[iface][cid] &= ~4u;
            }
        }
    }
    ios_bb_calls_tick(bb, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    bool voice_pending = false;
    for (unsigned iface = 0; iface <= MDM_INTERFACES; iface++) {
        for (unsigned cid = 1; cid < 256; cid++) {
            if (!s->voice_client[iface][cid] ||
                !(s->voice_events[iface][cid] & 8)) {
                continue;
            }
            pending = true;
            if (s->voice_dirty[iface][cid]) {
                s->voice_dirty[iface][cid] = !mdm_voice_indicate(s, iface, cid);
                voice_pending |= s->voice_dirty[iface][cid];
            }
        }
    }
    if (!voice_pending) {
        memset(s->voice_ended, 0, sizeof(s->voice_ended));
    }
    if (mode != s->dms_last_mode) {
        memset(s->dms_mode_notified, 0, sizeof(s->dms_mode_notified));
        s->dms_last_mode = mode;
    }
    for (unsigned iface = 0; iface <= MDM_INTERFACES; iface++) {
        for (unsigned cid = 1; cid < 256; cid++) {
            if (!s->dms_mode_events[iface][cid]) {
                continue;
            }
            pending = true;
            if (!s->dms_mode_notified[iface][cid]) {
                s->dms_mode_notified[iface][cid] =
                    mdm_dms_indicate(s, iface, cid);
            }
        }
    }
    for (unsigned i = 0; i < MDM_NAS_CLIENTS; i++) {
        MdmNasClient *c = &s->nas[i];
        bool sent = true;
        if (!c->used || !c->mask) {
            continue;
        }
        pending = true;
        if (c->notified && c->attached == attached && c->lac == bb->lac &&
            c->ci == bb->ci && !strcmp(c->plmn, bb->plmn) &&
            !strcmp(c->operator_long, bb->operator_long)) {
            continue;
        }
        if (c->mask & MDM_NAS_SERVING) {
            sent &= mdm_nas_indicate(s, c, 0x24);
        }
        if (c->mask & MDM_NAS_SYSTEM) {
            sent &= mdm_nas_indicate(s, c, 0x4e);
        }
        c->notified = sent;
        c->attached = attached;
        c->lac = bb->lac;
        c->ci = bb->ci;
        memcpy(c->plmn, bb->plmn, sizeof(c->plmn));
        memcpy(c->operator_long, bb->operator_long, sizeof(c->operator_long));
    }
    if (pending) {
        timer_mod(s->qmi_event_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 250);
    }
}

static GByteArray *mdm_qmi_reply(MDM9x15State *s, unsigned iface,
                                 const uint8_t *p, size_t n)
{
    unsigned service, header;

    if (iface > MDM_INTERFACES || n < 12 || p[0] != 1 ||
        lduw_le_p(p + 1) != n - 1) {
        return NULL;
    }
    service = p[4];
    header = service ? 13 : 12;
    if (n < header || lduw_le_p(p + header - 2) != n - header || p[3] != 0 ||
        p[6] != 0) {
        return NULL;
    }
    /* Reject truncated TLV chains before applying any modem state change. */
    for (size_t pos = header; pos < n;) {
        if (n - pos < 3 || lduw_le_p(p + pos + 1) > n - pos - 3) {
            return NULL;
        }
        pos += 3 + lduw_le_p(p + pos + 1);
    }
    unsigned message = lduw_le_p(p + (service ? 9 : 8));
    mdm_log(s, "qmi", service, message, n);
    GByteArray *a = g_byte_array_sized_new(256);
    g_byte_array_append(a, p, header);
    a->data[3] = 0x80;
    a->data[6] = service ? 2 : 1;
    uint8_t result[4] = {0};
    if (service == 5) {
        s->wms_client[iface][p[5]] = true;
        timer_mod(s->qmi_event_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
    }
    if (service == 0) {
        switch (message) {
        case 0x20: {
            uint8_t link[2] = {0};
            tlv(a, 1, link, 2);
            break;
        }
        case 0x21: {
            uint8_t versions[] = {4, 0, 1, 0, 5, 0,  2, 1, 0, 0, 0,
                                  3, 1, 0, 0, 0, 11, 1, 0, 0, 0};
            tlv(a, 1, versions, sizeof versions);
            break;
        }
        case 0x22: {
            size_t len = 0;
            const uint8_t *request = find_tlv(p + header, n - header, 1, &len);
            if (!request || len != 1) {
                result[0] = 1;
                result[2] = 0x30;
                break;
            }
            uint8_t alloc[2] = {request[0], ++s->next_cid[request[0]]};
            if (!alloc[1])
                alloc[1] = ++s->next_cid[request[0]];
            tlv(a, 1, alloc, 2);
            if (request[0] == 1) {
                memset(&s->wds[iface][alloc[1]], 0, sizeof(MdmWdsClient));
                s->wds[iface][alloc[1]].family = 4;
            }
            if (request[0] == 9) {
                /* Native call listeners may allocate VOICE without sending a
                 * command. Their asynchronous call-status service is live as
                 * soon as the client exists, including current incoming calls. */
                s->voice_events[iface][alloc[1]] =
                    8; /* Call events default on. */
                s->voice_client[iface][alloc[1]] = true;
                s->voice_dirty[iface][alloc[1]] = true;
                timer_mod(s->qmi_event_timer,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
            }
            break;
        }
        case 0x23: {
            size_t len = 0;
            const uint8_t *request = find_tlv(p + header, n - header, 1, &len);
            if (!request || len != 2) {
                result[0] = 1;
                result[2] = 0x30;
            } else {
                if (request[0] == 1) {
                    memset(&s->wds[iface][request[1]], 0, sizeof(MdmWdsClient));
                }
                if (request[0] == 5) {
                    s->wms_inbox.pending[iface][request[1]] = 0;
                    s->wms_inbox.awaiting_ack[iface][request[1]] = 0;
                    s->wms_events[iface][request[1]] = 0;
                    s->wms_notify[iface][request[1]] = 0;
                    s->wms_client[iface][request[1]] = false;
                    s->wms_smsc_sent[iface][request[1]] = false;
                }
                if (request[0] == 11) {
                    s->uim_client[iface][request[1]] = false;
                    s->uim_dirty[iface][request[1]] = false;
                }
                if (request[0] == 6) {
                    s->pds_client[iface][request[1]] = false;
                    s->pds_dirty[iface][request[1]] = false;
                    s->pds_events[iface][request[1]] = 0;
                    s->pds_fix_due[iface][request[1]] = 0;
                }
                if (request[0] == 9) {
                    s->voice_events[iface][request[1]] = 0;
                    s->voice_client[iface][request[1]] = false;
                    s->voice_dirty[iface][request[1]] = false;
                }
                if (request[0] == 3) {
                    MdmNasClient *c = mdm_nas_client(s, iface, request[1]);
                    if (c && c->used) {
                        memset(c, 0, sizeof(*c));
                    }
                }
                if (request[0] == 0xe2) {
                    for (unsigned i = 0; i < G_N_ELEMENTS(s->bsp_files); i++) {
                        MdmBspFile *f = &s->bsp_files[i];
                        if (f->iface == iface && f->cid == request[1]) {
                            g_clear_pointer(&f->staging, g_byte_array_unref);
                            f->next = f->count = 0;
                            f->pending = false;
                        }
                    }
                }
                tlv(a, 1, request, 2);
            }
            break;
        }
        case 0x26: { /* CTL Set Data Format (libqmi service schema). */
            size_t flen = 0, plen = 0;
            const uint8_t *format = find_tlv(p + header, n - header, 1, &flen);
            const uint8_t *protocol =
                find_tlv(p + header, n - header, 0x10, &plen);
            uint16_t selected = protocol && plen == 2 ? lduw_le_p(protocol) : 1;
            if (!format || flen != 1 || (protocol && plen != 2)) {
                result[0] = 1;
                result[2] = 0x30;
            } else if (*format != 0 || (selected != 1 && selected != 2)) {
                /* QoS flow headers are not implemented. */
                result[0] = 1;
                result[2] = 0x5e;
            } else {
                uint8_t value[2];
                s->qmi_protocol[iface] = selected;
                stw_le_p(value, selected);
                tlv(a, 0x10, value, sizeof(value));
            }
            break;
        }
        case 0x27:
            break;   /* CTL Sync */
        case 0x29: { /* CTL Configure Power Save Mode. */
            size_t plen = 0, ilen = 0;
            const uint8_t *profile = find_tlv(p + header, n - header, 1, &plen);
            const uint8_t *ids = find_tlv(p + header, n - header, 0x11, &ilen);
            if (!profile || plen != 5 || !ids || (ilen & 1)) {
                result[0] = 1;
                result[2] = 0x30;
            } else if (ldl_le_p(profile) != 1 || !profile[4]) {
                result[0] = 1;
                result[2] = 0x5e;
            } else {
                GBytes *filter = g_bytes_new(ids, ilen);
                g_clear_pointer(&s->qmi_power_filter[iface][profile[4]],
                                g_bytes_unref);
                s->qmi_power_filter[iface][profile[4]] = filter;
            }
            break;
        }
        case 0x2a: { /* CTL Set Power Save Mode. */
            size_t len = 0;
            const uint8_t *mode = find_tlv(p + header, n - header, 1, &len);
            if (!mode || len != 4 || ldl_le_p(mode) > 1) {
                result[0] = 1;
                result[2] = 0x30;
            } else {
                s->qmi_power_save[iface] = ldl_le_p(mode);
            }
            break;
        }
        case 0x2b: { /* CTL Get Power Save Mode. */
            uint8_t value[4];
            stl_le_p(value, s->qmi_power_save[iface]);
            tlv(a, 1, value, sizeof(value));
            break;
        }
        default:
            result[0] = 1;
            result[2] = 0x5e;
            break;
        }
    } else if (service ==
               2) { /* DMS: minimal native startup queries, shared identity */
        IosBbCore *bb = &s->baseband->bb;
        const char *text = NULL;
        switch (message) {
        case 0x00:
            break;
        case 0x01: {
            size_t len = 0;
            const uint8_t *report =
                find_tlv(p + header, n - header, 0x14, &len);
            if (report) {
                if (len != 1 || *report > 1) {
                    result[0] = 1;
                    result[2] = 48;
                } else {
                    s->dms_mode_events[iface][p[5]] = *report;
                    s->dms_mode_notified[iface][p[5]] = false;
                    timer_mod(s->qmi_event_timer,
                              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
                }
            }
            break;
        }
        case 0x21:
            text = "QUALCOMM INCORPORATED";
            break;
        case 0x22:
            text = "MDM9x15";
            break;
        case 0x23:
            text = "QEMU MDM9x15";
            break;
        case 0x24:
            text = "";
            break;   /* No own-number EF, as with the existing SIM. */
        case 0x2b: { /* UIM Get PIN Status. */
            uint8_t pin[] = {mdm_sim_pin_state(bb), bb->pin_left, bb->puk_left};
            uint8_t pin2[] = {3, 3, 10};
            tlv(a, 0x11, pin, sizeof(pin));
            tlv(a, 0x12, pin2, sizeof(pin2));
            break;
        }
        case 0x3c:
        case 0x43:
            if (!bb->sim_present) {
                result[0] = 1;
                result[2] = 102;
            } else {
                text = message == 0x3c ? bb->iccid : bb->imsi;
            }
            break;
        case 0x44: {
            uint8_t state = bb->sim_present ? 0 : 0xff;
            tlv(a, 1, &state, 1);
            break;
        }
        case 0x25:
            tlv(a, 0x11, bb->imei, strlen(bb->imei));
            break;
        case 0x2d: {
            uint8_t mode = mdm_dms_mode(bb);
            tlv(a, 1, &mode, 1);
            break;
        }
        case 0x2e: {
            size_t len = 0;
            const uint8_t *mode = find_tlv(p + header, n - header, 1, &len);
            if (!mode || len != 1) {
                result[0] = 1;
                result[2] = 48;
            } else if (*mode > 1 || bb->off) {
                result[0] = 1;
                result[2] = 94;
            } else {
                ios_bb_set_radio_mode(bb, *mode == 0 ? 1 : 4);
                timer_mod(s->qmi_event_timer,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
            }
            break;
        }
        default:
            result[0] = 1;
            result[2] = 0x5e;
            break;
        }
        if (text)
            tlv(a, 1, text, strlen(text));
    } else if (service == 1) { /* WDS client configuration; no active PDN. */
        MdmWdsClient *client = &s->wds[iface][p[5]];
        switch (message) {
        case 0x30:
        case 0x44:
            /* No packet call has been established. OutOfCall is a service
             * state, distinct from an unsupported query; do not fabricate
             * a current or historical bearer. */
            result[0] = 1;
            result[2] = n == header ? 15 : 48;
            break;
        case 0x6b:
            if (n != header) {
                result[0] = 1;
                result[2] = 48;
            } else {
                /* Prefer 3GPP, with no active packet-data systems. This
                 * does not change the shared NAS/voice registration. */
                const uint8_t systems[] = {0, 0};
                tlv(a, 0x10, systems, sizeof(systems));
            }
            break;
        case 0x3e: {
            size_t len = 0;
            const uint8_t *profile = find_tlv(p + header, n - header, 1, &len);
            /* The GSM virtual network has no CDMA Mobile-IP profiles. */
            result[0] = 1;
            result[2] = profile && len == 1 ? 10 : 48;
            break;
        }
        case 0:
            if (n != header) {
                result[0] = 1;
                result[2] = 48;
            } else {
                memset(client, 0, sizeof(*client));
                client->family = 4;
            }
            break;
        case 0x4d: {
            size_t len = 0;
            const uint8_t *family = find_tlv(p + header, n - header, 1, &len);
            if (!family || len != 1 ||
                (*family != 4 && *family != 6 && *family != 8)) {
                result[0] = 1;
                result[2] = 48;
            } else {
                client->family = *family;
            }
            break;
        }
        case 0x01:
        case 0x03: {
            /* Event subscriptions are per client and transport. There is no
             * active packet call yet, so its bearer/IP configuration does
             * not change. Keep these subscriptions for that service state. */
            MdmWdsClient next = *client;
            for (size_t pos = header; pos < n;) {
                uint8_t tag = p[pos];
                size_t len = lduw_le_p(p + pos + 1);
                bool supported = message == 3
                                     ? tag == 0x12
                                     : tag == 0x10 ||
                                           (tag >= 0x12 && tag <= 0x15) ||
                                           (tag >= 0x17 && tag <= 0x1b);
                if (!supported) {
                    result[0] = 1;
                    result[2] = 94;
                    break;
                }
                if (len != 1 || p[pos + 3] > 1) {
                    result[0] = 1;
                    result[2] = 48;
                    break;
                }
                if (message == 3) {
                    next.extended_ip_events = p[pos + 3];
                } else {
                    uint16_t bit = 1u << (tag - 0x10);
                    next.events =
                        p[pos + 3] ? next.events | bit : next.events & ~bit;
                }
                pos += 3 + len;
            }
            if (!result[0]) {
                *client = next;
            }
            break;
        }
        default:
            result[0] = 1;
            result[2] = 94;
            break;
        }
    } else if (service == 0xe2) { /* Apple BSP on the virtual Qualcomm modem. */
        if (message == 0x8802) {  /* BSP ReleaseResetDetect, no payload. */
            if (n != header) {
                result[0] = 1;
                result[2] = 48;
            } else {
                mdm_release_reset_detect(s);
            }
        } else if (message == 0xe00d) {
            /* SetConfigurationSettings is part of native attach-APN setup,
             * even when no APN profile needs writing. This virtual modem has
             * one IMSI and no multi-PDN implementation. Accept disabling
             * those modes; reject enabling functionality we do not provide.
             * Both settings are fixed defaults and survive modem resets. */
            size_t ilen = 0, mlen = 0;
            const uint8_t *imsi = find_tlv(p + header, n - header, 0x10, &ilen);
            const uint8_t *multi =
                find_tlv(p + header, n - header, 0x11, &mlen);
            if ((!imsi && !multi) || (imsi && (ilen != 1 || *imsi > 1)) ||
                (multi && (mlen != 1 || *multi > 1))) {
                result[0] = 1;
                result[2] = 48;
            } else if ((imsi && *imsi) || (multi && *multi)) {
                result[0] = 1;
                result[2] = 94;
            }
        } else if (message == 0xe00e) {
            /* BSP Getconfig: native N42 reads IMSISwitch (TLV10) and
             * MultiPDNToSameAPN (TLV11), each one byte. Report the same
             * fixed single-IMSI/single-PDN settings accepted by Setconfig. */
            if (n != header) {
                result[0] = 1;
                result[2] = 48;
            } else {
                const uint8_t disabled = 0;
                tlv(a, 0x10, &disabled, sizeof(disabled));
                tlv(a, 0x11, &disabled, sizeof(disabled));
            }
        } else if (message == 0xa000) {
            unsigned error =
                mdm_bsp_file_write(s, iface, p[5], p + header, n - header);
            if (error) {
                result[0] = 1;
                result[2] = error;
            } else {
                size_t len = 0;
                const uint8_t *file = find_tlv(p + header, n - header, 1, &len);
                uint8_t next[2];
                /* Native SendFile checks ExpectedSegmentInfo before
                 * sending the next segment. Reflect the committed receive
                 * cursor, including idempotent retries and the final one. */
                stw_le_p(next, s->bsp_files[file[0]].next);
                tlv(a, 0x40, next, sizeof(next));
            }
        } else if (message != 0xb001 && message != 0xb003) {
            result[0] = 1;
            result[2] = 94;
        } else if (n != header) {
            result[0] = 1;
            result[2] = 48;
        } else if (message == 0xb001) {
            uint8_t identity[8];
            stl_le_p(identity, 0x006f10e1); /* Same MDM9615 chip as DLOAD. */
            stl_le_p(identity + 4, mdm_fuse_serial(s->baseband->bb.imei));
            tlv(a, 1, identity, sizeof(identity));
        } else {
            /* Standalone simulated modem: no activation postponement or
             * pending file transfer. Native N42's BSP parser reads word,
             * byte, byte; its activation model maps status0 to Activated4.
             * This is local device state, not an Apple activation ticket. */
            const uint8_t status[4] = {0, 0, 0, 0};
            tlv(a, 3, status, sizeof(status));
            /* Legacy BSP activation info: PRI major/minor bytes, release
             * and PRL version words, then six carrier-ID characters and NUL.
             * The virtual carrier
             * has its own local ID; this is not a real carrier entitlement.
             * The native parser requires exactly 13 bytes and publishes the
             * ID as kCTPostponementInfoCarrierID for service readiness. */
            uint8_t activation_info[13] = {1,   0,   0,   0,   0,   0, '0',
                                           '0', '0', '0', '0', '1', 0};
            if (s->bsp_files[2].pri_valid) {
                /* Original N42 ActivationInfo wire type: CCSS[7C].
                 * PRI major/minor bytes, release word, then PRL word. */
                activation_info[0] = s->bsp_files[2].pri[0];
                activation_info[1] = s->bsp_files[2].pri[1];
                stw_le_p(activation_info + 2, s->bsp_files[2].pri[2]);
            }
            tlv(a, 4, activation_info, sizeof(activation_info));
            IosBbCore *bb = &s->baseband->bb;
            /* Legacy BSP IMEI uses the GSM mobile-identity encoding: first
             * digit above odd/type bits, then swapped semi-octets. The native
             * parser normalizes this into its activation dictionary's ID. */
            uint8_t imei[8];
            memset(imei, 0xff, sizeof(imei));
            imei[0] = ((bb->imei[0] - '0') << 4) | 0x0a;
            for (unsigned i = 1; i < MIN(strlen(bb->imei), 15); i++) {
                uint8_t digit = bb->imei[i] - '0';
                unsigned byte = (i + 1) / 2;
                if (i & 1) {
                    imei[byte] = (imei[byte] & 0xf0) | digit;
                } else {
                    imei[byte] = (digit << 4) | (imei[byte] & 0xf);
                }
            }
            tlv(a, 0x11, imei, sizeof(imei));
            if (bb->sim_present &&
                (mdm_sim_pin_state(bb) == 2 || mdm_sim_pin_state(bb) == 3)) {
                /* Legacy BSP's native dictionary maps 0x13 to IMSI and
                 * 0x16 to ICCID. Both use reading-order BCD, unlike the UIM
                 * elementary files' swapped semi-octets. */
                const char *digits[] = {bb->imsi, bb->iccid};
                const uint8_t tags[] = {0x13, 0x16};
                const size_t sizes[] = {8, 10};
                for (unsigned item = 0; item < G_N_ELEMENTS(tags); item++) {
                    uint8_t identity[10];
                    size_t len = strlen(digits[item]);
                    memset(identity, 0xff, sizeof(identity));
                    for (size_t i = 0; i < MIN(len, sizes[item] * 2); i++) {
                        uint8_t digit = digits[item][i] - '0';
                        if (i & 1) {
                            identity[i / 2] = (identity[i / 2] & 0xf0) | digit;
                        } else {
                            identity[i / 2] = (digit << 4) | 0xf;
                        }
                    }
                    tlv(a, tags[item], identity, sizes[item]);
                }
            }
        }
    } else if (service == 5 && message == 0x20) { /* WMS Raw Send */
        size_t len = 0;
        const uint8_t *raw = find_tlv(p + header, n - header, 1, &len);
        uint8_t reference;
        if (!raw || len < 3 || lduw_le_p(raw + 1) != len - 3) {
            result[0] = 1;
            result[2] = 48;
        } else if (raw[0] != 6) {
            result[0] = 1;
            result[2] = 94; /* GSM/WCDMA PDU only */
        } else if (!ios_bb_submit_sms_pdu(&s->baseband->bb, raw + 3, len - 3,
                                          &reference)) {
            result[0] = 1;
            result[2] = 55; /* WMS message not sent */
        } else {
            uint8_t id[2];
            stw_le_p(id, reference);
            tlv(a, 1, id, sizeof(id));
        }
    } else if (service == 5) { /* WMS configuration, not fabricated delivery. */
        switch (message) {
        case 0x22: { /* Raw Read: the same inbox used by AT+CMGR. */
            size_t len = 0, mode_len = 0;
            const uint8_t *v = find_tlv(p + header, n - header, 1, &len);
            const uint8_t *mode =
                find_tlv(p + header, n - header, 0x10, &mode_len);
            if (!v || len != 5 || (mode && (mode_len != 1 || *mode != 1))) {
                result[0] = 1;
                result[2] = 48;
                break;
            }
            unsigned slot = ldl_le_p(v + 1);
            if (slot >= IOS_BB_SMS_STORE || !s->wms_inbox.valid[slot] ||
                s->wms_inbox.route[slot] != 1 ||
                v[0] != s->wms_inbox.storage[slot]) {
                result[0] = 1;
                result[2] = 35; /* InvalidIndex. */
                break;
            }
            uint8_t raw[204] = {s->wms_inbox.read[slot] ? 0 : 1, 6};
            size_t bytes = mdm_wms_pdu(&s->baseband->bb.store[slot], raw + 4,
                                       sizeof(raw) - 4);
            if (!bytes) {
                result[0] = 1;
                result[2] = 35;
                break;
            }
            stw_le_p(raw + 2, bytes);
            tlv(a, 1, raw, 4 + bytes);
            s->wms_inbox.read[slot] = true;
            break;
        }
        case 0x37: { /* Send ACK for a delivered TransferOnly transaction. */
            size_t len = 0;
            const uint8_t *v = find_tlv(p + header, n - header, 1, &len);
            if (!v || len != 6 || v[4] != 1 || v[5] > 1) {
                result[0] = 1;
                result[2] = 48;
                break;
            }
            bool found = false;
            for (unsigned slot = 0; slot < IOS_BB_SMS_STORE; slot++) {
                if ((s->wms_inbox.awaiting_ack[iface][p[5]] & (1u << slot)) &&
                    s->wms_inbox.transaction[slot] == ldl_le_p(v)) {
                    s->wms_inbox.awaiting_ack[iface][p[5]] &= ~(1u << slot);
                    found = true;
                    break;
                }
            }
            if (!found) {
                result[0] = 1;
                result[2] = 48;
            }
            break;
        }
        case 0x34:
            if (!s->baseband->bb.sim_present) {
                result[0] = 1;
                result[2] = 102;
            } else {
                mdm_wms_smsc(a, &s->baseband->bb);
            }
            break;
        case 0x01: {
            size_t len = 0;
            const uint8_t *v = find_tlv(p + header, n - header, 0x10, &len);
            if (!v || len != 1 || *v > 1) {
                result[0] = 1;
                result[2] = 48;
            } else {
                s->wms_events[iface][p[5]] =
                    (s->wms_events[iface][p[5]] & ~1u) | *v;
            }
            break;
        }
        case 0x32: {
            size_t len = 0, flen = 0;
            const uint8_t *routes = find_tlv(p + header, n - header, 1, &len);
            const uint8_t *flag = find_tlv(p + header, n - header, 0x10, &flen);
            if (!routes || len < 2 || len > sizeof(s->wms_routes) ||
                len != 2u + 4u * lduw_le_p(routes) ||
                (flag && (flen != 1 || *flag > 1))) {
                result[0] = 1;
                result[2] = 48;
                break;
            }
            for (size_t i = 2; i < len; i += 4) {
                if (routes[i] > 1 || routes[i + 1] > 5 ||
                    (routes[i + 2] > 1 && routes[i + 2] != 255) ||
                    routes[i + 3] > 3) {
                    result[0] = 1;
                    result[2] = 48;
                    break;
                }
            }
            if (!result[0]) {
                memcpy(s->wms_routes, routes, len);
                s->wms_routes_len = len;
                if (flag) {
                    s->wms_status_reports = *flag;
                }
            }
            break;
        }
        case 0x33: {
            uint8_t flag = s->wms_status_reports;
            tlv(a, 1, s->wms_routes, MAX(2, s->wms_routes_len));
            tlv(a, 0x10, &flag, 1);
            break;
        }
        case 0x38:
        case 0x39:
        case 0x3a: {
            size_t len = 0;
            const uint8_t *v = find_tlv(p + header, n - header, 1, &len);
            if (!v || len != 4) {
                result[0] = 1;
                result[2] = 48;
            } else {
                s->wms_timers[message - 0x38] = ldl_le_p(v);
            }
            break;
        }
        case 0x53:
        case 0x54:
        case 0x55: {
            uint8_t value[4];
            stl_le_p(value, s->wms_timers[message - 0x53]);
            tlv(a, 1, value, sizeof(value));
            break;
        }
        case 0x3c: {
            size_t len = 0;
            const uint8_t *v = find_tlv(p + header, n - header, 1, &len);
            if (!v || len != 2 || v[0] > 1 || v[1] > 1) {
                result[0] = 1;
                result[2] = 48;
            } else {
                s->wms_broadcast[v[0]] = v[1];
            }
            break;
        }
        case 0x43: {
            /* No waiting voicemail/fax/etc in the local network. */
            uint8_t count = 0;
            tlv(a, 1, &count, 1);
            break;
        }
        case 0x47: {
            uint8_t mask = s->wms_events[iface][p[5]];
            for (unsigned tag = 0x10; tag <= 0x14; tag++) {
                size_t len = 0;
                const uint8_t *v = find_tlv(p + header, n - header, tag, &len);
                if (!v) {
                    continue;
                }
                if (len != 1 || *v > 1) {
                    result[0] = 1;
                    result[2] = 48;
                    break;
                }
                uint8_t bit = 1u << (tag - 0x10 + 1);
                mask = *v ? mask | bit : mask & ~bit;
            }
            if (!result[0]) {
                s->wms_events[iface][p[5]] = mask;
                s->wms_notify[iface][p[5]] = mask & 6;
                timer_mod(s->qmi_event_timer,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
            }
            break;
        }
        case 0x48:
        case 0x4a: {
            uint8_t state =
                message == 0x4a && mdm_registered(&s->baseband->bb) ? 4 : 0;
            tlv(a, 0x10, &state, 1);
            break;
        }
        case 0x4d:
            for (unsigned tag = 0x10; tag <= 0x14; tag++) {
                uint8_t flag =
                    (s->wms_events[iface][p[5]] >> (tag - 0x10 + 1)) & 1;
                tlv(a, tag, &flag, 1);
            }
            break;
        default:
            result[0] = 1;
            result[2] = 94;
            break;
        }
    } else if (service == 9) { /* VOICE */
        IosBbCore *bb = &s->baseband->bb;
        if (!s->voice_client[iface][p[5]]) {
            s->voice_events[iface][p[5]] = 8;
            s->voice_client[iface][p[5]] = true;
        }
        timer_mod(s->qmi_event_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
        switch (message) {
        case 0x03: { /* Indication Register; omitted fields retain their setting. */
            uint16_t events = s->voice_events[iface][p[5]];
            for (unsigned tag = 0x10; tag <= 0x1d; tag++) {
                size_t len = 0;
                const uint8_t *v = find_tlv(p + header, n - header, tag, &len);
                if (!v) {
                    continue;
                }
                if (len != 1 || *v > 1) {
                    result[0] = 1;
                    result[2] = 48;
                    break;
                }
                uint16_t bit = 1u << (tag - 0x10);
                events = *v ? events | bit : events & ~bit;
            }
            if (!result[0]) {
                bool was_enabled = s->voice_events[iface][p[5]] & 8;
                s->voice_events[iface][p[5]] = events;
                if (!(events & 8)) {
                    s->voice_dirty[iface][p[5]] = false;
                } else if (!was_enabled) {
                    s->voice_dirty[iface][p[5]] = true;
                }
            }
            break;
        }
        case 0x20: { /* Dial Call */
            size_t len = 0, tlen = 0;
            const uint8_t *num = find_tlv(p + header, n - header, 1, &len);
            const uint8_t *type = find_tlv(p + header, n - header, 0x10, &tlen);
            char number[sizeof(bb->last_dialed)];
            if (!num || !len || len >= sizeof(number) || memchr(num, 0, len) ||
                (type &&
                 (tlen != 1 || (*type != 0 && *type != 1 && *type != 9)))) {
                result[0] = 1;
                result[2] = 48;
                break;
            }
            memcpy(number, num, len);
            number[len] = 0;
            bb->now_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
            int id = ios_bb_dial(bb, number);
            if (id < 0) {
                result[0] = 1;
                result[2] = 3; /* internal call refused */
            } else {
                uint8_t call_id = id;
                tlv(a, 0x10, &call_id, 1);
            }
            break;
        }
        case 0x22: { /* Answer Call */
            size_t len = 0;
            const uint8_t *id = find_tlv(p + header, n - header, 1, &len);
            if (!id || len != 1) {
                result[0] = 1;
                result[2] = 48;
            } else if (!ios_bb_answer(bb, *id)) {
                result[0] = 1;
                result[2] = 47;
            } else {
                tlv(a, 0x10, id, 1);
            }
            break;
        }
        case 0x21: { /* End Call */
            size_t len = 0;
            const uint8_t *id = find_tlv(p + header, n - header, 1, &len);
            if (!id || len != 1) {
                result[0] = 1;
                result[2] = 48;
            } else if (!ios_bb_hangup(bb, *id)) {
                result[0] = 1;
                result[2] = 47; /* invalid ID */
            } else {
                tlv(a, 0x10, id, 1);
            }
            break;
        }
        case 0x31: { /* Manage Calls: native Phone uses this for End. */
            size_t len = 0, ilen = 0;
            const uint8_t *op = find_tlv(p + header, n - header, 1, &len);
            const uint8_t *id = find_tlv(p + header, n - header, 0x10, &ilen);
            if (!op || len != 1 || (id && ilen != 1)) {
                result[0] = 1;
                result[2] = 48;
            } else if (*op == 2) {
                if (!ios_bb_release_active(bb)) {
                    result[0] = 1;
                    result[2] = 47;
                }
            } else if (*op == 8) {
                for (unsigned i = 0; i < IOS_BB_MAX_CALLS; i++) {
                    if (bb->calls[i].used) {
                        ios_bb_hangup(bb, bb->calls[i].id);
                    }
                }
            } else if (*op == 9) {
                if (!id) {
                    result[0] = 1;
                    result[2] = 48;
                } else if (!ios_bb_hangup(bb, *id)) {
                    result[0] = 1;
                    result[2] = 47;
                }
            } else {
                result[0] = 1;
                result[2] = 94;
            }
            break;
        }
        case 0x24: { /* Get Call Info */
            size_t len = 0;
            const uint8_t *id = find_tlv(p + header, n - header, 1, &len);
            const IosBbCall *call = NULL;
            if (!id || len != 1) {
                result[0] = 1;
                result[2] = 48;
                break;
            }
            for (unsigned i = 0; i < IOS_BB_MAX_CALLS; i++) {
                if (bb->calls[i].used && bb->calls[i].id == *id) {
                    call = &bb->calls[i];
                    break;
                }
            }
            if (!call) {
                result[0] = 1;
                result[2] = 47;
                break;
            }
            uint8_t info[] = {call->id, mdm_voice_state(call),
                              call->emergency ? 9 : 0, call->mt ? 2 : 1, 2};
            uint8_t number[2 + sizeof(call->number)] = {0};
            size_t count = strlen(call->number);
            number[1] = count;
            memcpy(number + 2, call->number, count);
            tlv(a, 0x10, info, sizeof(info));
            tlv(a, 0x11, number, count + 2);
            break;
        }
        case 0x2f:
            mdm_voice_info(a, s, false);
            break;
        default:
            result[0] = 1;
            result[2] = 94;
            break;
        }
    } else if (service == 6) { /* PDS */
        if (!s->pds_client[iface][p[5]]) {
            s->pds_client[iface][p[5]] = true;
            s->pds_dirty[iface][p[5]] = true;
        }
        timer_mod(s->qmi_event_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
        switch (message) {
        case 0x5557: { /* Apple: Set SV-in-fix measurement reporting. */
            size_t len = 0;
            const uint8_t *value = find_tlv(p + header, n - header, 1, &len);
            if (!value || len != 1 || *value > 1) {
                result[0] = 1;
                result[2] = 48;
            } else if (*value) {
                s->pds_events[iface][p[5]] |= MDM_PDS_SV_IN_FIX;
            } else {
                s->pds_events[iface][p[5]] &= ~MDM_PDS_SV_IN_FIX;
            }
            break;
        }
        case 0:
            /* Native locationd requires a real session reset at startup. */
            ios_bb_gps_reset(&s->baseband->bb.gps);
            mdm_pds_reset(s);
            break;
        case 0x01: {
            uint32_t mask = s->pds_events[iface][p[5]];
            for (size_t pos = header; pos < n;) {
                uint8_t tag = p[pos];
                unsigned len = lduw_le_p(p + pos + 1);
                if (tag >= 0x10 && tag <= 0x23) {
                    if (len != 1 || p[pos + 3] > 1) {
                        result[0] = 1;
                        result[2] = 48;
                        break;
                    }
                    unsigned bit = tag - 0x10;
                    mask =
                        p[pos + 3] ? mask | (1u << bit) : mask & ~(1u << bit);
                }
                pos += 3 + len;
            }
            if (!result[0]) {
                s->pds_events[iface][p[5]] = mask;
            }
            break;
        }
        case 0x20: {
            /* Gobi: unknown0, inactive1, active2. Auto tracking owns a
             * session even while no position has been acquired. */
            uint8_t state[] = {s->pds_enabled, mdm_pds_tracking_state(s)};
            tlv(a, 1, state, sizeof(state));
            break;
        }
        case 0x29:
            tlv(a, 1, s->pds_tracking_config, sizeof(s->pds_tracking_config));
            break;
        case 0x2a: {
            size_t len = 0;
            const uint8_t *value = find_tlv(p + header, n - header, 1, &len);
            if (!value || len != sizeof(s->pds_tracking_config) ||
                value[0] > 2 || !ldl_le_p(value + 2)) {
                result[0] = 1;
                result[2] = 48;
            } else {
                memcpy(s->pds_tracking_config, value, len);
            }
            break;
        }
        case 0x21:
        case 0x31:
        case 0x49: {
            size_t len = 0;
            const uint8_t *value = find_tlv(p + header, n - header, 1, &len);
            if (!value || len != 1 || *value > 1) {
                result[0] = 1;
                result[2] = 48;
            } else if (message == 0x21) {
                s->pds_enabled = *value;
                if (*value) {
                    /* An explicit service start powers the receiver again.
                     * Native startup forces the old receiver off before
                     * configuring and enabling the next tracking session. */
                    s->pds_force_off = false;
                } else {
                    ios_bb_gps_reset(&s->baseband->bb.gps);
                }
            } else if (message == 0x31) {
                s->pds_autotracking = *value;
            } else {
                s->pds_force_off = *value;
                if (*value) {
                    ios_bb_gps_reset(&s->baseband->bb.gps);
                }
            }
            break;
        }
        case 0x26:
            tlv(a, 1, s->pds_nmea, sizeof(s->pds_nmea));
            break;
        case 0x27: {
            size_t len = 0;
            const uint8_t *value = find_tlv(p + header, n - header, 1, &len);
            if (!value || len != sizeof(s->pds_nmea)) {
                result[0] = 1;
                result[2] = 48;
            } else {
                memcpy(s->pds_nmea, value, len);
            }
            break;
        }
        case 0x30: {
            uint8_t state = s->pds_autotracking;
            tlv(a, 1, &state, 1);
            break;
        }
        case 0x2b:
            tlv(a, 0x10, s->pds_xtra_auto, sizeof(s->pds_xtra_auto));
            tlv(a, 0x14, s->pds_xtra_embedded, sizeof(s->pds_xtra_embedded));
            break;
        case 0x2c: {
            size_t alen = 0, elen = 0;
            const uint8_t *automatic =
                find_tlv(p + header, n - header, 0x10, &alen);
            const uint8_t *embedded =
                find_tlv(p + header, n - header, 0x14, &elen);
            if ((automatic && (alen != 3 || automatic[0] > 1)) ||
                (embedded &&
                 (elen != 2 || embedded[0] > 1 || embedded[1] > 1))) {
                result[0] = 1;
                result[2] = 48;
            } else {
                if (automatic) {
                    memcpy(s->pds_xtra_auto, automatic, 3);
                }
                if (embedded) {
                    memcpy(s->pds_xtra_embedded, embedded, 2);
                }
            }
            break;
        }
        case 0x2d:
            /* Force Download accepts an asynchronous assistance-data request.
             * No fix or completed download is reported by the virtual receiver. */
            s->pds_xtra_pending = true;
            break;
        case 0x3f: {
            uint8_t enabled = s->pds_dpo;
            tlv(a, 0x10, &enabled, 1);
            break;
        }
        case 0x40: {
            size_t len = 0;
            const uint8_t *config =
                find_tlv(p + header, n - header, 0x10, &len);
            if (!config || len != 1 ||
                (*config != 0 && *config != 1 && *config != 0xf0 &&
                 *config != 0xf1)) {
                result[0] = 1;
                result[2] = 48;
            } else {
                s->pds_dpo = *config & 1;
                s->pds_dpo_persistent = *config < 0xf0;
            }
            break;
        }
        case 0x56:
            for (unsigned i = 0; i < G_N_ELEMENTS(s->pds_navigation); i++) {
                if (s->pds_navigation_mask & (1u << i)) {
                    uint8_t value[4];
                    unsigned tag = 0x10 + i;
                    stl_le_p(value, s->pds_navigation[i]);
                    tlv(a, tag, value, tag == 0x13 || tag >= 0x1c ? 1 : 4);
                }
            }
            break;
        case 0x57: {
            /* Gobi SetNavigationConfig: bounded scalar calibration metadata.
             * It configures a receiver with no simulated motion or fix. */
            uint32_t values[14], mask = s->pds_navigation_mask;
            memcpy(values, s->pds_navigation, sizeof(values));
            for (size_t pos = header; pos < n;) {
                unsigned tag = p[pos], len = lduw_le_p(p + pos + 1);
                if (tag >= 0x10 && tag <= 0x1d) {
                    unsigned expected = tag == 0x13 || tag >= 0x1c ? 1 : 4;
                    if (len != expected) {
                        result[0] = 1;
                        result[2] = 48;
                        break;
                    }
                    values[tag - 0x10] =
                        len == 1 ? p[pos + 3] : ldl_le_p(p + pos + 3);
                    mask |= 1u << (tag - 0x10);
                }
                pos += 3 + len;
            }
            if (!result[0]) {
                memcpy(s->pds_navigation, values, sizeof(values));
                s->pds_navigation_mask = mask;
            }
            break;
        }
        case 0x5f: {
            size_t len = 0;
            const uint8_t *control =
                find_tlv(p + header, n - header, 0x10, &len);
            if (!control || len != 4 || ldl_le_p(control) & ~3u) {
                result[0] = 1;
                result[2] = 48;
            } else {
                s->pds_cell_database = ldl_le_p(control);
            }
            break;
        }
        case 0xaa03: {
            size_t len = 0;
            const uint8_t *mode = find_tlv(p + header, n - header, 1, &len);
            /* Native Maverick startup selects report mode0 (QMI reports).
             * Retain that configuration for QMI event reports. */
            if (!mode || len != 1 || *mode != 0) {
                result[0] = 1;
                result[2] = 48;
            } else {
                s->pds_report_mode = *mode;
            }
            break;
        }
        case 0x36: {
            /* Gobi 2012 sPDSGetServiceVersionResponse_Version: two uint8s,
             * unlike the uint16 major/minor pairs in CTL GetVersionInfo. */
            const uint8_t version[] = {1, 0};
            tlv(a, 1, version, sizeof(version));
            break;
        }
        default:
            result[0] = 1;
            result[2] = 71; /* invalid QMI command */
            break;
        }
    } else if (service == 11) { /* UIM */
        switch (message) {
        case 0x5556: { /* Legacy Apple GetCardProvStatus. */
            if (n != header) {
                result[0] = 1;
                result[2] = 48;
                break;
            }
            /* Native parser: TLV10 is two bytes, provision state and event.
             * Wire state2 maps to subscriber::CardProvisionedState1;
             * event3 is Query. A missing or locked SIM remains Unknown0. */
            IosBbCore *bb = &s->baseband->bb;
            uint8_t pin = mdm_sim_pin_state(bb);
            uint8_t status[] = {
                bb->sim_present && (pin == 2 || pin == 3) ? 2 : 0, 3};
            tlv(a, 0x10, status, sizeof(status));
            break;
        }
        case 0x20:
        case 0x24:
            mdm_uim_file(a, &s->baseband->bb, message, p + header, n - header,
                         result);
            break;
        case 0x2e: {
            size_t len = 0;
            const uint8_t *mask = find_tlv(p + header, n - header, 1, &len);
            if (!mask || len != 4) {
                result[0] = 1;
                result[2] = 0x30;
            } else {
                uint8_t supported[4];
                s->qmi_uim_events[iface] = ldl_le_p(mask) & 1;
                stl_le_p(supported, s->qmi_uim_events[iface]);
                tlv(a, 0x10, supported, sizeof(supported));
                s->uim_client[iface][p[5]] = s->qmi_uim_events[iface] != 0;
                s->uim_dirty[iface][p[5]] = s->uim_client[iface][p[5]];
                timer_mod(s->qmi_event_timer,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
            }
            break;
        }
        case 0x2f:
            mdm_uim_card_status(a, &s->baseband->bb);
            break;
        default:
            result[0] = 1;
            result[2] = 0x5e;
            break;
        }
    } else if (service ==
               3) { /* NAS, same network exposed by the AT backend. */
        IosBbCore *bb = &s->baseband->bb;
        bool attached = mdm_registered(bb);
        switch (message) {
        case 0x03:
            mdm_nas_register(s, iface, p[5], p + header, n - header, result);
            break;
        case 0x4d:
            mdm_nas_system_info(a, bb);
            break;
        case 0x20: {
            size_t len = 0;
            const uint8_t *request =
                find_tlv(p + header, n - header, 0x10, &len);
            uint16_t mask = request && len == 2 ? lduw_le_p(request) : 0;
            if (request && len != 2) {
                result[0] = 1;
                result[2] = 48;
                break;
            }
            uint8_t radio = attached ? 4 : 0;
            uint8_t signal[] = {(uint8_t)bb->signal_dbm, radio};
            tlv(a, 1, signal, sizeof(signal));
            if (mask & 1) {
                uint8_t list[] = {1, 0, signal[0], radio};
                tlv(a, 0x10, list, sizeof(list));
            }
            if (mask & 2) {
                /* RSSI list uses positive magnitude, unlike Strength. */
                uint8_t list[] = {1, 0, MIN(125, MAX(0, -bb->signal_dbm)),
                                  radio};
                tlv(a, 0x11, list, sizeof(list));
            }
            break;
        }
        case 0x24:
            mdm_nas_serving(a, bb);
            break;
        case 0x25:
            mdm_nas_plmn(a, 1, bb);
            break;
        case 0x34: {
            uint8_t emergency = 0, automatic = 0;
            uint8_t mode[] = {4, 0};         /* GSM */
            uint8_t domain[] = {3, 0, 0, 0}; /* CS + PS */
            tlv(a, 0x10, &emergency, 1);
            tlv(a, 0x11, mode, sizeof(mode));
            tlv(a, 0x16, &automatic, 1);
            tlv(a, 0x18, domain, sizeof(domain));
            break;
        }
        default:
            result[0] = 1;
            result[2] = 0x5e;
            break;
        }
    } else {
        result[0] = 1;
        result[2] = 0x5e;
    }
    tlv(a, 2, result, sizeof result);
    stw_le_p(a->data + 1, a->len - 1);
    stw_le_p(a->data + header - 2, a->len - header);
    return a;
}

static void mdm_qmi(MDM9x15State *s, unsigned iface, const uint8_t *p, size_t n)
{
    mdm_qmi_trace(iface, -1, p, n);
    if (s->qmi_logs++ < 512 || (n > 4 && (p[4] == 9 || p[4] == 5))) {
        char hex[257];
        unsigned count = MIN(n, 128);

        for (unsigned i = 0; i < count; i++) {
            snprintf(hex + i * 2, 3, "%02x", p[i]);
        }
        hex[count * 2] = 0;
        qemu_log_mask(LOG_UNIMP, "MDM9x15 qmi-wire %u %s\n", iface, hex);
    }
    g_autoptr(GByteArray) a = mdm_qmi_reply(s, iface, p, n);
    if (!a) {
        return;
    }
    mdm_qmi_output(s, iface, a->data, a->len);
}

/* QMUX framing is shared by USB and the reliable UART stream. */
static void mdm_qmux_receive(MDM9x15State *s, GByteArray *a, unsigned iface,
                             const uint8_t *p, size_t n)
{
    if (n > MDM_MAX_FRAME - a->len) {
        g_byte_array_set_size(a, 0);
        return;
    }
    g_byte_array_append(a, p, n);
    while (a->len >= 3) {
        unsigned size = lduw_le_p(a->data + 1) + 1;
        if (a->data[0] != 1 || size < 12) {
            mdm_log(s, "non-qmux", iface, a->data[0], a->len);
            g_byte_array_set_size(a, 0);
            return;
        }
        if (size > a->len) {
            return;
        }
        mdm_qmi(s, iface, a->data, size);
        g_byte_array_remove_range(a, 0, size);
    }
}
/* The boot/download pipe uses HDLC, not QMUX. CRC is complemented CRC-16/X25
 * in little-endian wire order; multibyte DLOAD command fields are big-endian.
 * Public command layout: posixninja/DLOADTool dload.h. */
static uint16_t mdm_dload_crc(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xffff;

    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
        }
    }
    return crc ^ 0xffff;
}

static GByteArray *mdm_hdlc_encode(const uint8_t *p, size_t n)
{
    GByteArray *wire = g_byte_array_new();
    uint16_t crc = mdm_dload_crc(p, n);
    uint8_t flag = 0x7e;

    g_byte_array_append(wire, &flag, 1);
    for (size_t i = 0; i < n + 2; i++) {
        uint8_t byte = i < n ? p[i] : crc >> (8 * (i - n));

        if (byte == 0x7e || byte == 0x7d) {
            uint8_t escape = 0x7d;
            g_byte_array_append(wire, &escape, 1);
            byte ^= 0x20;
        }
        g_byte_array_append(wire, &byte, 1);
    }
    g_byte_array_append(wire, &flag, 1);
    return wire;
}

static void mdm_dload_reply(MDM9x15State *s, const uint8_t *p, size_t n)
{
    if (g_queue_get_length(&s->rx[0]) >= 64) {
        return;
    }
    g_autoptr(GByteArray) wire = mdm_hdlc_encode(p, n);
    g_queue_push_tail(&s->rx[0], g_bytes_new(wire->data, wire->len));
    usb_wakeup(usb_ep_get(USB_DEVICE(s), USB_TOKEN_IN, 1), 0);
}

static void mdm_serial_receive_hdlc(MDM9x15State *s, const uint8_t *buf,
                                    size_t len);

/* Opt-in packet capture through QEMU's trace backend, without guest stops. */
static void mdm_serial_trace(bool transmit, const uint8_t *buf, size_t len)
{
    if (trace_event_get_state_backends(TRACE_USB_MDM9X15_UART)) {
        g_autoptr(GString) hex = g_string_sized_new(MIN(len, 4096) * 2);
        for (size_t i = 0; i < MIN(len, 4096); i++) {
            g_string_append_printf(hex, "%02x", buf[i]);
        }
        trace_usb_mdm9x15_uart(transmit, len, hex->str);
    }
}

static void mdm_serial_wire_out(void *opaque, const uint8_t *buf, size_t len)
{
    MDM9x15State *s = opaque;
    MdmSerial *serial = s->serial;

    if (len <= 4 * MDM_SERIAL_MAX_FRAME - serial->tx->len) {
        mdm_serial_trace(true, buf, len);
        g_byte_array_append(serial->tx, buf, len);
        /* Traffic must not postpone an already scheduled UART delivery. */
        timer_mod_anticipate(serial->timer,
                             qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
    }
}

static void mdm_serial_router_defaults(MdmSerial *serial)
{
    memset(serial->routes, 0xff, sizeof(serial->routes));
    serial->routes[1] = 0; /* UART, before the native QMI-channel swap */
    for (unsigned ch = 4; ch <= 9; ch++) {
        serial->routes[ch] = ch - 3; /* six USB control devices */
    }
}

static bool mdm_serial_qmi_routed(const MdmSerial *serial)
{
    if (serial->routes[1] == 0) {
        return true; /* legacy UART QMI route */
    }
    for (unsigned ch = 4; ch <= 9; ch++) {
        if (serial->routes[ch] == 0) {
            return true;
        }
    }
    return false;
}

/* ETL RouterSequence: tag2 route(channel, device), tag3 unroute(channel).
 * Native11A465 and the original Mav5 firmware agree on these operations.
 * Validate every TLV before committing, so a bad tail cannot change routing. */
static bool mdm_serial_router_sequence(MdmSerial *serial, const uint8_t *p,
                                       size_t n)
{
    uint8_t routes[MDM_ROUTER_CHANNELS];
    bool changed = false;

    memcpy(routes, serial->routes, sizeof(routes));
    while (n) {
        if (n < 3) {
            return false;
        }
        unsigned tag = p[0], size = lduw_le_p(p + 1);
        if (size > n - 3 || size < 2) {
            return false;
        }
        unsigned channel = lduw_le_p(p + 3);
        if (channel >= MDM_ROUTER_CHANNELS) {
            return false;
        }
        if (tag == 3 && size == 2) {
            routes[channel] = 0xff;
        } else if (tag == 2 && size == 4) {
            unsigned device = lduw_le_p(p + 5);
            if (device > (MDM_INTERFACES - 1) / 2) {
                return false;
            }
            routes[channel] = device;
        } else {
            return false;
        }
        changed = true;
        p += 3 + size;
        n -= 3 + size;
    }
    if (changed) {
        memcpy(serial->routes, routes, sizeof(routes));
        if (!mdm_serial_qmi_routed(serial)) {
            g_byte_array_set_size(serial->qmux, 0);
        }
    }
    return changed;
}

static void mdm_serial_h5_payload(void *opaque, const uint8_t *buf, size_t len)
{
    MDM9x15State *s = opaque;

    mdm_log(s, "uart-h5-payload", len, len ? buf[0] : 0, len > 1 ? buf[1] : 0);
    /* ETL shares type1 between HDLC DIAG and raw QMUX. Preserve a partial
     * QMUX message across reliable packets instead of treating it as DIAG. */
    if (s->serial->qmux->len || (len && buf[0] == 1)) {
        if (mdm_serial_qmi_routed(s->serial)) {
            mdm_qmux_receive(s, s->serial->qmux, MDM_INTERFACES, buf, len);
        }
    } else {
        mdm_serial_receive_hdlc(s, buf, len);
    }
}

/* The native ETL bootstrap uses HDLC/CRC16-X25 on /dev/cu.debug. Command
 * 7b is a loopback ping: the response must preserve the sequence/payload
 * (original 10A405 ETL validators321228b0 and32122964). */
static void mdm_serial_frame(MDM9x15State *s)
{
    MdmSerial *serial = s->serial;
    GByteArray *rx = serial->rx;

    if (!s->ready || !s->download || !s->download->runtime || rx->len < 3 ||
        mdm_dload_crc(rx->data, rx->len - 2) !=
            lduw_le_p(rx->data + rx->len - 2)) {
        return;
    }
    mdm_log(s, "uart-command", rx->data[0], rx->len - 2,
            rx->len > 3 ? rx->data[1] : 0);
    const uint8_t enable_h5[] = {0x4b, 0xfd, 0x31, 0, 1, 1, 0, 1};
    if (rx->len == sizeof(enable_h5) + 2 &&
        !memcmp(rx->data, enable_h5, sizeof(enable_h5))) {
        IosBbCore *bb = &s->baseband->bb;

        bb->h5_userspace = true;
        bb->h5_data_type = 1;
        bb->h5_wire_out = mdm_serial_wire_out;
        bb->h5_payload_out = mdm_serial_h5_payload;
        bb->h5_opaque = s;
        serial->h5 = true;
        mdm_log(s, "uart-h5-enable", 1, 0, 0);
        /* Native configure sends this without waiting for a DIAG response. */
    } else if (serial->h5 &&
               ((rx->len == 10 &&
                 !memcmp(rx->data,
                         (const uint8_t[]){0x4b, 0xfd, 0x31, 0, 5, 0, 0, 0},
                         8)) ||
                (rx->len == 13 &&
                 !memcmp(rx->data,
                         (const uint8_t[]){0x4b, 0xfd, 0x31, 0, 5, 4, 0}, 7) &&
                 ldl_le_p(rx->data + 7)))) {
        /* Original ETL sends tag5 before changing UART speed, without a
         * response wait. Its next reliable frame starts again at seq/ack0.
         * 11A465 supplies a length-4 baud value (3000000) instead of the
         * older zero-length form. Baud itself is virtual; restart only the
         * H5 sequence space for either correctly framed request. */
        ios_bb_h5_restart(&s->baseband->bb);
        mdm_log(s, "uart-h5-baud-restart",
                rx->len == 13 ? ldl_le_p(rx->data + 7) : 0, 0, 0);
    } else if (serial->h5 && rx->len == 13 &&
               !memcmp(rx->data,
                       (const uint8_t[]){0x4b, 0xfd, 0x31, 0, 0xff, 4, 0}, 7)) {
        /* Native11A465 ETLRouterParseVersionPayload expects two byte
         * components and a little-endian16-bit revision. Report the modeled
         * router protocol0.0.1; this is not a generic command-success ACK. */
        const uint8_t version[] = {0x4b, 0xfd, 0x31, 0, 0xff, 4, 0, 0, 0, 1, 0};
        g_autoptr(GByteArray) wire = mdm_hdlc_encode(version, sizeof(version));

        ios_bb_h5_send(&s->baseband->bb, wire->data, wire->len);
        serial->router_version_negotiated = true;
        mdm_log(s, "uart-router-version", 0, 0, 1);
    } else if (serial->h5 && rx->len > 6 &&
               !memcmp(rx->data, (const uint8_t[]){0x4b, 0xfd, 0x31, 0}, 4) &&
               mdm_serial_router_sequence(serial, rx->data + 4, rx->len - 6)) {
        /* Legacy ETL sends routing commands without waiting for a response.
         * Newer ETL first queries version and uses revision bit0 to enable
         * response waits. Do not inject an unsolicited reply into the legacy
         * stream; acknowledge committed routes after that negotiation only. */
        if (serial->router_version_negotiated) {
            const uint8_t response[] = {0x4b, 0xfd, 0x31, 0};
            g_autoptr(GByteArray) wire =
                mdm_hdlc_encode(response, sizeof(response));

            ios_bb_h5_send(&s->baseband->bb, wire->data, wire->len);
        }
        mdm_log(s, "uart-router-sequence", mdm_serial_qmi_routed(serial), 0, 0);
    } else if (serial->h5 && rx->len == 10 &&
               !memcmp(rx->data, (const uint8_t[]){0x4b, 0xfd, 0x2c, 0}, 4)) {
        const uint8_t response[] = {0x4b, 0xfd, 0x2c, 0, 1};
        g_autoptr(GByteArray) wire =
            mdm_hdlc_encode(response, sizeof(response));

        /* ETLUARTParseSetInactivityTimerResponse requires body byte1.
         * Retain the requested policy; the virtual peer can stay awake. */
        serial->inactivity_timer = ldl_le_p(rx->data + 4);
        ios_bb_h5_send(&s->baseband->bb, wire->data, wire->len);
        mdm_log(s, "uart-inactivity-timer", serial->inactivity_timer, 0, 0);
    } else if (rx->data[0] == 0x7b) {
        g_autoptr(GByteArray) wire = mdm_hdlc_encode(rx->data, rx->len - 2);

        if (serial->h5) {
            ios_bb_h5_send(&s->baseband->bb, wire->data, wire->len);
        } else {
            mdm_serial_wire_out(s, wire->data, wire->len);
        }
    }
}

static void mdm_serial_receive_hdlc(MDM9x15State *s, const uint8_t *buf,
                                    size_t len)
{
    MdmSerial *serial = s->serial;

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = buf[i];

        if (byte == 0x7e) {
            if (!serial->discard && !serial->escaped) {
                mdm_serial_frame(s);
            }
            g_byte_array_set_size(serial->rx, 0);
            serial->escaped = false;
            serial->discard = false;
        } else if (!serial->discard) {
            if (serial->escaped) {
                byte ^= 0x20;
                serial->escaped = false;
            } else if (byte == 0x7d) {
                serial->escaped = true;
                continue;
            }
            if (serial->rx->len >= MDM_SERIAL_MAX_FRAME) {
                serial->discard = true;
                g_byte_array_set_size(serial->rx, 0);
            } else {
                g_byte_array_append(serial->rx, &byte, 1);
            }
        }
    }
}

static void mdm_serial_receive(MDM9x15State *s, const uint8_t *buf, size_t len)
{
    mdm_serial_trace(false, buf, len);
    if (s->ready && s->download && s->download->runtime) {
        for (size_t i = 0; i < len; i++) {
            if (s->serial->h5) {
                ios_bb_h5_input(&s->baseband->bb, buf + i, len - i);
                break;
            }
            mdm_serial_receive_hdlc(s, buf + i, 1);
        }
    }
}

static void mdm_serial_flush(void *opaque)
{
    MDM9x15State *s = opaque;
    MdmSerial *serial = s->serial;

    if (serial->h5) {
        ios_bb_h5_tick(&s->baseband->bb, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    }
    while (serial->tx->len) {
        int room = qemu_chr_be_can_write(serial->chr);
        unsigned count;

        if (room <= 0) {
            timer_mod_anticipate(serial->timer,
                                 qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
            return;
        }
        count = MIN((unsigned)room, serial->tx->len);
        qemu_chr_be_write(serial->chr, serial->tx->data, count);
        g_byte_array_remove_range(serial->tx, 0, count);
    }
    if (serial->h5 && s->baseband->bb.h5_nunacked) {
        timer_mod_anticipate(serial->timer,
                             qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
    }
}

static void mdm_serial_reset(MDM9x15State *s)
{
    MdmSerial *serial = s->serial;

    trace_usb_mdm9x15_serial_reset(serial && serial->h5);

    if (serial) {
        timer_del(serial->timer);
        if (serial->h5) {
            ios_bb_reset(&s->baseband->bb);
        }
        g_byte_array_set_size(serial->rx, 0);
        g_byte_array_set_size(serial->tx, 0);
        g_byte_array_set_size(serial->qmux, 0);
        serial->escaped = serial->discard = serial->h5 = false;
        serial->inactivity_timer = 0;
        serial->router_version_negotiated = false;
        mdm_serial_router_defaults(serial);
    }
}

static int mdm_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    MDM9x15State *s = MDM_CHARDEV(chr)->dev;

    if (s) {
        mdm_serial_receive(s, buf, len);
    }
    return len;
}

static void mdm_chr_accept_input(Chardev *chr)
{
    MDM9x15State *s = MDM_CHARDEV(chr)->dev;

    if (s && s->serial->tx->len) {
        timer_mod_anticipate(s->serial->timer,
                             qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
    }
}

static bool mdm_chr_open(Chardev *chr, ChardevBackend *backend, Error **errp)
{
    qemu_chr_be_event(chr, CHR_EVENT_OPENED);
    return true;
}

static void mdm_chr_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->chr_open = mdm_chr_open;
    cc->chr_write = mdm_chr_write;
    cc->chr_accept_input = mdm_chr_accept_input;
}

static const TypeInfo mdm_chr_type = {
    .name = TYPE_CHARDEV_MDM9X15,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(MdmChardev),
    .class_init = mdm_chr_class_init,
};

Chardev *mdm9x15_chardev(DeviceState *dev)
{
    return MDM9X15(dev)->serial->chr;
}

/* SBL1's Sahara handshake is an unescaped little-endian command stream.
 * Public layout: linux-msm/qdl src/sahara.c; original ETL parser 62f0f8. */
static void mdm_download_free(MdmDownloadState *d)
{
    g_clear_pointer(&d->image, g_byte_array_unref);
    g_free(d);
}

static void mdm_sahara_packet(MDM9x15State *s, const uint8_t *p, size_t n)
{
    if (g_queue_get_length(&s->rx[0]) >= 64) {
        return;
    }
    g_queue_push_tail(&s->rx[0], g_bytes_new(p, n));
    usb_wakeup(usb_ep_get(USB_DEVICE(s), USB_TOKEN_IN, 1), 0);
}

static void mdm_sahara_begin(MDM9x15State *s)
{
    uint8_t hello[48] = {0};
    stl_le_p(hello, 1);
    stl_le_p(hello + 4, sizeof(hello));
    stl_le_p(hello + 8, 2);
    stl_le_p(hello + 12, 1);
    stl_le_p(hello + 16, 4096);
    mdm_sahara_packet(s, hello, sizeof(hello));
}

static void mdm_sahara_read_image(MDM9x15State *s)
{
    MdmDownloadState *d = s->download;
    uint8_t request[20] = {0};
    uint32_t length = MIN(MDM_MAX_FRAME, d->image_total - d->image->len);

    stl_le_p(request, 3);
    stl_le_p(request + 4, sizeof(request));
    /* Native ETL transfers the file selected by its per-image session. This
     * opaque wire tag stays consistent between READ_DATA and END_OF_IMAGE;
     * it is not the MBN header's image ID. */
    stl_le_p(request + 8, 22);
    stl_le_p(request + 12, d->image->len);
    stl_le_p(request + 16, length);
    d->image_pending = length;
    mdm_sahara_packet(s, request, sizeof(request));
}

static bool mdm_sahara_image_header(MDM9x15State *s)
{
    MdmDownloadState *d = s->download;
    const uint8_t *h = d->image->data;
    unsigned n = d->image->len;

    if (n == 40) {
        if (!memcmp(h, "\177ELF", 4)) {
            if (h[4] != 1 || h[5] != 1 || h[6] != 1) {
                return false; /* ELF32 little endian, version1 only */
            }
            d->image_elf = true;
            d->image_total = 52;
            return true;
        }
        uint32_t size = ldl_le_p(h + 16);
        mdm_log(s, "sahara-image-header", ldl_le_p(h), ldl_le_p(h + 12), size);
        if (ldl_le_p(h) != (d->image_index ? 22 : 23) || ldl_le_p(h + 4) != 3 ||
            !size || size > MDM_MAX_IMAGE - 40 || ldl_le_p(h + 20) > size) {
            return false;
        }
        d->image_total = size + 40;
        d->image_size_known = true;
        return true;
    }
    if (!d->image_elf) {
        return false;
    }
    if (n == 52) {
        uint16_t machine = lduw_le_p(h + 18);
        uint32_t phoff = ldl_le_p(h + 28);
        unsigned phnum = lduw_le_p(h + 44);
        if (lduw_le_p(h + 16) != 2 || ldl_le_p(h + 20) != 1 ||
            (machine != 40 && machine != 164) || lduw_le_p(h + 40) != 52 ||
            lduw_le_p(h + 42) != 32 || !phnum || phnum > 1024 || phoff < 52 ||
            phoff > 65536 || phnum * 32 > 65536 - phoff) {
            return false;
        }
        d->image_phoff = phoff;
        d->image_phnum = phnum;
        d->image_total = phoff + phnum * 32;
        mdm_log(s, "sahara-elf-header", machine, ldl_le_p(h + 24), phnum);
        return true;
    }
    if (n != d->image_phoff + d->image_phnum * 32) {
        return false;
    }
    uint32_t end = n;
    unsigned loads = 0;
    for (unsigned i = 0; i < d->image_phnum; i++) {
        const uint8_t *ph = h + d->image_phoff + i * 32;
        uint32_t offset = ldl_le_p(ph + 4);
        uint32_t size = ldl_le_p(ph + 16);
        if (offset > MDM_MAX_IMAGE || size > MDM_MAX_IMAGE - offset) {
            return false;
        }
        if (ldl_le_p(ph) == 1 && size) {
            if (ldl_le_p(ph + 20) < size) {
                return false;
            }
            loads++;
        }
        if (size) {
            end = MAX(end, offset + size);
        }
    }
    if (!loads) {
        return false;
    }
    d->image_total = end;
    d->image_size_known = true;
    return true;
}

static void mdm_sahara_image_data(MDM9x15State *s, GByteArray *a)
{
    MdmDownloadState *d = s->download;
    unsigned length = MIN(a->len, d->image_pending);
    g_byte_array_append(d->image, a->data, length);
    g_byte_array_remove_range(a, 0, length);
    d->image_pending -= length;
    if (d->image_pending) {
        return;
    }
    if (!d->image_size_known) {
        if (!mdm_sahara_image_header(s)) {
            /* Reject an invalid image; never report a successful transfer. */
            uint8_t error[16] = {0};
            stl_le_p(error, 4);
            stl_le_p(error + 4, sizeof(error));
            stl_le_p(error + 8, 22);
            stl_le_p(error + 12, 7); /* invalid image header */
            mdm_sahara_packet(s, error, sizeof(error));
            return;
        }
    }
    if (d->image->len == d->image_total) {
        uint8_t end[16] = {0};
        stl_le_p(end, 4);
        stl_le_p(end + 4, sizeof(end));
        stl_le_p(end + 8, 22);
        d->image_complete = true;
        mdm_log(s, "sahara-image-complete", 22, d->image->len,
                mdm_dload_crc(d->image->data, d->image->len));
        mdm_sahara_packet(s, end, sizeof(end));
    } else {
        mdm_sahara_read_image(s);
    }
}

static bool mdm_sahara_finish_image(MdmDownloadState *d)
{
    const uint8_t *h = d->image->data;
    if (!d->image_elf) {
        d->loaded_roles |= ldl_le_p(h) == 23 ? 1 : 2; /* RPM, SBL2 */
    } else if (lduw_le_p(h + 18) == 40) {
        d->loaded_roles |= 4; /* modem applications processor */
    } else {
        uint32_t entry = ldl_le_p(h + 24);
        unsigned i;
        for (i = 0; i < d->dsp_count; i++) {
            if (d->dsp_entries[i] == entry) {
                break;
            }
        }
        if (i == d->dsp_count && d->dsp_count < 3) {
            d->dsp_entries[d->dsp_count++] = entry;
        }
    }
    /* Native N42 normal boot consumes RPM, SBL2, three separate Hexagon
     * executables and APPS. Repeated uploads cannot satisfy missing roles. */
    return d->loaded_roles == 7 && d->dsp_count == 3;
}

static void mdm_sahara_receive(MDM9x15State *s, const uint8_t *p, size_t n)
{
    GByteArray *a = s->tx[0];
    if (n > MDM_MAX_FRAME - a->len) {
        g_byte_array_set_size(a, 0);
        return;
    }
    g_byte_array_append(a, p, n);
    while (a->len) {
        if (s->download->image_pending) {
            mdm_sahara_image_data(s, a);
            continue;
        }
        if (a->len < 8) {
            return;
        }
        uint32_t command = ldl_le_p(a->data);
        uint32_t length = ldl_le_p(a->data + 4);
        if (length < 8 || length > 4096) {
            g_byte_array_set_size(a, 0);
            return;
        }
        if (length > a->len) {
            return;
        }
        mdm_log(s, "sahara", command, length,
                length >= 12 ? ldl_le_p(a->data + 8) : 0);
        if (command == 2 && length == 48 && ldl_le_p(a->data + 8) >= 1 &&
            ldl_le_p(a->data + 8) <= 2 && ldl_le_p(a->data + 12) >= 1 &&
            ldl_le_p(a->data + 12) <= 2 &&
            ldl_le_p(a->data + 12) <= ldl_le_p(a->data + 8) &&
            ldl_le_p(a->data + 16) == 0 && ldl_le_p(a->data + 20) == 0) {
            s->download->hello_accepted = true;
            mdm_log(s, "sahara-hello-accepted", 0, 0, 0);
            if (!s->download->image) {
                s->download->image = g_byte_array_new();
                s->download->image_total = 40;
                mdm_sahara_read_image(s);
            }
        } else if (command == 5 && length == 8 && s->download->image_complete) {
            MdmDownloadState *d = s->download;
            uint8_t pending[12] = {0};
            bool complete = mdm_sahara_finish_image(d);
            stl_le_p(pending, 6);
            stl_le_p(pending + 4, sizeof(pending));
            stl_le_p(pending + 8, complete ? 1 : 0);
            mdm_sahara_packet(s, pending, sizeof(pending));
            d->image_index++;
            d->image_complete = false;
            d->hello_accepted = false;
            d->image_pending = 0;
            d->image_size_known = false;
            d->image_elf = false;
            d->image_phoff = 0;
            d->image_phnum = 0;
            g_clear_pointer(&d->image, g_byte_array_unref);
            if (complete) {
                d->runtime = true;
                /* The firmware's radio starts searching now, independently
                 * of when the host opens its NAS client. Keep the shared
                 * core's normal search delay and SIM/radio readiness gates. */
                ios_bb_network_registration(
                    &s->baseband->bb, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
                s->reconnect_pending = true;
                s->data_logs = 0;
                mdm_log(s, "firmware-runtime", d->loaded_roles, d->dsp_count,
                        0);
            } else {
                mdm_sahara_begin(s);
            }
        }
        g_byte_array_remove_range(a, 0, length);
    }
}

static bool mdm_dload_boot(MDM9x15State *s, uint32_t entry)
{
    MdmDownloadState *d = s->download;
    uint32_t source, image_size;

    if (!d) {
        return false;
    }
    for (unsigned i = 0; i < 80; i++) {
        if (!(d->valid[i / 8] & (1u << (i % 8)))) {
            return false;
        }
    }
    source = ldl_le_p(d->ram + 20);
    image_size = ldl_le_p(d->ram + 28);
    if (ldl_le_p(d->ram) != 0x844bdcd1 || ldl_le_p(d->ram + 4) != 0x73d71034 ||
        ldl_le_p(d->ram + 8) != 0x15 || source < 80 ||
        source >= MDM_BOOT_SRAM_SIZE || !image_size ||
        image_size > MDM_BOOT_SRAM_SIZE - source ||
        ldl_le_p(d->ram + 32) > image_size ||
        ldl_le_p(d->ram + 24) != MDM_BOOT_SRAM_BASE + source ||
        ((entry & ~1u) != MDM_BOOT_SRAM_BASE &&
         (entry & ~1u) != MDM_BOOT_SRAM_BASE + source)) {
        return false;
    }
    for (unsigned i = 0; i < source + image_size; i++) {
        if (!(d->valid[i / 8] & (1u << (i % 8)))) {
            return false;
        }
    }
    /* The modem is a protocol/state model, like the shared IFX simulation;
     * it consumes the guest's firmware upload without executing a second ARM
     * CPU. Transition only after the complete boot image has arrived. */
    d->loader_started = true;
    mdm_log(s, "dload-boot", entry, source + image_size,
            mdm_dload_crc(d->ram, source + image_size));
    return true;
}

static void mdm_dload_command(MDM9x15State *s, const uint8_t *p, size_t n)
{
    /* Decode only after a delimiter, so USB transfer boundaries and escaped
     * bytes split across transfers have no effect on the command stream. */
    g_autoptr(GByteArray) decoded = g_byte_array_new();

    for (size_t i = 0; i < n; i++) {
        uint8_t byte = p[i];
        if (byte == 0x7d) {
            if (++i == n) {
                return;
            }
            byte = p[i] ^ 0x20;
        }
        g_byte_array_append(decoded, &byte, 1);
    }
    if (decoded->len < 3 || mdm_dload_crc(decoded->data, decoded->len - 2) !=
                                lduw_le_p(decoded->data + decoded->len - 2)) {
        return;
    }
    n = decoded->len - 2;
    p = decoded->data;
    mdm_log(s, "dload", p[0], n, 0);
    if (n == 1 && p[0] == 0x07) {
        /* Download protocol v9, minimum v1, 1024-byte writes, flashless.
         * Native ETL's parameter parser expects eight bytes and swaps maxWrite.
         * No claim of successful upload or execution is made here. */
        const uint8_t params[] = {0x08, 9, 1, 4, 0, 0, 0, 0};
        mdm_dload_reply(s, params, sizeof(params));
    } else if (n == 1 && p[0] == 0x06) {
        const uint8_t ack[] = {0x02};
        mdm_dload_reply(s, ack, sizeof(ack));
    } else if (n == 1 && p[0] == 0x0c) {
        /* Software-version response: command, string length, ASCII bytes. */
        static const char version[] = "QEMU MDM9x15 DLOAD";
        uint8_t response[2 + sizeof(version) - 1] = {0x0d, sizeof(version) - 1};
        memcpy(response + 2, version, sizeof(version) - 1);
        mdm_dload_reply(s, response, sizeof(response));
    } else if (n == 1 && p[0] == 0x16) {
        /* Stable virtual fuse serial derived from the shared modem identity.
         * The wire field is a bit count followed by opaque serial bytes. */
        uint8_t response[6] = {0x16, 32};
        stl_le_p(response + 2, mdm_fuse_serial(s->baseband->bb.imei));
        mdm_dload_reply(s, response, sizeof(response));
    } else if (n == 1 && p[0] == 0x17) {
        /* 64-bit hardware identifier: MDM9615 silicon ID from N42's supplied
         * BuildManifest (BbChipID), followed by the OEM/model fuse word.
         * ETL consumes the two words directly after a big-endian bit count. */
        uint8_t response[11] = {0x17, 0, 64};
        stl_le_p(response + 3, 0x006f10e1);
        mdm_dload_reply(s, response, sizeof(response));
    } else if (n == 1 && p[0] == 0x18) {
        /* No OEM trust root is provisioned in the simulated modem. Report its
         * zeroed virtual OTP hash, not a real device's credentials. The native
         * protocol represents a SHA-256 hash with a big-endian bit count. */
        const uint8_t response[35] = {0x18, 1, 0};
        mdm_dload_reply(s, response, sizeof(response));
    } else if (p[0] == 0x0f) {
        uint8_t response[] = {0x03, 0, 3}; /* NAK invalid length */
        if (n >= 7) {
            uint32_t address = ldl_be_p(p + 1);
            unsigned length = lduw_be_p(p + 5);
            uint32_t offset = address - MDM_BOOT_SRAM_BASE;
            mdm_log(s, "dload-write", address, length, n);
            if (length && length <= 1024 && length == n - 7) {
                response[2] = 2; /* NAK invalid destination */
                if (address >= MDM_BOOT_SRAM_BASE &&
                    offset < MDM_BOOT_SRAM_SIZE &&
                    length <= MDM_BOOT_SRAM_SIZE - offset) {
                    if (!s->download) {
                        s->download = g_new0(MdmDownloadState, 1);
                    }
                    memcpy(s->download->ram + offset, p + 7, length);
                    for (unsigned i = offset; i < offset + length; i++) {
                        s->download->valid[i / 8] |= 1u << (i % 8);
                    }
                    response[0] = 0x02; /* ACK only after storing the bytes */
                }
            }
        }
        mdm_dload_reply(s, response,
                        response[0] == 0x02 ? 1 : sizeof(response));
    } else if (n == 5 && p[0] == 0x05) {
        uint8_t response[] = {0x03, 0, 2}; /* NAK invalid destination/image */
        uint32_t entry = ldl_be_p(p + 1);
        if (mdm_dload_boot(s, entry)) {
            response[0] = 0x02;
        }
        mdm_dload_reply(s, response,
                        response[0] == 0x02 ? 1 : sizeof(response));
        if (response[0] == 0x02) {
            mdm_sahara_begin(s);
        }
    } else {
        /* NAK invalid command. Never acknowledge unimplemented memory writes. */
        const uint8_t nak[] = {0x03, 0, 6};
        mdm_dload_reply(s, nak, sizeof(nak));
    }
}

static void mdm_dload_receive(MDM9x15State *s, const uint8_t *p, size_t n)
{
    GByteArray *a = s->tx[0];

    for (size_t i = 0; i < n; i++) {
        if (p[i] == 0x7e) {
            if (a->len && a->len < MDM_MAX_FRAME) {
                mdm_dload_command(s, a->data, a->len);
            }
            g_byte_array_set_size(a, 0);
        } else if (a->len < MDM_MAX_FRAME) {
            /* Reaching the limit discards this entire frame until a delimiter. */
            g_byte_array_append(a, p + i, 1);
        }
    }
}

static void mdm_receive(MDM9x15State *s, unsigned iface, const uint8_t *p,
                        size_t n)
{
    if (iface == 0) {
        if (s->download && s->download->runtime) {
            /* Runtime interface0 is diagnostics, not the QMI control port. */
            mdm_log(s, "diag-unhandled", n, n ? p[0] : 0, 0);
            return;
        }
        if (s->download && s->download->loader_started) {
            mdm_sahara_receive(s, p, n);
        } else {
            mdm_dload_receive(s, p, n);
        }
        return;
    }
    mdm_qmux_receive(s, s->tx[iface], iface, p, n);
}
static int mdm_read(MDM9x15State *s, unsigned iface, uint8_t *out, size_t cap)
{
    GBytes *b = g_queue_pop_head(&s->rx[iface]);
    if (!b)
        return 0;
    gsize size;
    const uint8_t *p = g_bytes_get_data(b, &size);
    size_t n = MIN(size, cap);
    memcpy(out, p, n);
    if (n < size)
        g_queue_push_head(&s->rx[iface], g_bytes_new(p + n, size - n));
    g_bytes_unref(b);
    /* Finish the final DONE response before removing its USB endpoint. The
     * electrical reconnect runs outside the active EHCI packet callback. */
    if (iface == 0 && s->reconnect_pending && g_queue_is_empty(&s->rx[0])) {
        s->reconnect_pending = false;
        timer_mod(s->reconnect_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
    }
    return n;
}
static void mdm_control(USBDevice *dev, USBPacket *p, int request, int value,
                        int index, int length, uint8_t *data)
{
    MDM9x15State *s = MDM9X15(dev);
    int ret =
        usb_desc_handle_control(dev, p, request, value, index, length, data);
    mdm_log(s, "control", request, ((unsigned)value << 16) | (index & 0xffff),
            length);
    if (ret >= 0) {
        if (request == 9 && value == 1 && s->download && s->download->runtime) {
            mdm_qmi_ready(s);
        }
        return;
    }
    if (request == (EndpointOutRequest | USB_REQ_CLEAR_FEATURE) && value == 0 &&
        length == 0 && !(index & ~0x8f) && (index & 15)) {
        int pid = index & USB_DIR_IN ? USB_TOKEN_IN : USB_TOKEN_OUT;
        USBEndpoint *ep = usb_ep_get(dev, pid, index & 15);

        /* ENDPOINT_HALT is feature zero. Native USB setup clears each pipe
         * before using it, even when it has never stalled. Only configured
         * bulk/interrupt endpoints support this feature. */
        if (ep->type == USB_ENDPOINT_XFER_BULK ||
            ep->type == USB_ENDPOINT_XFER_INT) {
            usb_ep_set_halted(dev, pid, index & 15, false);
            p->actual_length = 0;
            return;
        }
    }
    if (index >= 0 && index < MDM_INTERFACES) {
        if (request == 0x2100) {
            mdm_receive(s, index, data, length);
            p->actual_length = length;
            return;
        }
        if (request == 0xa101) {
            p->actual_length = mdm_read(s, index, data, length);
            return;
        }
        if (request == 0x2122) {
            p->actual_length = 0;
            return;
        } /* CDC control-line state */
    }
    p->status = USB_RET_STALL;
}
static bool mdm_is_notification(USBDevice *dev, unsigned ep)
{
    /* Native management QMI is controlIF1/dataIF2; subsequent pairs are3/4,
     * 5/6, etc (native factory eeaaa/eeadc). Diagnostic/downloadIF0 stays bulk. */
    return (ep == 2 && dev->usb_desc == &mdm_runtime_desc) ||
           (ep >= 4 && ep <= 12 && !(ep & 1));
}

static void mdm_data(USBDevice *dev, USBPacket *p)
{
    MDM9x15State *s = MDM9X15(dev);
    unsigned ep = p->ep->nr;
    if (p->pid == USB_TOKEN_IN && mdm_is_notification(dev, ep)) {
        unsigned i = ep - 1;
        if (!g_queue_is_empty(&s->rx[i])) {
            uint8_t notification[8] = {0xa1, 1, 0, 0, i, 0, 0, 0};
            usb_packet_copy(p, notification,
                            MIN(sizeof notification, p->iov.size));
            return;
        }
        p->status = USB_RET_NAK;
        return;
    }
    if (ep < 1 || ep > MDM_INTERFACES) {
        p->status = USB_RET_STALL;
        return;
    }
    unsigned iface = ep - 1;
    if (p->pid == USB_TOKEN_OUT) {
        if (p->iov.size > MDM_MAX_FRAME) {
            p->status = USB_RET_STALL;
            return;
        }
        g_autofree uint8_t *bytes = g_malloc(p->iov.size);
        qemu_iovec_to_buf(&p->iov, 0, bytes, p->iov.size);
        uint32_t prefix = 0;
        for (unsigned i = 0; i < MIN(p->iov.size, 4); i++) {
            prefix |= (uint32_t)bytes[i] << (8 * i);
        }
        mdm_log(s, "bulk-out", iface, p->iov.size, prefix);
        mdm_receive(s, iface, bytes, p->iov.size);
        p->actual_length = p->iov.size;
    } else if (p->pid == USB_TOKEN_IN) {
        uint8_t bytes[MDM_MAX_FRAME];
        unsigned n = mdm_read(s, iface, bytes, MIN(sizeof bytes, p->iov.size));
        if (n)
            usb_packet_copy(p, bytes, n);
        else
            p->status = USB_RET_NAK;
    } else
        p->status = USB_RET_STALL;
}
static void mdm_reset(USBDevice *dev)
{
    MDM9x15State *s = MDM9X15(dev);
    s->logs = 0;
    mdm_bsp_file_reset(s, false);
    memset(s->wds, 0, MDM_INTERFACES * sizeof(s->wds[0]));
    s->data_logs = 0;
    s->qmi_ready_sent = false;
    memset(s->uim_client, 0, MDM_INTERFACES * sizeof(s->uim_client[0]));
    memset(s->uim_dirty, 0, MDM_INTERFACES * sizeof(s->uim_dirty[0]));
    memset(s->wms_inbox.pending, 0,
           MDM_INTERFACES * sizeof(s->wms_inbox.pending[0]));
    memset(s->wms_inbox.awaiting_ack, 0,
           MDM_INTERFACES * sizeof(s->wms_inbox.awaiting_ack[0]));
    memset(s->wms_client, 0, MDM_INTERFACES * sizeof(s->wms_client[0]));
    memset(s->wms_smsc_sent, 0, MDM_INTERFACES * sizeof(s->wms_smsc_sent[0]));
    memset(s->wms_events, 0, MDM_INTERFACES * sizeof(s->wms_events[0]));
    memset(s->wms_notify, 0, MDM_INTERFACES * sizeof(s->wms_notify[0]));
    memset(s->pds_client, 0, MDM_INTERFACES * sizeof(s->pds_client[0]));
    memset(s->pds_dirty, 0, MDM_INTERFACES * sizeof(s->pds_dirty[0]));
    memset(s->voice_events, 0, MDM_INTERFACES * sizeof(s->voice_events[0]));
    memset(s->voice_client, 0, MDM_INTERFACES * sizeof(s->voice_client[0]));
    memset(s->voice_dirty, 0, MDM_INTERFACES * sizeof(s->voice_dirty[0]));
    /* A USB reset must not invalidate the independent reliable UART link. */
    memset(s->qmi_protocol, 0, MDM_INTERFACES * sizeof(s->qmi_protocol[0]));
    memset(s->qmi_power_save, 0, MDM_INTERFACES * sizeof(s->qmi_power_save[0]));
    mdm_power_filter_reset(s, MDM_INTERFACES);
    memset(s->qmi_uim_events, 0, MDM_INTERFACES * sizeof(s->qmi_uim_events[0]));
    memset(s->pds_events, 0, MDM_INTERFACES * sizeof(s->pds_events[0]));
    memset(s->pds_fix_due, 0, MDM_INTERFACES * sizeof(s->pds_fix_due[0]));
    memset(s->dms_mode_events, 0,
           MDM_INTERFACES * sizeof(s->dms_mode_events[0]));
    memset(s->dms_mode_notified, 0,
           MDM_INTERFACES * sizeof(s->dms_mode_notified[0]));
    for (unsigned i = 0; i < MDM_NAS_CLIENTS; i++) {
        if (s->nas[i].iface != MDM_INTERFACES) {
            memset(&s->nas[i], 0, sizeof(s->nas[i]));
        }
    }
    for (unsigned i = 0; i < MDM_INTERFACES; i++) {
        g_queue_clear_full(&s->rx[i], (GDestroyNotify)g_bytes_unref);
        g_byte_array_set_size(s->tx[i], 0);
    }
}
/* HSIC readiness announces a powered peer; a USB connection additionally
 * needs the host's link handshake and PHY training. The 10ms virtual training
 * interval is a model fixture, not a measured silicon latency. It separates
 * link establishment from the AP's power-on/reset sequence; no USB controller
 * register or native driver instruction is changed.
 * Suspending an established link does not unplug or reset the modem. */
static void mdm_link_connect(void *opaque)
{
    MDM9x15State *s = opaque;
    USBDevice *dev = USB_DEVICE(s);

    if (s->ready && s->host_ready && !s->reconnect_detached && !dev->attached) {
        usb_device_attach(dev, &error_fatal);
        mdm_log(s, "hsic-connect", 0, 0, 0);
    }
}

static void mdm_link_update(MDM9x15State *s)
{
    if (s->ready && s->host_ready && !s->reconnect_detached &&
        !USB_DEVICE(s)->attached) {
        timer_mod(s->link_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
    } else {
        timer_del(s->link_timer);
    }
}

static void mdm_reconnect(void *opaque)
{
    MDM9x15State *s = opaque;
    USBDevice *dev = USB_DEVICE(s);

    if (!s->download || !s->download->runtime ||
        (s->managed_power && !s->ready)) {
        s->reconnect_detached = false;
        return;
    }
    if (!s->reconnect_detached) {
        timer_del(s->link_timer);
        if (dev->attached) {
            usb_device_detach(dev);
        }
        mdm_reset(dev); /* USB queues reset; loaded modem state survives. */
        s->reconnect_detached = true;
        s->link_started = false;
        qemu_set_irq(s->device_ready, 0);
        mdm_log(s, "runtime-usb-detach", 0, 0, 0);
        timer_mod(s->reconnect_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
    } else {
        dev->usb_desc = &mdm_runtime_desc;
        s->reconnect_detached = false;
        if (s->managed_power) {
            mdm_link_update(s);
        } else if (!dev->attached) {
            usb_device_attach(dev, &error_fatal);
        }
        qemu_set_irq(s->device_ready, s->ready);
        mdm_log(s, "runtime-usb-attach", 0x9034, 0, 0);
    }
}
/* A link reset does not remove power. The board's supply/reset lines do:
 * advertise a USB peer and DEVICE_READY only while the modem can respond. */
static void mdm_update_power(MDM9x15State *s)
{
    USBDevice *dev = USB_DEVICE(s);
    bool ready = s->powered && !s->reset_asserted;

    if (ready == s->ready) {
        return;
    }
    s->ready = ready;
    s->link_started = ready && s->host_ready;
    mdm_log(s, "power", s->powered, s->reset_asserted, ready);
    if (ready) {
        if (s->managed_power) {
            mdm_link_update(s);
        } else if (!dev->attached) {
            usb_device_attach(dev, &error_fatal);
        }
    } else {
        timer_del(s->link_timer);
        timer_del(s->reconnect_timer);
        s->reconnect_pending = false;
        s->reconnect_detached = false;
        if (dev->attached) {
            usb_device_detach(dev);
        }
        mdm_reset(dev);
        mdm_serial_reset(s);
        mdm_bsp_file_reset(s, true);
        timer_del(s->qmi_event_timer);
        memset(s->uim_client, 0, sizeof(s->uim_client));
        memset(s->uim_dirty, 0, sizeof(s->uim_dirty));
        s->uim_last_len = 0;
        /* Keep the transaction sequence across resets to reject stale ACKs. */
        uint32_t next_transaction = s->wms_inbox.next_transaction;
        memset(&s->wms_inbox, 0, sizeof(s->wms_inbox));
        s->wms_inbox.next_transaction = next_transaction;
        memset(s->wms_client, 0, sizeof(s->wms_client));
        memset(s->wms_smsc_sent, 0, sizeof(s->wms_smsc_sent));
        memset(s->wms_last_smsc, 0, sizeof(s->wms_last_smsc));
        memset(s->wms_events, 0, sizeof(s->wms_events));
        memset(s->wms_notify, 0, sizeof(s->wms_notify));
        memset(s->wms_routes, 0, sizeof(s->wms_routes));
        s->wms_routes_len = 0;
        s->wms_status_reports = false;
        memset(s->wms_broadcast, 0, sizeof(s->wms_broadcast));
        memset(s->wms_timers, 0, sizeof(s->wms_timers));
        memset(s->voice_events, 0, sizeof(s->voice_events));
        memset(s->voice_client, 0, sizeof(s->voice_client));
        memset(s->voice_dirty, 0, sizeof(s->voice_dirty));
        memset(s->voice_ended, 0, sizeof(s->voice_ended));
        memset(s->nas, 0, sizeof(s->nas));
        mdm_pds_reset(s);
        memset(s->dms_mode_events, 0, sizeof(s->dms_mode_events));
        memset(s->dms_mode_notified, 0, sizeof(s->dms_mode_notified));
        memset(s->next_cid, 0, sizeof(s->next_cid));
        memset(s->wds, 0, sizeof(s->wds));
        s->qmi_protocol[MDM_INTERFACES] = 0;
        s->qmi_power_save[MDM_INTERFACES] = 0;
        for (unsigned service = 0; service < 256; service++) {
            g_clear_pointer(&s->qmi_power_filter[MDM_INTERFACES][service],
                            g_bytes_unref);
        }
        s->qmi_uim_events[MDM_INTERFACES] = 0;
        g_clear_pointer(&s->download, mdm_download_free);
        dev->usb_desc = &mdm_desc;
    }
    qemu_set_irq(s->device_ready, ready);
    qemu_set_irq(s->reset_detect, !ready);
}

static void mdm_host_ready_input(void *opaque, int pin, int level)
{
    MDM9x15State *s = opaque;

    s->host_ready = !!level;
    s->link_started |= s->host_ready;
    if (s->managed_power) {
        mdm_link_update(s);
    }
    /* Announce the newly booted modem, then acknowledge the host's HSIC
     * suspend/resume handshake without power-cycling the modem or its SIM. */
    qemu_set_irq(s->device_ready, s->ready && !s->reconnect_detached &&
                                      (!s->link_started || s->host_ready));
}

static void mdm_power_input(void *opaque, int pin, int level)
{
    MDM9x15State *s = opaque;

    s->powered = !!level;
    mdm_update_power(s);
}

static void mdm_reset_input(void *opaque, int pin, int level)
{
    MDM9x15State *s = opaque;

    s->reset_asserted = !!level;
    mdm_update_power(s);
}

static void mdm_instance_init(Object *obj)
{
    MDM9x15State *s = MDM9X15(obj);

    qdev_init_gpio_in_named(DEVICE(obj), mdm_power_input, "power", 1);
    qdev_init_gpio_in_named(DEVICE(obj), mdm_reset_input, "reset", 1);
    qdev_init_gpio_in_named(DEVICE(obj), mdm_host_ready_input, "host-ready", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->device_ready, "device-ready", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->reset_detect, "reset-detect", 1);
}
static void mdm_realize(USBDevice *dev, Error **errp)
{
    MDM9x15State *s = MDM9X15(dev);
    if (!s->baseband) {
        error_setg(errp, "MDM9x15 requires an ios-baseband backend");
        return;
    }
    if (s->managed_power) {
        dev->auto_attach = false;
    }
    for (unsigned i = 0; i < MDM_INTERFACES; i++) {
        g_queue_init(&s->rx[i]);
        s->tx[i] = g_byte_array_new();
    }
    s->reconnect_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mdm_reconnect, s);
    s->link_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mdm_link_connect, s);
    s->qmi_event_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mdm_qmi_events, s);
    s->baseband->bb.call_event = mdm_voice_changed;
    s->baseband->bb.call_opaque = s;
    s->baseband->bb.sms_event = mdm_wms_received;
    s->baseband->bb.sms_opaque = s;
    s->serial = g_new0(MdmSerial, 1);
    mdm_serial_router_defaults(s->serial);
    s->serial->rx = g_byte_array_new();
    s->serial->tx = g_byte_array_new();
    s->serial->qmux = g_byte_array_new();
    s->serial->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mdm_serial_flush, s);
    s->serial->chr =
        qemu_chardev_new(NULL, TYPE_CHARDEV_MDM9X15, NULL, NULL, &error_abort);
    MDM_CHARDEV(s->serial->chr)->dev = s;
    usb_desc_init(dev);
    if (s->managed_power) {
        qemu_set_irq(s->device_ready, 0);
        qemu_set_irq(s->reset_detect, 1);
    }
}
static void mdm_unrealize(USBDevice *dev)
{
    MDM9x15State *s = MDM9X15(dev);
    for (unsigned i = 0; i < G_N_ELEMENTS(s->bsp_files); i++) {
        g_clear_pointer(&s->bsp_files[i].staging, g_byte_array_unref);
        g_clear_pointer(&s->bsp_files[i].committed, g_byte_array_unref);
    }
    if (s->baseband && s->baseband->bb.call_opaque == s) {
        s->baseband->bb.call_event = NULL;
        s->baseband->bb.call_opaque = NULL;
    }
    if (s->baseband && s->baseband->bb.sms_opaque == s) {
        s->baseband->bb.sms_event = NULL;
        s->baseband->bb.sms_opaque = NULL;
    }
    if (s->serial) {
        MDM_CHARDEV(s->serial->chr)->dev = NULL;
        timer_free(s->serial->timer);
        object_unref(OBJECT(s->serial->chr));
        g_byte_array_unref(s->serial->rx);
        g_byte_array_unref(s->serial->tx);
        g_byte_array_unref(s->serial->qmux);
        g_clear_pointer(&s->serial, g_free);
    }
    mdm_power_filter_reset(s, MDM_INTERFACES + 1);
    g_clear_pointer(&s->qmi_event_timer, timer_free);
    g_clear_pointer(&s->reconnect_timer, timer_free);
    g_clear_pointer(&s->link_timer, timer_free);
    g_clear_pointer(&s->download, mdm_download_free);
    for (unsigned i = 0; i < MDM_INTERFACES; i++) {
        g_queue_clear_full(&s->rx[i], (GDestroyNotify)g_bytes_unref);
        g_clear_pointer(&s->tx[i], g_byte_array_unref);
    }
}
static const Property mdm_properties[] = {
    DEFINE_PROP_LINK("baseband", MDM9x15State, baseband, TYPE_IOS_BASEBAND,
                     IosBasebandState *),
    DEFINE_PROP_BOOL("managed-power", MDM9x15State, managed_power, false),
};
static char *mdm_get_boot_status(Object *obj, Error **errp)
{
    MDM9x15State *s = MDM9X15(obj);
    MdmDownloadState *d = s->download;

    return g_strdup_printf(
        "{\"powered\":%s,\"reset\":%s,\"ready\":%s,"
        "\"loader\":%s,\"hello\":%s,\"image_index\":%u,"
        "\"image_received\":%u,\"image_total\":%u,"
        "\"image_pending\":%u,\"image_complete\":%s,\"runtime\":%s,"
        "\"carrier_file_bytes\":[%u,%u,%u,%u,%u]}",
        s->powered ? "true" : "false", s->reset_asserted ? "true" : "false",
        s->ready ? "true" : "false", d && d->loader_started ? "true" : "false",
        d && d->hello_accepted ? "true" : "false", d ? d->image_index : 0,
        d && d->image ? d->image->len : 0, d ? d->image_total : 0,
        d ? d->image_pending : 0, d && d->image_complete ? "true" : "false",
        d && d->runtime ? "true" : "false",
        s->bsp_files[0].committed ? s->bsp_files[0].committed->len : 0,
        s->bsp_files[1].committed ? s->bsp_files[1].committed->len : 0,
        s->bsp_files[2].committed ? s->bsp_files[2].committed->len : 0,
        s->bsp_files[3].committed ? s->bsp_files[3].committed->len : 0,
        s->bsp_files[4].committed ? s->bsp_files[4].committed->len : 0);
}
static void mdm_class_init(ObjectClass *klass, const void *data)
{
    object_class_property_add_str(klass, "boot-status", mdm_get_boot_status,
                                  NULL);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);
    device_class_set_props(DEVICE_CLASS(klass), mdm_properties);
    uc->product_desc = "Qualcomm MDM9x15 HSIC bring-up";
    uc->usb_desc = &mdm_desc;
    uc->realize = mdm_realize;
    uc->unrealize = mdm_unrealize;
    uc->handle_attach = usb_desc_attach;
    uc->handle_reset = mdm_reset;
    uc->handle_control = mdm_control;
    uc->handle_data = mdm_data;
}
static const TypeInfo mdm_type = {.name = TYPE_MDM9X15,
                                  .parent = TYPE_USB_DEVICE,
                                  .instance_size = sizeof(MDM9x15State),
                                  .instance_init = mdm_instance_init,
                                  .class_init = mdm_class_init};
static void mdm_register_types(void)
{
    type_register_static(&mdm_chr_type);
    type_register_static(&mdm_type);
}
type_init(mdm_register_types)
