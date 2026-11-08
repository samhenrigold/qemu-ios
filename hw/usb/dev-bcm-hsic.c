/*
 * Broadcom BCM4334 HSIC transport qualification fixture.
 *
 * The N42 stock driver matches USB vendor 0a5c, running product bd1a and
 * discovers interrupt-IN/bulk-IN/bulk-OUT pipes by descriptor attributes.
 * Its USB Vendor/Product Name strings supply chip/module matching tokens.
 * This opt-in fixture exposes those interfaces and records the native wire
 * protocol. Unsupported commands stall; it does not yet claim a WLAN link.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/usb/usb.h"
#include "desc.h"
#include "qom/object.h"
#include "hw/net/bcm_fullmac.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "qemu/iov.h"
#include <zlib.h>

#define BCM_DL_MAX (8 * 1024 * 1024)
#define BCM_TRX_MAGIC 0x30524448u
#define BCM_TRX_UNCOMP_IMAGE 0x20u
#define BCM_DL_WAITING 0
#define BCM_DL_READY 1
#define BCM_DL_BAD_HDR 2
#define BCM_DL_BAD_CRC 3
#define BCM_DL_RUNNABLE 4
#define BCM_DL_IMAGE_TOOBIG 7

#define TYPE_BCM_HSIC "usb-bcm-hsic"
OBJECT_DECLARE_SIMPLE_TYPE(BCMHSICState, BCM_HSIC)
struct BCMHSICState {
    USBDevice parent_obj;
    unsigned log_count;
    GByteArray *download;
    uint32_t dl_state, dl_size;
    bool dl_started, running;
    bool reset_asserted;
    BCMFullmacState fullmac;
    GQueue *control_rx, *data_rx;
    bool control_notified;
    bool control_prefixed;
    uint8_t control_prefix[16];
    uint16_t interrupt_seq, bulk_rx_seq, bulk_tx_seq;
    size_t bulk_rx_offset;
    unsigned bulk_error_log;
};

static const USBDescStrings bcm_strings = {
    [1] = "s=B2",
    [2] = "P=N41 V=t",
    [3] = "QEMU-BCM4334",
};
static const USBDescDevice bcm_device = {
    .bcdUSB = 0x0200,
    .bMaxPacketSize0 = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]){{
        .bNumInterfaces = 1,
        .bConfigurationValue = 1,
        .bmAttributes = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER,
        .nif = 1,
        .ifs = (USBDescIface[]){{
            .bInterfaceNumber = 0,
            .bNumEndpoints = 3,
            .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
            .bInterfaceSubClass = 2,
            .bInterfaceProtocol = 0xff,
            .eps =
                (USBDescEndpoint[]){
                    {.bEndpointAddress = USB_DIR_IN | 1,
                     .bmAttributes = USB_ENDPOINT_XFER_INT,
                     .wMaxPacketSize = 16,
                     .bInterval = 4},
                    {.bEndpointAddress = USB_DIR_IN | 2,
                     .bmAttributes = USB_ENDPOINT_XFER_BULK,
                     .wMaxPacketSize = 512},
                    {.bEndpointAddress = 3,
                     .bmAttributes = USB_ENDPOINT_XFER_BULK,
                     .wMaxPacketSize = 512},
                },
        }},
    }},
};
static const USBDesc bcm_desc = {
    .id = {.idVendor = 0x0a5c,
           .idProduct = 0xbd1a,
           .bcdDevice = 0x0001,
           .iManufacturer = 1,
           .iProduct = 2,
           .iSerialNumber = 3},
    .high = &bcm_device,
    .str = bcm_strings,
};

/* Broadcom DL_GETVER's first two little-endian words are chip/revision
 * (Linux brcmfmac/usb.c bootrom_id_le). Native 11A465 asks c105/value1/
 * index0/length8 and its chip-manager factory at 80891070 matches 4334.
 * The Linux postboot sentinel A123 is rejected by that native factory.
 * Revision 2 is a virtual chip fixture pending further qualification. */
static void bcm_download_reset(BCMHSICState *s)
{
    if (s->download) {
        g_byte_array_set_size(s->download, 0);
    }
    s->dl_state = BCM_DL_WAITING;
    s->dl_size = 0;
    s->dl_started = false;
    s->running = false;
}

static bool bcm_download_data(BCMHSICState *s, const uint8_t *data, size_t size)
{
    if (!s->running && s->dl_state == BCM_DL_RUNNABLE && !size) {
        return true; /* A terminating USB ZLP carries no additional firmware. */
    }
    if (s->running ||
        (s->dl_state > BCM_DL_READY && s->dl_state != BCM_DL_RUNNABLE)) {
        return false;
    }
    /* A native uncompressed transfer may include unused buffer tail after
     * the declared TRX end. Keep counting bounded USB input until GO, but
     * never interpret that tail as firmware or reopen a failed image. */
    if (s->dl_state == BCM_DL_RUNNABLE) {
        if (!(ldl_le_p(s->download->data + 12) & BCM_TRX_UNCOMP_IMAGE) ||
            size > BCM_DL_MAX - s->download->len) {
            return false;
        }
        g_byte_array_append(s->download, data, size);
        return true;
    }
    /* Native 11A465 sends the first TRX chunk directly from DL_WAITING,
     * without issuing DL_START. The ROM is already listening in that state. */
    if (!s->dl_started) {
        s->dl_started = true;
        if (!s->download) {
            s->download = g_byte_array_new();
        }
    }
    if (size > BCM_DL_MAX - s->download->len) {
        s->dl_state = BCM_DL_IMAGE_TOOBIG;
        return false;
    }
    g_byte_array_append(s->download, data, size);
    if (!s->dl_size && s->download->len >= 28) {
        s->dl_size = ldl_le_p(s->download->data + 4);
        if (ldl_le_p(s->download->data) != BCM_TRX_MAGIC || s->dl_size < 28) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "BCM HSIC invalid TRX magic=%08x size=%u\n",
                          ldl_le_p(s->download->data), s->dl_size);
            s->dl_state = BCM_DL_BAD_HDR;
            return false;
        }
        if (s->dl_size > BCM_DL_MAX) {
            s->dl_state = BCM_DL_IMAGE_TOOBIG;
            return false;
        }
        if (ldl_le_p(s->download->data + 12) & BCM_TRX_UNCOMP_IMAGE) {
            uint32_t firmware = ldl_le_p(s->download->data + 16);
            uint32_t nvram = ldl_le_p(s->download->data + 24);
            uint32_t payload = s->dl_size - 28;
            if (!firmware || firmware > payload || nvram > payload - firmware ||
                (nvram && nvram != payload - firmware)) {
                s->dl_state = BCM_DL_BAD_HDR;
                return false;
            }
        }
        s->dl_state = BCM_DL_READY;
        qemu_log_mask(LOG_UNIMP, "BCM HSIC TRX header size=%u\n", s->dl_size);
    }
    if (s->dl_size && s->download->len > s->dl_size &&
        !(ldl_le_p(s->download->data + 12) & BCM_TRX_UNCOMP_IMAGE)) {
        s->dl_state = BCM_DL_BAD_HDR;
        return false;
    }
    if (s->dl_size && s->download->len >= s->dl_size) {
        uint32_t expected = ldl_le_p(s->download->data + 8);
        uint32_t actual =
            crc32(0, s->download->data + 12, s->dl_size - 12) ^ UINT32_MAX;
        /* Native uncompressed TRX is rebuilt with per-unit NVRAM and keeps
         * the original disk CRC. Its firmware/NVRAM lengths are checked
         * above; DL_BAD_CRC is the compressed-download checksum failure.
         * Applying that checksum to uncompressed data rejects the original
         * 11A465 loader's otherwise unchanged firmware payload. */
        bool uncompressed =
            ldl_le_p(s->download->data + 12) & BCM_TRX_UNCOMP_IMAGE;
        s->dl_state = uncompressed || actual == expected ? BCM_DL_RUNNABLE
                                                         : BCM_DL_BAD_CRC;
        qemu_log_mask(LOG_UNIMP, "BCM HSIC downloaded %u bytes, TRX state=%u\n",
                      s->dl_size, s->dl_state);
    }
    return true;
}

/* BCM4334 ROM register-read transport. The original native loader reads
 * the SW-region bit boundaries at OTP+38/+3A, then fetches 16-bit CIS words.
 * Expose one bounded, immutable virtual module identity. Calibration remains
 * supplied by the native firmware/NVRAM templates, as on the other profiles. */
static bool bcm_otp_word(uint32_t address, uint32_t *value)
{
    static const uint8_t version[] = "\x05\x00"
                                     "s=B2\0M=HEIN P=N41 V=t\0\0\0\xff";
    const uint32_t base = 0x18000800;
    const unsigned start = 0x40;
    const unsigned tuple_size = 2 + sizeof(version) - 1;
    const unsigned end = (start + tuple_size + 1 + 1) & ~1u;
    unsigned offset;
    uint32_t word = 0;

    if (address < base || address - base > 510 || (address & 1)) {
        return false;
    }
    offset = address - base;
    if (offset == 0x38 || offset == 0x3a) {
        *value = (offset == 0x38 ? start : end) << 3;
        return true;
    }
    for (unsigned i = 0; i < 2; i++) {
        unsigned pos = offset + i;
        uint8_t byte = 0xff;
        if (pos == start) {
            byte = 0x15; /* CISTPL_VERS_1 */
        } else if (pos == start + 1) {
            byte = sizeof(version) - 1;
        } else if (pos >= start + 2 && pos < start + tuple_size) {
            byte = version[pos - start - 2];
        }
        word |= (uint32_t)byte << (i * 8);
    }
    *value = word;
    return true;
}

static int bcm_vendor_control(BCMHSICState *s, int request, int value,
                              int index, int length, uint8_t *data)
{
    uint8_t reply[8] = {0};
    if (request == 0xc111 && length == 16) {
        uint32_t word;
        uint32_t address = ((uint32_t)(uint16_t)index << 16) | (uint16_t)value;
        if (!bcm_otp_word(address, &word)) {
            return -1;
        }
        memset(data, 0, 16);
        stl_le_p(data + 12, word);
        return 16;
    }
    if ((request & 0xff00) != 0xc100 || value != 1 || index != 0 ||
        length < 0) {
        return -1;
    }
    switch (request & 0xff) {
    case 5: /* DL_GETVER */
        /* The ROM identifies the physical chip. After GO, firmware uses
         * Broadcom's runtime sentinel. Apple's native check at 8089fbb4
         * compares all eight bytes against { A123, 1 } before enabling I/O;
         * returning that sentinel before download breaks chip selection. */
        stl_le_p(reply, s->running ? 0xa123 : 0x4334);
        stl_le_p(reply + 4, s->running ? 1 : 2);
        break;
    case 3: /* DL_START */
        bcm_download_reset(s);
        s->dl_started = true;
        if (!s->download) {
            s->download = g_byte_array_new();
        }
        /* fall through */
    case 0: /* DL_GETSTATE */
        stl_le_p(reply, s->dl_state);
        stl_le_p(reply + 4, s->download ? s->download->len : 0);
        break;
    case 2: /* DL_GO: only a complete, mode-validated image can run. */
        if (s->dl_state != BCM_DL_RUNNABLE) {
            return -1;
        }
        s->running = true;
        s->dl_started = false;
        s->log_count = 0; /* retain visibility of the post-download CDC phase */
        stl_le_p(reply, s->dl_state);
        stl_le_p(reply + 4, s->download->len);
        break;
    default:
        return -1;
    }
    unsigned size = MIN((unsigned)length, sizeof(reply));
    if (s->log_count < 256) {
        s->log_count++;
        qemu_log_mask(
            LOG_UNIMP,
            "BCM HSIC boot reply request=%02x state=%u bytes=%u running=%d\n",
            request & 0xff, s->dl_state, s->download ? s->download->len : 0,
            s->running);
    }
    memcpy(data, reply, size);
    return size;
}

static uint16_t bcm_header_sum(const uint8_t *header)
{
    uint32_t sum = 0;
    for (unsigned i = 0; i < 16; i += 2) {
        sum += lduw_le_p(header + i);
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return sum;
}

static void bcm_make_header(uint8_t *header, unsigned channel,
                            uint16_t sequence, uint16_t length)
{
    memset(header, 0, 16);
    stw_le_p(header, 0x8001 | (channel << 12));
    stw_le_p(header + 4, length);
    stw_le_p(header + 6, sequence);
    stw_le_p(header + 2, ~bcm_header_sum(header));
}

/* CDC command/reply uses endpoint zero; BDC data and firmware events use
 * bulk pipes. Queue payloads independently of the SDPCM transport header. */
static void bcm_fullmac_output(void *opaque, unsigned channel,
                               const uint8_t *data, uint32_t len)
{
    BCMHSICState *s = opaque;
    GQueue *queue = channel == BCM_FULLMAC_CONTROL ? s->control_rx : s->data_rx;
    if (g_queue_get_length(queue) >= 256) {
        qemu_log_mask(LOG_GUEST_ERROR, "BCM HSIC receive queue full\n");
        return;
    }
    if (channel != BCM_FULLMAC_CONTROL && s->control_prefixed) {
        /* USB bulk channel 2 carries both BDC data and firmware events;
         * channel 1 is the separate interrupt pipe, unlike SDPCM events. */
        g_autofree uint8_t *packet = g_malloc(16 + len);
        bcm_make_header(packet, 2, s->bulk_rx_seq++, 16 + len);
        memcpy(packet + 16, data, len);
        g_queue_push_tail(queue, g_bytes_new(packet, 16 + len));
    } else {
        g_queue_push_tail(queue, g_bytes_new(data, len));
    }
    if (channel == BCM_FULLMAC_CONTROL) {
        s->control_notified = false;
        usb_wakeup(usb_ep_get(&s->parent_obj, USB_TOKEN_IN, 1), 0);
    } else {
        usb_wakeup(usb_ep_get(&s->parent_obj, USB_TOKEN_IN, 2), 0);
    }
}

static bool bcm_fullmac_ready(void *opaque)
{
    BCMHSICState *s = opaque;
    return g_queue_get_length(s->data_rx) < 16;
}

/* Native Apple HSIC optionally prepends a sixteen-byte transport header:
 * type 0x8001, one's-complement header checksum, wire length and sequence.
 * This wrapper is independent of the CDC payload and its transaction ID. */
/* Native completeInterruptRead validates channel 1 in the HSIC header
 * (808995b2), using its own sequence (808a66a8), not the CDC reply sequence. */
static unsigned bcm_notification(BCMHSICState *s, uint8_t *notification)
{
    if (!s->control_prefixed) {
        memset(notification, 0, 8);
        notification[0] = 1;
        return 8;
    }
    bcm_make_header(notification, 1, s->interrupt_seq, 16);
    return 16;
}

static int bcm_cdc_control(BCMHSICState *s, int request, int value, int index,
                           int length, uint8_t *data)
{
    if (!s->running || value || index || length < 0) {
        return -1;
    }
    if (request == 0x2100) { /* SEND_ENCAPSULATED_COMMAND */
        if (length < CDC_HDRLEN_STATUS || length > CDC_MAX_PAYLOAD + 32 ||
            !g_queue_is_empty(s->control_rx)) {
            return -1;
        }
        unsigned prefix = lduw_le_p(data) == 0x8001 ? 16 : 0;
        if (prefix && (length < 32 || lduw_le_p(data + 4) != length ||
                       bcm_header_sum(data) != 0xffff)) {
            return -1;
        }
        s->control_prefixed = prefix;
        if (prefix) {
            memcpy(s->control_prefix, data, prefix);
        }
        /* USB GET carries its input, not a padded output-sized buffer. */
        bcm_fullmac_cdc(&s->fullmac, data + prefix, length - prefix,
                        CDC_HDRLEN_STATUS);
        return length;
    }
    /* Apple 11A465 uses bRequest 0 for both directions. Linux brcmfmac
     * uses 1 for GET_ENCAPSULATED_RESPONSE; accept that convention too. */
    if (request == 0xa100 || request == 0xa101) {
        GBytes *reply = g_queue_pop_head(s->control_rx);
        if (!reply) {
            return 0;
        }
        gsize size;
        const uint8_t *bytes = g_bytes_get_data(reply, &size);
        unsigned prefix = s->control_prefixed ? 16 : 0;
        unsigned copied = MIN((gsize)length, size + prefix);
        if (prefix) {
            uint8_t header[16];
            memcpy(header, s->control_prefix, sizeof(header));
            stw_le_p(header + 2, 0);
            stw_le_p(header + 4, copied);
            stw_le_p(header + 2, ~bcm_header_sum(header));
            memcpy(data, header, MIN(copied, prefix));
        }
        if (copied > prefix) {
            memcpy(data + prefix, bytes, copied - prefix);
        }
        g_bytes_unref(reply);
        s->control_notified = false;
        return copied;
    }
    return -1;
}

/* Native bulk reads use the same HSIC header validator (8089bc04),
 * with channel 2 and an independent sequence. Strip it before shared BDC. */
static bool bcm_bulk_input(BCMHSICState *s, const uint8_t *data, size_t size)
{
    if (s->control_prefixed) {
        if (size < 16 || lduw_le_p(data) != 0xa001 ||
            bcm_header_sum(data) != 0xffff) {
            return false;
        }
        uint16_t length = lduw_le_p(data + 4);
        if (length < 16 || length > size ||
            lduw_le_p(data + 6) != s->bulk_tx_seq) {
            return false;
        }
        s->bulk_tx_seq++;
        data += 16;
        size = length - 16;
    }
    bcm_fullmac_tx_data(&s->fullmac, data, size);
    return true;
}

static void bcm_control(USBDevice *dev, USBPacket *p, int request, int value,
                        int index, int length, uint8_t *data)
{
    BCMHSICState *s = BCM_HSIC(dev);
    int ret =
        usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (s->log_count < 256) {
        s->log_count++;
        qemu_log_mask(
            LOG_UNIMP,
            "BCM HSIC control request=%04x value=%04x index=%04x len=%d standard=%d\n",
            request, value, index, length, ret);
    }
    if (ret < 0) {
        ret = bcm_cdc_control(s, request, value, index, length, data);
        if (ret < 0) {
            ret = bcm_vendor_control(s, request, value, index, length, data);
        }
        if (ret >= 0) {
            if (s->running) {
                s->fullmac.dongle_started = true;
            }
            p->actual_length = ret;
            return;
        }
        p->status = USB_RET_STALL;
    }
}
static bool bcm_bulk_read(BCMHSICState *s, USBPacket *p)
{
    GBytes *frame = g_queue_peek_head(s->data_rx);
    if (!frame) {
        return false;
    }
    gsize len;
    const uint8_t *data = g_bytes_get_data(frame, &len);
    size_t copied = MIN(len - s->bulk_rx_offset, p->iov.size);
    usb_packet_copy(p, (void *)(data + s->bulk_rx_offset), copied);
    s->bulk_rx_offset += copied;
    if (s->bulk_rx_offset == len) {
        g_queue_pop_head(s->data_rx);
        g_bytes_unref(frame);
        s->bulk_rx_offset = 0;
        if (s->fullmac.nic) {
            qemu_flush_queued_packets(qemu_get_queue(s->fullmac.nic));
        }
    }
    return true;
}

static void bcm_data(USBDevice *dev, USBPacket *p)
{
    BCMHSICState *s = BCM_HSIC(dev);
    if (s->log_count < 256) {
        s->log_count++;
        qemu_log_mask(LOG_UNIMP, "BCM HSIC data pid=%d ep=%d len=%zu\n", p->pid,
                      p->ep->nr, p->iov.size);
    }
    if (!s->running &&
        (s->dl_state <= BCM_DL_READY || s->dl_state == BCM_DL_RUNNABLE) &&
        p->pid == USB_TOKEN_OUT && p->ep->nr == 3) {
        if (p->iov.size > BCM_DL_MAX) {
            s->dl_state = BCM_DL_IMAGE_TOOBIG;
            p->status = USB_RET_STALL;
            return;
        }
        g_autofree uint8_t *data = g_malloc(p->iov.size);
        qemu_iovec_to_buf(&p->iov, 0, data, p->iov.size);
        if (bcm_download_data(s, data, p->iov.size)) {
            p->actual_length = p->iov.size;
        } else {
            p->status = USB_RET_STALL;
        }
        return;
    }
    if (s->running && p->pid == USB_TOKEN_IN && p->ep->nr == 1 &&
        !s->control_notified && !g_queue_is_empty(s->control_rx)) {
        uint8_t notification[16];
        unsigned size = bcm_notification(s, notification);
        usb_packet_copy(p, notification, MIN(p->iov.size, size));
        s->interrupt_seq++;
        s->control_notified = true;
        return;
    }
    if (s->running && p->pid == USB_TOKEN_IN && p->ep->nr == 2 &&
        bcm_bulk_read(s, p)) {
        return;
    }
    if (s->running && p->pid == USB_TOKEN_OUT && p->ep->nr == 3) {
        if (p->iov.size > 2048) {
            p->status = USB_RET_STALL;
            return;
        }
        uint8_t data[2048];
        usb_packet_copy(p, data, p->iov.size);
        if (!bcm_bulk_input(s, data, p->iov.size)) {
            if (s->bulk_error_log++ < 8) {
                qemu_log_mask(
                    LOG_GUEST_ERROR,
                    "BCM HSIC invalid bulk header len=%zu expected-seq=%u\n",
                    p->iov.size, s->bulk_tx_seq);
                if (p->iov.size >= 16) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "BCM HSIC header %08x %08x %08x %08x\n",
                                  ldl_le_p(data), ldl_le_p(data + 4),
                                  ldl_le_p(data + 8), ldl_le_p(data + 12));
                }
            }
            p->status = USB_RET_STALL;
        }
        return;
    }
    p->status = USB_RET_NAK;
}
/* A module's physical reset differs from a USB link reset: firmware and
 * protocol queues are lost, and the HSIC device electrically disconnects. */
static void bcm_power_reset(void *opaque, int pin, int level)
{
    BCMHSICState *s = BCM_HSIC(opaque);
    USBDevice *dev = USB_DEVICE(s);
    bool asserted = !!level;

    if (s->reset_asserted == asserted) {
        return;
    }
    s->reset_asserted = asserted;
    if (asserted) {
        if (dev->attached) {
            usb_device_detach(dev);
        }
        bcm_download_reset(s);
        bcm_fullmac_reset(&s->fullmac);
        g_queue_clear_full(s->control_rx, (GDestroyNotify)g_bytes_unref);
        g_queue_clear_full(s->data_rx, (GDestroyNotify)g_bytes_unref);
        s->control_notified = s->control_prefixed = false;
        s->interrupt_seq = s->bulk_rx_seq = s->bulk_tx_seq = 0;
        s->bulk_rx_offset = 0;
    } else if (!dev->attached) {
        usb_device_attach(dev, &error_abort);
    }
}

static void bcm_realize(USBDevice *dev, Error **errp)
{
    BCMHSICState *s = BCM_HSIC(dev);
    s->control_rx = g_queue_new();
    s->data_rx = g_queue_new();
    bcm_fullmac_init(&s->fullmac, bcm_fullmac_output, bcm_fullmac_ready, s);
    qemu_macaddr_default_if_unset(&s->fullmac.conf.macaddr);
    memcpy(s->fullmac.mac, s->fullmac.conf.macaddr.a, sizeof(s->fullmac.mac));
    s->fullmac.fws_supported = true;
    s->fullmac.fw_version = "wl0: version 6.10 (qemu-ios virtual FullMAC)";
    bcm_fullmac_setup_net(&s->fullmac, DEVICE(dev), TYPE_BCM_HSIC, "wifi0");
    usb_desc_init(dev);
    dev->auto_attach = 1;
    qdev_init_gpio_in_named(DEVICE(dev), bcm_power_reset, "reset", 1);
}
static void bcm_reset(USBDevice *dev)
{
    /* Running firmware survives a link reset. An unfinished ROM transfer
     * restarts at the header so the native loader can retry without DL_START. */
    BCMHSICState *s = BCM_HSIC(dev);
    s->log_count = 0;
    if (!s->running) {
        bcm_download_reset(s);
    }
}
static void bcm_unrealize(USBDevice *dev)
{
    BCMHSICState *s = BCM_HSIC(dev);
    bcm_fullmac_cleanup(&s->fullmac);
    g_queue_free_full(s->control_rx, (GDestroyNotify)g_bytes_unref);
    g_queue_free_full(s->data_rx, (GDestroyNotify)g_bytes_unref);
    g_clear_pointer(&s->download, g_byte_array_unref);
}
static char *bcm_get_boot_status(Object *obj, Error **errp)
{
    BCMHSICState *s = BCM_HSIC(obj);
    return g_strdup_printf(
        "{\"state\":%u,\"received\":%u,\"expected\":%u,\"running\":%s}",
        s->dl_state, s->download ? s->download->len : 0, s->dl_size,
        s->running ? "true" : "false");
}
static const Property bcm_properties[] = {
    DEFINE_PROP_MACADDR("mac", BCMHSICState, fullmac.conf.macaddr),
};

static void bcm_class_init(ObjectClass *klass, const void *data)
{
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);
    device_class_set_props(DEVICE_CLASS(klass), bcm_properties);
    uc->product_desc = "Broadcom BCM4334 HSIC qualification fixture";
    uc->usb_desc = &bcm_desc;
    uc->realize = bcm_realize;
    uc->unrealize = bcm_unrealize;
    /* usb_desc_init starts at full speed; this HSIC device is high-speed
     * only. Select its descriptor after the host negotiates the port speed,
     * as the other descriptor-backed USB devices do. */
    uc->handle_attach = usb_desc_attach;
    uc->handle_reset = bcm_reset;
    uc->handle_control = bcm_control;
    uc->handle_data = bcm_data;
    object_class_property_add_str(klass, "boot-status", bcm_get_boot_status,
                                  NULL);
}
static const TypeInfo bcm_type = {
    .name = TYPE_BCM_HSIC,
    .parent = TYPE_USB_DEVICE,
    .instance_size = sizeof(BCMHSICState),
    .class_init = bcm_class_init,
};
static void bcm_register_types(void)
{
    type_register_static(&bcm_type);
}
type_init(bcm_register_types)
