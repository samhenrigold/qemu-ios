/*
 * Fake cellular baseband for the emulated iPhones: the QEMU device.
 *
 * The M68's baseband lives on UART1 (the DeviceTree's /arm-io/baseband node); the
 * kernel wraps it in AppleReliableSerialLayer, so the guest only ever sees the
 * byte stream this model answers (see docs/baseband/commcenter-1.0.md). The device
 * owns the transport-independent core (ios_baseband_core.c) and rides it on a
 * chardev that the board hands to the UART as its backend:
 *
 *      dev = qdev_new("ios-baseband");
 *      ... set carrier / mcc-mnc / signal-dbm / registered / sim-present ...
 *      qdev_realize(dev, NULL, &error_fatal);
 *      qdev_prop_set_chr(uart, "chardev", ios_baseband_chardev(dev));
 *
 * The Mac app drives it over QMP (qom-set on the device's properties): carrier,
 * mcc-mnc, signal-dbm, registered, sim-present, battery-percent (wired by the
 * board from the PMU), voicemail, imei/imsi/iccid, answer-delay-ms; the actions
 * incoming-call, remote-answer, remote-hangup and incoming-sms ("<num>|<text>");
 * the observables call-state, last-dialed and last-mo-sms.
 *
 * The 3GS and iPhone 4 put the same modem behind SPI2 instead (BasebandSPI's IFX
 * framing, docs/baseband/commcenter-4.2.1-3gs.md): with ifx-version 1/2 and
 * ifx-max-data set from the DT spi2 node there is no chardev; the board's
 * baseband SPI controller calls ios_baseband_spi_xfer() per frame and wires the
 * "mrdy" GPIO in and "srdy" GPIO out.
 *
 * Replies leave from a timer, never inside chr_write, for the same two reasons the
 * Bluetooth HCI chardev has: re-entering the UART model mid-write, and beating the
 * guest to its own receive setup. It also gives the model a millisecond to be a
 * baseband.
 */
#include "qemu/osdep.h"
#include "chardev/char.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/qdev-core.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "system/reset.h"
#include "migration/vmstate.h"
#include "hw/misc/ios_baseband.h"
#include "hw/misc/ios_baseband_core.h"

#define TYPE_CHARDEV_IOS_BB "chardev-ios-baseband"

/* Latency between the core producing bytes and the UART seeing them. */
#define IOS_BB_LATENCY_MS 2

struct IosBbChardev {
    Chardev parent;

    IosBasebandState *dev;
};
typedef struct IosBbChardev IosBbChardev;

DECLARE_INSTANCE_CHECKER(IosBbChardev, IOS_BB_CHARDEV, TYPE_CHARDEV_IOS_BB)

static void iosbb_arm(IosBasebandState *s, int64_t at_ms);

/* The core calls this synchronously; bytes go out from the timer. */
static void iosbb_out(void *opaque, const uint8_t *buf, size_t len)
{
    IosBasebandState *s = opaque;

    if (s->out_len + len > sizeof(s->out)) {
        fprintf(stderr, "ios-bb: output backlog overflow, dropping %zu bytes\n", len);
        return;
    }
    memcpy(s->out + s->out_len, buf, len);
    s->out_len += len;
    iosbb_arm(s, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + IOS_BB_LATENCY_MS);
}

static void iosbb_flush(IosBasebandState *s)
{
    while (s->out_len) {
        int can = qemu_chr_be_can_write(s->chr);
        int have = s->out_len;

        if (can <= 0) {
            return;                             /* the UART is full; come back later */
        }
        if (can > have) {
            can = have;
        }
        qemu_chr_be_write(s->chr, s->out, can);
        memmove(s->out, s->out + can, have - can);
        s->out_len = have - can;
    }
}

/*
 * SPI handshake (IFX, as BasebandSPI v2 checks it: an SRDY timeout with SRDY already
 * high is an error, so every transfer needs a fresh rising edge). The AP sets the
 * frame up, raises MRDY and waits for SRDY; the frame moves while SRDY is high
 * (the controller's FIFOs only open then); the end of the frame (go cleared, or MRDY
 * falling) drops SRDY again, so the next transfer gets its own edge.
 * With nothing asked of it, SRDY rises on its own when the modem has something
 * to say (or credits to grant), which makes the AP start a transfer.
 */
static void iosbb_set_srdy(IosBasebandState *s, bool level)
{
    if (level != s->srdy_level) {
        if (getenv("IOS_BB_TRACE")) {
            fprintf(stderr, "%.3f ios-bb: SRDY %d\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e6, level);
        }
        s->srdy_level = level;
        qemu_set_irq(s->srdy, level);
        if (level && s->spi_ready) {
            s->spi_ready(s->spi_ready_opaque);
        }
    }
}

static void iosbb_srdy_update(IosBasebandState *s)
{
    if (s->mrdy_level || (!s->srdy_level && ios_bb_ifx_pending(&s->ifx))) {
        iosbb_set_srdy(s, true);
    }
}

static void iosbb_mrdy(void *opaque, int n, int level)
{
    IosBasebandState *s = opaque;

    if (getenv("IOS_BB_TRACE")) {
        fprintf(stderr, "%.3f ios-bb: MRDY %d\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e6, level);
    }
    s->mrdy_level = level;
    /* MRDY up while SRDY is still high from our own request: the AP waits for an
     * edge it will not get otherwise, so give it a fresh one. */
    iosbb_set_srdy(s, false);
    iosbb_arm(s, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + IOS_BB_LATENCY_MS);
}

/* The controller finished a frame (go cleared): SRDY drops, rising again if more is due. */
void ios_baseband_spi_done(DeviceState *dev)
{
    IosBasebandState *s = IOS_BASEBAND(dev);

    iosbb_set_srdy(s, false);
    iosbb_arm(s, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + IOS_BB_LATENCY_MS);
}

bool ios_baseband_spi_srdy(DeviceState *dev)
{
    return IOS_BASEBAND(dev)->srdy_level;
}

void ios_baseband_spi_set_ready(DeviceState *dev, void (*cb)(void *), void *opaque)
{
    IOS_BASEBAND(dev)->spi_ready = cb;
    IOS_BASEBAND(dev)->spi_ready_opaque = opaque;
}

void ios_baseband_spi_xfer(DeviceState *dev, const uint8_t *mosi, uint8_t *miso, size_t n)
{
    IosBasebandState *s = IOS_BASEBAND(dev);
    const uint8_t *rx;
    size_t rxlen;

    s->bb.now_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    ios_bb_ifx_xfer(&s->ifx, mosi, miso, n, &rx, &rxlen);
    if (rxlen) {
        ios_bb_input(&s->bb, rx, rxlen);
    }
    iosbb_arm(s, s->bb.now_ms + IOS_BB_LATENCY_MS);
}

static void iosbb_tick_timer(void *opaque)
{
    IosBasebandState *s = opaque;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    int64_t due;

    ios_bb_tick(&s->bb, now);
    if (s->ifx_version) {
        iosbb_srdy_update(s);
    } else {
        iosbb_flush(s);
    }

    due = ios_bb_next_due(&s->bb);
    if (s->out_len) {
        if (!due || now + IOS_BB_LATENCY_MS < due) {
            due = now + IOS_BB_LATENCY_MS;
        }
    }
    if (due) {
        iosbb_arm(s, due);
    }
}

static void iosbb_arm(IosBasebandState *s, int64_t at_ms)
{
    if (s->timer) {
        timer_mod(s->timer, at_ms);            /* timer_new_ms: ms units */
    }
}

/* ------------------------------------------------------------- chardev (UART peer) */

static int iosbb_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    IosBasebandState *s = IOS_BB_CHARDEV(chr)->dev;

    s->bb.now_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    ios_bb_input(&s->bb, buf, len);
    /* Replies come from the timer (see the file comment). */
    iosbb_arm(s, s->bb.now_ms + IOS_BB_LATENCY_MS);
    return len;
}

static void iosbb_chr_accept_input(Chardev *chr)
{
    IosBasebandState *s = IOS_BB_CHARDEV(chr)->dev;

    iosbb_arm(s, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + IOS_BB_LATENCY_MS);
}

static void iosbb_chr_open(Chardev *chr, ChardevBackend *backend,
                           bool *be_opened, Error **errp)
{
    *be_opened = true;
}

static void iosbb_chr_class_init(ObjectClass *oc, void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->open = iosbb_chr_open;
    cc->chr_write = iosbb_chr_write;
    cc->chr_accept_input = iosbb_chr_accept_input;
}

static const TypeInfo iosbb_chr_type_info = {
    .name = TYPE_CHARDEV_IOS_BB,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(IosBbChardev),
    .class_init = iosbb_chr_class_init,
};

/* -------------------------------------------------------------- cellular data */

/*
 * The PDP context is raw IPv4; slirp speaks Ethernet. Wrap each packet in a frame
 * from our MAC to slirp's gateway MAC, and answer slirp's ARP for the guest's
 * address so it can address frames back. Only "cell0" is looked up: the board
 * creates this device, so there is no netdev property on the command line.
 */
static const uint8_t iosbb_gw_mac[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static void iosbb_data_out(void *opaque, const uint8_t *buf, size_t len)
{
    IosBasebandState *s = opaque;
    uint8_t f[14 + 2048];

    if (!s->nic || len > sizeof(f) - 14) {
        return;
    }
    memcpy(f, iosbb_gw_mac, 6);
    memcpy(f + 6, s->conf.macaddr.a, 6);
    f[12] = 0x08;
    f[13] = 0x00;
    memcpy(f + 14, buf, len);
    qemu_send_packet(qemu_get_queue(s->nic), f, 14 + len);
}

static ssize_t iosbb_net_receive(NetClientState *nc, const uint8_t *buf, size_t len)
{
    IosBasebandState *s = qemu_get_nic_opaque(nc);
    static const uint8_t guest_ip[4] = { 10, 0, 2, 15 };

    if (len < 14) {
        return len;
    }
    if (buf[12] == 0x08 && buf[13] == 0x00) {
        ios_bb_data_input(&s->bb, buf + 14, len - 14);
        iosbb_arm(s, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + IOS_BB_LATENCY_MS);
    } else if (buf[12] == 0x08 && buf[13] == 0x06 && len >= 42 && buf[21] == 1 &&
               memcmp(buf + 38, guest_ip, 4) == 0) {
        uint8_t r[42];

        memcpy(r, buf + 6, 6);                  /* to the asker */
        memcpy(r + 6, s->conf.macaddr.a, 6);
        memcpy(r + 12, buf + 12, 8);            /* ethertype, htype..plen */
        r[20] = 0;
        r[21] = 2;                              /* reply */
        memcpy(r + 22, s->conf.macaddr.a, 6);
        memcpy(r + 28, guest_ip, 4);
        memcpy(r + 32, buf + 22, 10);           /* asker's MAC + IP */
        qemu_send_packet(qemu_get_queue(s->nic), r, sizeof(r));
    }
    return len;
}

static NetClientInfo iosbb_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = iosbb_net_receive,
};

/* ------------------------------------------------------------------ QOM properties */

#define STR_PROP(name, field)                                               \
    static char *iosbb_get_##name(Object *obj, Error **errp)                \
    {                                                                       \
        return g_strdup(IOS_BASEBAND(obj)->bb.field);                       \
    }                                                                       \
    static void iosbb_set_##name(Object *obj, const char *value, Error **errp)\
    {                                                                       \
        snprintf(IOS_BASEBAND(obj)->bb.field,                               \
                 sizeof(IOS_BASEBAND(obj)->bb.field), "%s", value ? : "");  \
        ios_bb_changed(&IOS_BASEBAND(obj)->bb);                             \
    }

STR_PROP(carrier, operator_long)
STR_PROP(mcc_mnc, plmn)
STR_PROP(voicemail, voicemail)
STR_PROP(imei, imei)
STR_PROP(imsi, imsi)
STR_PROP(iccid, iccid)

/*
 * Integer properties with a "the model state changed" hook. QOM here only
 * offers pointer-based uint adders; the model wants signed values (dBm) and
 * a push of what the guest would see, so this is a plain Visitor property.
 */
static void iosbb_prop_get_int(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    int64_t val = *(int *)opaque;

    visit_type_int(v, name, &val, errp);
}

static void iosbb_prop_set_int(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    int64_t val = 0;

    if (!visit_type_int(v, name, &val, errp)) {
        return;
    }
    *(int *)opaque = val;
    ios_bb_changed(&IOS_BASEBAND(obj)->bb);
}

static void iosbb_add_int_prop(Object *obj, const char *name, int *field)
{
    object_property_add(obj, name, "int", iosbb_prop_get_int, iosbb_prop_set_int,
                        NULL, field);
}

#define INT_PROP(fn, name, field)                                            \
    static void iosbb_add_##fn(Object *obj)                                 \
    {                                                                       \
        iosbb_add_int_prop(obj, name, &IOS_BASEBAND(obj)->bb.field);        \
    }

INT_PROP(signal_dbm, "signal-dbm", signal_dbm)
INT_PROP(battery_percent, "battery-percent", battery)
INT_PROP(answer_delay_ms, "answer-delay-ms", answer_delay_ms)

#define BOOL_PROP(name, field)                                              \
    static bool iosbb_get_##name(Object *obj, Error **errp)                \
    {                                                                       \
        return IOS_BASEBAND(obj)->bb.field;                                 \
    }                                                                       \
    static void iosbb_set_##name(Object *obj, bool value, Error **errp)     \
    {                                                                       \
        IOS_BASEBAND(obj)->bb.field = value;                                \
        ios_bb_changed(&IOS_BASEBAND(obj)->bb);                             \
    }

BOOL_PROP(registered, registered)
BOOL_PROP(sim_present, sim_present)

/* Actions: writing the property performs the network-side event. */
static void iosbb_set_incoming_call(Object *obj, const char *value, Error **errp)
{
    IosBasebandState *s = IOS_BASEBAND(obj);

    if (!value || !*value) {
        error_setg(errp, "incoming-call wants a phone number");
        return;
    }
    if (!ios_bb_incoming_call(&s->bb, value)) {
        error_setg(errp, "the modem is not in a state to ring "
                         "(registered, radio on, channel open?)");
        return;
    }
    iosbb_arm(s, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + IOS_BB_LATENCY_MS);
}

static void iosbb_set_remote_answer(Object *obj, const char *value, Error **errp)
{
    IosBasebandState *s = IOS_BASEBAND(obj);

    ios_bb_remote_answer(&s->bb);
    iosbb_arm(s, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + IOS_BB_LATENCY_MS);
}

static void iosbb_set_remote_hangup(Object *obj, const char *value, Error **errp)
{
    IosBasebandState *s = IOS_BASEBAND(obj);

    ios_bb_remote_hangup(&s->bb);
    iosbb_arm(s, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + IOS_BB_LATENCY_MS);
}

static void iosbb_set_incoming_sms(Object *obj, const char *value, Error **errp)
{
    IosBasebandState *s = IOS_BASEBAND(obj);
    const char *bar = value ? strchr(value, '|') : NULL;
    char num[32];
    int n;

    if (!bar || bar == value || !bar[1]) {
        error_setg(errp, "incoming-sms wants \"<number>|<text>\"");
        return;
    }
    n = MIN(bar - value, (int)sizeof(num) - 1);
    memcpy(num, value, n);
    num[n] = 0;
    if (!ios_bb_incoming_sms(&s->bb, num, bar + 1)) {
        error_setg(errp, "the modem is not in a state to receive SMS");
        return;
    }
    iosbb_arm(s, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + IOS_BB_LATENCY_MS);
}

/* Observables. */
static char *iosbb_get_call_state(Object *obj, Error **errp)
{
    return g_strdup(ios_bb_call_state(&IOS_BASEBAND(obj)->bb));
}

static char *iosbb_get_last_dialed(Object *obj, Error **errp)
{
    return g_strdup(IOS_BASEBAND(obj)->bb.last_dialed);
}

static char *iosbb_get_last_mo_sms(Object *obj, Error **errp)
{
    IosBbCore *bb = &IOS_BASEBAND(obj)->bb;

    return g_strdup_printf("%s|%s", ios_bb_last_mo_sms_number(bb),
                           ios_bb_last_mo_sms_text(bb));
}

/* ------------------------------------------------------------------------ vmstate */

/*
 * The core keeps its strings as char[]; the VMSTATE_*_ARRAY macros type-check
 * against uint8_t, so byte-array fields on char[] go through this one.
 */
#define VMSTATE_CHAR_ARRAY(_f, _s, _n) {                                    \
    .name = (stringify(_f)),                                                \
    .version_id = 0,                                                        \
    .num = (_n),                                                            \
    .info = &vmstate_info_uint8,                                            \
    .size = 1,                                                              \
    .flags = VMS_ARRAY,                                                     \
    .offset = offsetof(_s, _f),                                             \
}

static const VMStateDescription vmstate_ios_bb_call = {
    .name = "ios-baseband/call",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(used, IosBbCall),
        VMSTATE_BOOL(mt, IosBbCall),
        VMSTATE_INT32(id, IosBbCall),
        VMSTATE_INT32(stat, IosBbCall),
        VMSTATE_INT32(next_stat, IosBbCall),
        VMSTATE_INT64(due_ms, IosBbCall),
        VMSTATE_INT32(rings, IosBbCall),
        VMSTATE_CHAR_ARRAY(number, IosBbCall, 32),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_ios_bb_chan = {
    .name = "ios-baseband/chan",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_CHAR_ARRAY(line, IosBbAtChan, 600),
        VMSTATE_UINT32(len, IosBbAtChan),
        VMSTATE_BOOL(echo, IosBbAtChan),
        VMSTATE_BOOL(open, IosBbAtChan),
        VMSTATE_BOOL(sms_prompt, IosBbAtChan),
        VMSTATE_INT32_V(data_cid, IosBbAtChan, 2),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_ios_bb_h5_pkt = {
    .name = "ios-baseband/h5pkt",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(seq, IosBbH5Pkt),
        VMSTATE_UINT16(len, IosBbH5Pkt),
        VMSTATE_UINT8_ARRAY(data, IosBbH5Pkt, 1024),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_ios_bb_muxrx = {
    .name = "ios-baseband/muxrx",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(state, IosBbMuxRx),
        VMSTATE_UINT32(flags_run, IosBbMuxRx),
        VMSTATE_UINT8(addr, IosBbMuxRx),
        VMSTATE_UINT8(ctrl, IosBbMuxRx),
        VMSTATE_UINT32(len, IosBbMuxRx),
        VMSTATE_UINT32(cnt, IosBbMuxRx),
        VMSTATE_UINT8_ARRAY(buf, IosBbMuxRx, 1600),
        VMSTATE_UINT8(fcs, IosBbMuxRx),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_ios_bb_sms = {
    .name = "ios-baseband/sms",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(used, IosBbSms),
        VMSTATE_CHAR_ARRAY(num, IosBbSms, 32),
        VMSTATE_CHAR_ARRAY(pdu, IosBbSms, 400),
        VMSTATE_END_OF_LIST()
    }
};

static bool iosbb_spi_needed(void *opaque)
{
    return IOS_BASEBAND(opaque)->ifx_version != 0;
}

static const VMStateDescription vmstate_ios_baseband_spi = {
    .name = "ios-baseband/spi",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = iosbb_spi_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(ifx.credits_out, IosBasebandState),
        VMSTATE_UINT8_ARRAY(ifx.txq, IosBasebandState, 16384),
        VMSTATE_UINT32(ifx.txq_len, IosBasebandState),
        VMSTATE_BOOL(srdy_level, IosBasebandState),
        VMSTATE_BOOL(mrdy_level, IosBasebandState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_ios_baseband = {
    .name = "ios-baseband",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        /* Controls: re-set by the board at realize, but kept so a restored
         * device agrees with what QMP last set. */
        VMSTATE_CHAR_ARRAY(bb.operator_long, IosBasebandState, 33),
        VMSTATE_CHAR_ARRAY(bb.operator_short, IosBasebandState, 17),
        VMSTATE_CHAR_ARRAY(bb.plmn, IosBasebandState, 7),
        VMSTATE_CHAR_ARRAY(bb.sca, IosBasebandState, 24),
        VMSTATE_CHAR_ARRAY(bb.voicemail, IosBasebandState, 24),
        VMSTATE_INT32(bb.signal_dbm, IosBasebandState),
        VMSTATE_INT32(bb.battery, IosBasebandState),
        VMSTATE_BOOL(bb.registered, IosBasebandState),
        VMSTATE_BOOL(bb.sim_present, IosBasebandState),
        VMSTATE_UINT32(bb.lac, IosBasebandState),
        VMSTATE_UINT32(bb.ci, IosBasebandState),
        VMSTATE_CHAR_ARRAY(bb.imei, IosBasebandState, 16),
        VMSTATE_CHAR_ARRAY(bb.imsi, IosBasebandState, 16),
        VMSTATE_CHAR_ARRAY(bb.iccid, IosBasebandState, 21),
        VMSTATE_INT32(bb.answer_delay_ms, IosBasebandState),
        /* H5 link. */
        VMSTATE_BOOL(bb.h5, IosBasebandState),
        VMSTATE_BOOL(bb.h5_active, IosBasebandState),
        VMSTATE_BOOL(bb.h5_crc, IosBasebandState),
        VMSTATE_BOOL(bb.h5_in_frame, IosBasebandState),
        VMSTATE_BOOL(bb.h5_esc, IosBasebandState),
        VMSTATE_UINT8_ARRAY(bb.h5_rx, IosBasebandState, 2048),
        VMSTATE_UINT32(bb.h5_rxlen, IosBasebandState),
        VMSTATE_UINT8(bb.h5_tx_seq, IosBasebandState),
        VMSTATE_UINT8(bb.h5_rx_next, IosBasebandState),
        VMSTATE_BOOL(bb.h5_need_ack, IosBasebandState),
        VMSTATE_STRUCT_ARRAY(bb.h5_unacked, IosBasebandState, IOS_BB_H5_WINDOW, 1,
                             vmstate_ios_bb_h5_pkt, IosBbH5Pkt),
        VMSTATE_UINT32(bb.h5_nunacked, IosBasebandState),
        VMSTATE_INT64(bb.h5_last_tx_ms, IosBasebandState),
        VMSTATE_UINT8_ARRAY(bb.h5_txq, IosBasebandState, 8192),
        VMSTATE_UINT32(bb.h5_txq_len, IosBasebandState),
        /* Mux. */
        VMSTATE_BOOL(bb.mux, IosBasebandState),
        VMSTATE_BOOL(bb.mux_leave, IosBasebandState),
        VMSTATE_STRUCT(bb.muxrx, IosBasebandState, 1, vmstate_ios_bb_muxrx, IosBbMuxRx),
        VMSTATE_STRUCT_ARRAY(bb.ch, IosBasebandState, IOS_BB_MAX_CH, 1,
                             vmstate_ios_bb_chan, IosBbAtChan),
        /* AT/network state. */
        VMSTATE_BOOL(bb.hex_cs, IosBasebandState),
        VMSTATE_INT32(bb.cfun, IosBasebandState),
        VMSTATE_INT32(bb.cops_format, IosBasebandState),
        VMSTATE_BOOL(bb.cops_detached, IosBasebandState),
        VMSTATE_INT32(bb.creg_n, IosBasebandState),
        VMSTATE_INT32(bb.creg_ch, IosBasebandState),
        VMSTATE_INT32(bb.cgreg_n, IosBasebandState),
        VMSTATE_INT32(bb.cgreg_ch, IosBasebandState),
        VMSTATE_INT32(bb.xciev_ch, IosBasebandState),
        VMSTATE_INT32(bb.xsim_ch, IosBasebandState),
        VMSTATE_INT32(bb.call_ch, IosBasebandState),
        VMSTATE_INT32_V(bb.sms_ch, IosBasebandState, 2),
        VMSTATE_BOOL_V(bb.pdp_active, IosBasebandState, 2),
        VMSTATE_UINT8_ARRAY_V(bb.ip_rx, IosBasebandState, 2048, 2),
        VMSTATE_UINT32_V(bb.ip_rxlen, IosBasebandState, 2),
        VMSTATE_INT32(bb.s0, IosBasebandState),
        VMSTATE_INT32(bb.last_rssi, IosBasebandState),
        VMSTATE_INT32(bb.last_batt, IosBasebandState),
        VMSTATE_BOOL(bb.sim_last, IosBasebandState),
        VMSTATE_INT32(bb.last_creg, IosBasebandState),
        VMSTATE_INT32(bb.reg_step, IosBasebandState),
        VMSTATE_INT64(bb.reg_due_ms, IosBasebandState),
        VMSTATE_INT64(bb.xsim_due_ms, IosBasebandState),
        VMSTATE_INT32(bb.next_call_id, IosBasebandState),
        VMSTATE_STRUCT_ARRAY(bb.calls, IosBasebandState, IOS_BB_MAX_CALLS, 1,
                             vmstate_ios_bb_call, IosBbCall),
        VMSTATE_INT32(bb.ceer_cause, IosBasebandState),
        /* SMS. */
        VMSTATE_INT32(bb.sms_mr, IosBasebandState),
        VMSTATE_CHAR_ARRAY(bb.last_mo_num, IosBasebandState, 32),
        VMSTATE_CHAR_ARRAY(bb.last_mo_text, IosBasebandState, 512),
        VMSTATE_STRUCT_ARRAY(bb.store, IosBasebandState, IOS_BB_SMS_STORE, 1,
                             vmstate_ios_bb_sms, IosBbSms),
        VMSTATE_UINT32(bb.store_next, IosBasebandState),
        VMSTATE_CHAR_ARRAY(bb.last_dialed, IosBasebandState, 32),
        /* Pending bytes to the UART, and the timer that delivers them. */
        VMSTATE_UINT8_ARRAY(out, IosBasebandState, 8192),
        VMSTATE_UINT32(out_len, IosBasebandState),
        VMSTATE_TIMER_PTR(timer, IosBasebandState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_ios_baseband_spi,
        NULL
    }
};

/* ------------------------------------------------------------------ realize/reset */

/*
 * A guest reboot must not carry stale baseband state into the next boot:
 * half-parsed lines, queued replies, an H5 link the new kernel never
 * established. Reset with the machine, like the transport itself.
 */
static void iosbb_machine_reset(void *opaque)
{
    IosBasebandState *s = opaque;

    ios_bb_reset(&s->bb);
    s->out_len = 0;
    timer_del(s->timer);
    if (s->ifx_version) {
        ios_bb_ifx_init(&s->ifx, s->ifx_version, s->ifx_max_data);
        s->srdy_level = false;
        qemu_set_irq(s->srdy, 0);
    }
}

static void iosbb_realize(DeviceState *dev, Error **errp)
{
    IosBasebandState *s = IOS_BASEBAND(dev);

    if (s->ifx_version) {
        if (s->ifx_version > 2 || s->ifx_max_data <= 0) {
            error_setg(errp, "ifx-version must be 1 or 2 with a max-data-size");
            return;
        }
        ios_bb_ifx_init(&s->ifx, s->ifx_version, s->ifx_max_data);
        ios_bb_init(&s->bb, ios_bb_ifx_queue, &s->ifx);
    } else {
        if (!s->chr) {
            s->chr = qemu_chardev_new(NULL, TYPE_CHARDEV_IOS_BB, NULL, NULL,
                                      &error_abort);
            IOS_BB_CHARDEV(s->chr)->dev = s;
        }
        ios_bb_init(&s->bb, iosbb_out, s);
    }
    s->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, iosbb_tick_timer, s);
    /* SMS-DELIVER timestamps in host time (the guest's clock follows it too). */
    s->bb.wall_offset_ms = g_get_real_time() / 1000 - qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    qemu_register_reset(iosbb_machine_reset, s);

    {
        NetClientState *peer = qemu_find_netdev("cell0");

        if (peer) {
            qemu_macaddr_default_if_unset(&s->conf.macaddr);
            s->conf.peers.ncs[0] = peer;
            s->conf.peers.queues = 1;
            s->nic = qemu_new_nic(&iosbb_net_info, &s->conf, TYPE_IOS_BASEBAND,
                                  "cell", &dev->mem_reentrancy_guard, s);
            s->bb.data_out = iosbb_data_out;
            s->bb.data_opaque = s;
        }
    }
}

static void iosbb_unrealize(DeviceState *dev)
{
    IosBasebandState *s = IOS_BASEBAND(dev);

    qemu_unregister_reset(iosbb_machine_reset, s);
    if (s->timer) {
        timer_del(s->timer);
        timer_free(s->timer);
        s->timer = NULL;
    }
}

/*
 * Identity defaults: the 3GPP test network (MCC 001 / MNC 01, a carrier name the
 * Mac app can change), a test IMEI with TAC 00000000, and an IMSI/ICCID in the
 * 001-01 range. Full bars at -59 dBm, battery 100 until the board's PMU says
 * otherwise.
 */
static void iosbb_instance_init(Object *obj)
{
    IosBasebandState *s = IOS_BASEBAND(obj);

    snprintf(s->bb.operator_long, sizeof(s->bb.operator_long), "Test Network");
    snprintf(s->bb.plmn, sizeof(s->bb.plmn), "00101");
    snprintf(s->bb.imei, sizeof(s->bb.imei), "000000001234569");
    snprintf(s->bb.imsi, sizeof(s->bb.imsi), "001010000000001");
    snprintf(s->bb.iccid, sizeof(s->bb.iccid), "89001010000000000001");
    s->bb.signal_dbm = -59;
    s->bb.battery = 100;
    s->bb.registered = true;
    s->bb.sim_present = true;
    s->bb.lac = 0x1abf;
    s->bb.ci = 0x53f1;
    s->bb.answer_delay_ms = -1;

    object_property_add_str(obj, "carrier", iosbb_get_carrier, iosbb_set_carrier);
    object_property_set_description(obj, "carrier",
        "Operator name shown to the guest (+XCOPS, +COPS long format)");
    object_property_add_str(obj, "mcc-mnc", iosbb_get_mcc_mnc, iosbb_set_mcc_mnc);
    object_property_set_description(obj, "mcc-mnc",
        "PLMN digits (MCC+MNC) of the fake home network");
    object_property_add_str(obj, "voicemail", iosbb_get_voicemail,
                            iosbb_set_voicemail);
    object_property_add_str(obj, "imei", iosbb_get_imei, iosbb_set_imei);
    object_property_add_str(obj, "imsi", iosbb_get_imsi, iosbb_set_imsi);
    object_property_add_str(obj, "iccid", iosbb_get_iccid, iosbb_set_iccid);
    iosbb_add_signal_dbm(obj);
    object_property_set_description(obj, "signal-dbm",
        "Reported signal strength in dBm (rssi = (dbm+113)/2, clamped 0..31)");
    iosbb_add_battery_percent(obj);
    object_property_set_description(obj, "battery-percent",
        "Battery capacity reported via +XCIEV (the board wires it from the PMU)");
    iosbb_add_answer_delay_ms(obj);
    object_property_set_description(obj, "answer-delay-ms",
        "Outgoing calls: after alerting, the remote picks up after this many ms "
        "(-1: never, QMP answers instead)");
    object_property_add_bool(obj, "registered", iosbb_get_registered,
                             iosbb_set_registered);
    object_property_set_description(obj, "registered",
        "Whether the fake network registers the phone when the radio is on");
    object_property_add_bool(obj, "sim-present", iosbb_get_sim_present,
                             iosbb_set_sim_present);

    object_property_add_str(obj, "incoming-call", NULL, iosbb_set_incoming_call);
    object_property_set_description(obj, "incoming-call",
        "Write a phone number to ring the phone (+XCALLSTAT 4, +CLIP, RING)");
    object_property_add_str(obj, "remote-answer", NULL,
                            iosbb_set_remote_answer);
    object_property_set_description(obj, "remote-answer",
        "Write anything: the outgoing call's remote party picks up");
    object_property_add_str(obj, "remote-hangup", NULL,
                            iosbb_set_remote_hangup);
    object_property_set_description(obj, "remote-hangup",
        "Write anything: the remote party hangs up (+XCALLSTAT 6, +CEER 16)");
    object_property_add_str(obj, "incoming-sms", NULL, iosbb_set_incoming_sms);
    object_property_set_description(obj, "incoming-sms",
        "Write \"<number>|<text>\": deliver a 23.040 SMS-DELIVER as +CMT");

    qdev_init_gpio_in_named(DEVICE(obj), iosbb_mrdy, "mrdy", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->srdy, "srdy", 1);

    object_property_add_str(obj, "call-state", iosbb_get_call_state, NULL);
    object_property_set_description(obj, "call-state",
        "idle, dialing, alerting, incoming, active or held (first live call)");
    object_property_add_str(obj, "last-dialed", iosbb_get_last_dialed, NULL);
    object_property_add_str(obj, "last-mo-sms", iosbb_get_last_mo_sms, NULL);
}

/* Board data: the DT spi2 node's protocol-version and max-data-size. */
static const Property iosbb_props[] = {
    DEFINE_PROP_INT32("ifx-version", IosBasebandState, ifx_version, 0),
    DEFINE_PROP_INT32("ifx-max-data", IosBasebandState, ifx_max_data, 0),
};

static void iosbb_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, iosbb_props);
    dc->realize = iosbb_realize;
    dc->unrealize = iosbb_unrealize;
    dc->vmsd = &vmstate_ios_baseband;
    dc->desc = "Fake cellular baseband (H5 + 27.010 mux + AT) for the iPhones";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo iosbb_type_info = {
    .name = TYPE_IOS_BASEBAND,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(IosBasebandState),
    .instance_init = iosbb_instance_init,
    .class_init = iosbb_class_init,
};

static void iosbb_register_types(void)
{
    type_register_static(&iosbb_type_info);
    type_register_static(&iosbb_chr_type_info);
}

type_init(iosbb_register_types)

Chardev *ios_baseband_chardev(DeviceState *dev)
{
    return IOS_BASEBAND(dev)->chr;
}
