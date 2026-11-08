/* Native ETL ping framing, bounded buffering, deferred UART delivery/reset.
 * SLICE hw/usb/dev-mdm9x15.c define MDM_SERIAL_MAX_FRAME|MDM_INTERFACES|MDM_ROUTER_CHANNELS
 * SLICE hw/usb/dev-mdm9x15.c typedef MdmSerial
 * SLICE hw/usb/dev-mdm9x15.c fn mdm_serial_trace mdm_dload_crc mdm_hdlc_encode mdm_serial_wire_out mdm_serial_router_defaults mdm_serial_qmi_routed mdm_serial_router_sequence mdm_serial_h5_payload mdm_serial_frame mdm_serial_receive_hdlc mdm_serial_receive mdm_serial_flush mdm_serial_reset
 * PKG glib-2.0
 */
#include <glib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define TRACE_USB_MDM9X15_UART 0
#define trace_event_get_state_backends(x) false
#define trace_usb_mdm9x15_uart(...) ((void)0)
#define trace_usb_mdm9x15_serial_reset(...) ((void)0)
typedef struct {
    bool pending;
    int64_t deadline;
} QEMUTimer;
typedef struct {
    unsigned room;
    GByteArray *received;
} Chardev;
typedef struct MdmSerial MdmSerial;
typedef struct {
    bool runtime;
} MdmDownloadState;
typedef struct {
    bool h5_userspace;
    uint8_t h5_data_type;
    void (*h5_wire_out)(void *, const uint8_t *, size_t);
    void (*h5_payload_out)(void *, const uint8_t *, size_t);
    void *h5_opaque;
    unsigned h5_nunacked;
} IosBbCore;
typedef struct {
    IosBbCore bb;
} IosBasebandState;
typedef struct {
    bool ready;
    MdmDownloadState *download;
    MdmSerial *serial;
    IosBasebandState *baseband;
} MDM9x15State;
static unsigned h5_received;
static void mdm_qmux_receive(MDM9x15State *s, GByteArray *a, unsigned iface,
                             const uint8_t *p, size_t n)
{
    assert(iface == 13);
    g_byte_array_append(a, p, n);
}
static void ios_bb_reset(IosBbCore *bb)
{
    bb->h5_nunacked = 0;
}
static void ios_bb_h5_restart(IosBbCore *bb)
{
    bb->h5_nunacked = 0;
}
static void ios_bb_h5_input(IosBbCore *bb, const uint8_t *buf, size_t len)
{
    h5_received += len;
}
static void ios_bb_h5_tick(IosBbCore *bb, int64_t now)
{
}
static bool ios_bb_h5_send(IosBbCore *bb, const uint8_t *buf, size_t len)
{
    bb->h5_wire_out(bb->h5_opaque, buf, len);
    return true;
}
static void mdm_serial_receive_hdlc(MDM9x15State *, const uint8_t *, size_t);
#define QEMU_CLOCK_VIRTUAL 0
static int64_t test_time = 100;
static int64_t qemu_clock_get_ms(int clock)
{
    return test_time;
}
static void timer_mod(QEMUTimer *timer, int64_t deadline)
{
    timer->pending = true;
    timer->deadline = deadline;
}
static void timer_mod_anticipate(QEMUTimer *timer, int64_t deadline)
{
    if (!timer->pending || deadline < timer->deadline)
        timer_mod(timer, deadline);
}
static void timer_del(QEMUTimer *timer)
{
    timer->pending = false;
}
static int qemu_chr_be_can_write(Chardev *chr)
{
    return chr->room;
}
static void qemu_chr_be_write(Chardev *chr, const uint8_t *buf, unsigned count)
{
    assert(count <= chr->room);
    chr->room -= count;
    g_byte_array_append(chr->received, buf, count);
}
static uint16_t lduw_le_p(const void *ptr)
{
    const uint8_t *p = ptr;
    return p[0] | p[1] << 8;
}
static uint32_t ldl_le_p(const void *ptr)
{
    const uint8_t *p = ptr;
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
static void mdm_log(MDM9x15State *s, const char *name, unsigned a, unsigned b,
                    unsigned c)
{
}
#include "slice.h"

int main(void)
{
    QEMUTimer timer = {0};
    Chardev chr = {.room = 1, .received = g_byte_array_new()};
    MdmSerial serial = {.chr = &chr,
                        .timer = &timer,
                        .rx = g_byte_array_new(),
                        .tx = g_byte_array_new(),
                        .qmux = g_byte_array_new()};
    MdmDownloadState download = {.runtime = true};
    IosBasebandState baseband = {0};
    MDM9x15State s = {.ready = true,
                      .download = &download,
                      .serial = &serial,
                      .baseband = &baseband};
    mdm_serial_router_defaults(&serial);
    /* Original captured native sequence0 "initial" and CRC3154. */
    const uint8_t captured[] = {0x7e, 0x7b, 0,   0,   0, 0,    'i',  'n', 'i',
                                't',  'i',  'a', 'l', 0, 0x31, 0x54, 0x7e};
    mdm_serial_receive(&s, captured, 3);
    assert(!serial.tx->len && !timer.pending);
    mdm_serial_receive(&s, captured + 3, sizeof(captured) - 3);
    assert(serial.tx->len == sizeof(captured) && timer.pending);
    assert(timer.deadline == 102 && !chr.received->len);
    /* Another native request before delivery must not postpone the reply. */
    test_time = 101;
    mdm_serial_receive(&s, captured, sizeof(captured));
    assert(timer.deadline == 102 && serial.tx->len == 2 * sizeof(captured));
    mdm_serial_flush(&s); /* One-byte room; the rest must survive. */
    assert(chr.received->len == 1 &&
           serial.tx->len == 2 * sizeof(captured) - 1);
    chr.room = 100;
    mdm_serial_flush(&s);
    assert(!serial.tx->len && chr.received->len == 2 * sizeof(captured));
    assert(!memcmp(chr.received->data, captured, sizeof(captured)));
    assert(!memcmp(chr.received->data + sizeof(captured), captured,
                   sizeof(captured)));

    uint8_t corrupt[sizeof(captured)];
    memcpy(corrupt, captured, sizeof(captured));
    corrupt[4] ^= 1;
    mdm_serial_receive(&s, corrupt, sizeof(corrupt));
    assert(!serial.tx->len);
    const uint8_t escaped[] = {0x7b, 0x7e, 0x7d, 0, 255};
    g_autoptr(GByteArray) wire = mdm_hdlc_encode(escaped, sizeof(escaped));
    for (unsigned i = 0; i < wire->len; i++)
        mdm_serial_receive(&s, wire->data + i, 1);
    assert(serial.tx->len == wire->len &&
           !memcmp(serial.tx->data, wire->data, wire->len));
    mdm_serial_reset(&s);
    assert(!timer.pending && !serial.tx->len && !serial.rx->len);
    s.ready = false;
    mdm_serial_receive(&s, captured, sizeof(captured));
    assert(!serial.tx->len);
    s.ready = true;
    download.runtime = false;
    mdm_serial_receive(&s, captured, sizeof(captured));
    assert(!serial.tx->len);
    download.runtime = true;
    uint8_t junk[5000];
    memset(junk, 0x7b, sizeof(junk));
    mdm_serial_receive(&s, junk, sizeof(junk));
    assert(serial.discard && !serial.rx->len);
    mdm_serial_receive(&s, captured, sizeof(captured));
    assert(serial.tx->len == sizeof(captured));
    mdm_serial_reset(&s);
    for (unsigned i = 0; i < 2000; i++)
        mdm_serial_receive(&s, captured, sizeof(captured));
    assert(serial.tx->len <= 4 * MDM_SERIAL_MAX_FRAME);
    mdm_serial_reset(&s);
    const uint8_t invalid_escape[] = {0x7e, 0x7b, 0x7d, 0x7e};
    mdm_serial_receive(&s, invalid_escape, sizeof(invalid_escape));
    assert(!serial.tx->len);
    const uint8_t enable[] = {0x4b, 0xfd, 0x31, 0, 1, 1, 0, 1};
    g_autoptr(GByteArray) config = mdm_hdlc_encode(enable, sizeof(enable));
    uint8_t sync = 0xc0;
    g_byte_array_append(config, &sync, 1);
    mdm_serial_receive(&s, config->data, config->len);
    assert(serial.h5 && baseband.bb.h5_userspace && h5_received == 1);
    assert(!serial.tx->len); /* no fabricated configure ACK */
    baseband.bb.h5_wire_out(baseband.bb.h5_opaque, captured, sizeof(captured));
    assert(serial.tx->len == sizeof(captured));
    g_byte_array_set_size(serial.tx, 0);
    baseband.bb.h5_payload_out(baseband.bb.h5_opaque, captured,
                               sizeof(captured));
    assert(serial.tx->len == sizeof(captured));
    const uint8_t baud[] = {0x4b, 0xfd, 0x31, 0, 5, 0, 0, 0};
    g_autoptr(GByteArray) baud_wire = mdm_hdlc_encode(baud, sizeof(baud));
    baseband.bb.h5_nunacked = 1;
    baseband.bb.h5_payload_out(baseband.bb.h5_opaque, baud_wire->data,
                               baud_wire->len);
    assert(!baseband.bb.h5_nunacked);
    const uint8_t baud7[] = {0x4b, 0xfd, 0x31, 0, 5, 4, 0, 0xc0, 0xc6, 0x2d, 0};
    g_autoptr(GByteArray) baud7_wire = mdm_hdlc_encode(baud7, sizeof(baud7));
    baseband.bb.h5_nunacked = 1;
    baseband.bb.h5_payload_out(baseband.bb.h5_opaque, baud7_wire->data,
                               baud7_wire->len);
    assert(!baseband.bb.h5_nunacked);
    uint8_t malformed_baud7[sizeof(baud7)];
    memcpy(malformed_baud7, baud7, sizeof(baud7));
    malformed_baud7[5] = 3;
    g_autoptr(GByteArray) malformed_baud_wire =
        mdm_hdlc_encode(malformed_baud7, sizeof(malformed_baud7));
    baseband.bb.h5_nunacked = 1;
    baseband.bb.h5_payload_out(baseband.bb.h5_opaque, malformed_baud_wire->data,
                               malformed_baud_wire->len);
    assert(baseband.bb.h5_nunacked == 1);
    baseband.bb.h5_nunacked = 0;
    /* Legacy10A405 configure passes wait=false to ETLRouterSendRouterSequence.
     * Commit its routes without injecting a response before negotiation. */
    const uint8_t legacy_sequence[] = {0x4b, 0xfd, 0x31, 0, 2, 4,
                                       0,    1,    0,    0, 0};
    g_autoptr(GByteArray) legacy_wire =
        mdm_hdlc_encode(legacy_sequence, sizeof(legacy_sequence));
    g_byte_array_set_size(serial.tx, 0);
    baseband.bb.h5_payload_out(baseband.bb.h5_opaque, legacy_wire->data,
                               legacy_wire->len);
    assert(serial.routes[1] == 0 && !serial.tx->len);
    assert(!serial.router_version_negotiated);
    const uint8_t router_version[] = {0x4b, 0xfd, 0x31, 0, 0xff, 4,
                                      0,    0,    0,    1, 0};
    g_autoptr(GByteArray) version_wire =
        mdm_hdlc_encode(router_version, sizeof(router_version));
    g_byte_array_set_size(serial.tx, 0);
    baseband.bb.h5_payload_out(baseband.bb.h5_opaque, version_wire->data,
                               version_wire->len);
    assert(serial.tx->len == version_wire->len &&
           !memcmp(serial.tx->data, version_wire->data, version_wire->len));
    /* Original11A465 unroutes1/10, then routes9 to UART0 and1 to USB1. */
    const uint8_t sequence[] = {0x4b, 0xfd, 0x31, 0, 3, 2, 0, 1, 0, 3,
                                2,    0,    10,   0, 2, 4, 0, 9, 0, 0,
                                0,    2,    4,    0, 1, 0, 1, 0};
    g_autoptr(GByteArray) sequence_wire =
        mdm_hdlc_encode(sequence, sizeof(sequence));
    g_byte_array_set_size(serial.tx, 0);
    baseband.bb.h5_payload_out(baseband.bb.h5_opaque, sequence_wire->data,
                               sequence_wire->len);
    assert(serial.routes[1] == 1 && serial.routes[9] == 0 &&
           serial.routes[10] == 255);
    const uint8_t sequence_response[] = {0x4b, 0xfd, 0x31, 0};
    g_autoptr(GByteArray) sequence_reply =
        mdm_hdlc_encode(sequence_response, sizeof(sequence_response));
    assert(serial.tx->len == sequence_reply->len &&
           !memcmp(serial.tx->data, sequence_reply->data, sequence_reply->len));
    uint8_t prior[MDM_ROUTER_CHANNELS];
    memcpy(prior, serial.routes, sizeof(prior));
    const uint8_t bad_tail[] = {3, 2, 0, 9, 0, 2, 4, 0, 1, 0, 99, 0};
    assert(!mdm_serial_router_sequence(&serial, bad_tail, sizeof(bad_tail)));
    assert(!memcmp(prior, serial.routes, sizeof(prior)));
    const uint8_t unroute9[] = {3, 2, 0, 9, 0};
    assert(mdm_serial_router_sequence(&serial, unroute9, sizeof(unroute9)));
    assert(!mdm_serial_qmi_routed(&serial));
    const uint8_t blocked_qmux[] = {1, 11, 0, 0, 0, 0, 0, 10, 0x27, 0, 0, 0};
    baseband.bb.h5_payload_out(baseband.bb.h5_opaque, blocked_qmux,
                               sizeof(blocked_qmux));
    assert(!serial.qmux->len);
    assert(mdm_serial_router_sequence(&serial, sequence + 4,
                                      sizeof(sequence) - 4));
    const uint8_t inactivity[] = {0x4b, 0xfd, 0x2c, 0, 20, 0, 0, 0};
    const uint8_t inactivity_reply[] = {0x4b, 0xfd, 0x2c, 0, 1};
    g_autoptr(GByteArray) inactivity_wire =
        mdm_hdlc_encode(inactivity, sizeof(inactivity));
    g_autoptr(GByteArray) reply_wire =
        mdm_hdlc_encode(inactivity_reply, sizeof(inactivity_reply));
    g_byte_array_set_size(serial.tx, 0);
    baseband.bb.h5_payload_out(baseband.bb.h5_opaque, inactivity_wire->data,
                               inactivity_wire->len);
    assert(serial.inactivity_timer == 20);
    assert(serial.tx->len == reply_wire->len &&
           !memcmp(serial.tx->data, reply_wire->data, reply_wire->len));
    const uint8_t qmux[] = {1, 11, 0, 0, 0, 0, 0, 10, 0x27, 0, 0, 0};
    baseband.bb.h5_payload_out(baseband.bb.h5_opaque, qmux, 2);
    baseband.bb.h5_payload_out(baseband.bb.h5_opaque, qmux + 2,
                               sizeof(qmux) - 2);
    assert(serial.qmux->len == sizeof(qmux));
    assert(!memcmp(serial.qmux->data, qmux, sizeof(qmux)));
    mdm_serial_reset(&s);
    assert(!serial.router_version_negotiated);
    assert(!serial.h5 && !serial.tx->len && !serial.inactivity_timer &&
           !serial.qmux->len);
    assert(serial.routes[1] == 0 && serial.routes[9] == 6 &&
           serial.routes[10] == 255);
    g_byte_array_unref(serial.qmux);
    g_byte_array_unref(serial.rx);
    g_byte_array_unref(serial.tx);
    g_byte_array_unref(chr.received);
    puts("a6 modem UART ping/CRC/backpressure/power/reset PASS");
}
