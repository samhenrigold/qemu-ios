/*
 * Bluetooth HCI controller for the iPod touch 2G, as a chardev on UART1.
 *
 * The N72AP DeviceTree hangs a "bluetooth,n72" node off /arm-io/uart1, so the
 * BCM4325's Bluetooth side is a plain H4 (UART) HCI link. Nothing was on the
 * other end of that UART, so BTServer sent HCI_Reset, got no Command Complete,
 * and retried every 10 s forever. That is not a cosmetic gap: BluetoothManager
 * never reaches "connected", and every one of its blocking entry points then
 * costs its full client-side timeout, ~1 s, on whatever thread called it.
 * SpringBoard's status bar makes two such calls while an app launches, which
 * is what swallowed the app-launch zoom -- see
 * -[SBStatusBarBluetoothView start].
 *
 * This answers the HCI, it does not implement Bluetooth: there is no radio, so
 * inquiry finds nothing and no connection can ever be made. That is the honest
 * model of an iPod touch with nothing paired, and it is all the guest needs to
 * bring its stack up and stop timing out.
 *
 * ponytail: command-complete only, no ACL/SCO data path and no link control.
 * Add those the day something in the guest actually needs to talk to a remote
 * device; every command below is here because the guest was observed sending
 * it (IT_BT_TRACE=1 prints the ones we still answer blind).
 */

#include "qemu/osdep.h"
#include "chardev/char.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/arm/ipod_touch_2g.h"

/* chardev_new() asserts on the prefix; every chardev type carries it. */
#define TYPE_CHARDEV_IT_BT "chardev-ipodtouch-bt-hci"

/* H4 packet indicators. */
#define H4_CMD   0x01
#define H4_ACL   0x02
#define H4_SCO   0x03
#define H4_EVT   0x04

#define EVT_CMD_COMPLETE 0x0e

#define BT_RESP_MAX 1024

struct ItBtChardev {
    Chardev parent;

    uint8_t cmd[260];       /* one H4 command: 3 header + up to 255 payload */
    unsigned cmd_len;

    uint8_t resp[BT_RESP_MAX];
    unsigned resp_head, resp_tail;

    /*
     * Replies go out from a timer, never from inside chr_write. Two reasons,
     * and the second is the one that actually bites:
     *
     * 1. The guest reaches chr_write from its own UTXH store, so answering
     *    synchronously re-enters the UART model mid-write.
     * 2. A real BCM4325 takes milliseconds to answer. Replying in zero guest
     *    time beats the driver to its own receive setup: the bytes were in the
     *    Rx FIFO before it had armed the DMA it reads them with, its Rx
     *    timeout then fired against a channel that had moved nothing, and it
     *    reset the port and started over -- forever.
     *
     * IT_BT_LATENCY_US tunes it; the default is comfortably inside the 10 s
     * the guest allows per HCI_Reset and well clear of its setup path.
     */
    QEMUTimer *timer;
};
typedef struct ItBtChardev ItBtChardev;

DECLARE_INSTANCE_CHECKER(ItBtChardev, IT_BT_CHARDEV, TYPE_CHARDEV_IT_BT)

static int bt_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_BT_TRACE") ? 1 : 0;
    }
    return on;
}

/*
 * Return parameters that follow the status byte, per opcode. A command absent
 * from here is answered with status-only, which is correct for every "write"
 * command and wrong for any "read" the guest cares about -- so unknown reads
 * are traced rather than guessed at silently.
 */
static const uint8_t *bt_ret_params(uint16_t opcode, unsigned *len)
{
    /* Read Local Version: hci ver/rev, lmp ver, manufacturer, lmp subver. */
    static const uint8_t local_version[] = {
        0x05, 0x0f, 0x00, 0x05, 0x0f, 0x00, 0x11, 0x22,
    };
    /* Read Local Supported Commands: 64 bytes. Claim the basic set only. */
    static const uint8_t supported_commands[64] = {
        0xff, 0xff, 0xff, 0x03, 0xce, 0xff, 0xef, 0xff,
        0xff, 0xff, 0xff, 0x7f, 0xf2, 0x0f, 0xe8, 0xfe,
        0x3f, 0xf7, 0x83, 0xff, 0x1c, 0x00, 0x04, 0x00,
        0x61, 0xf7, 0xff, 0xff, 0x7f, 0x00, 0x00, 0x00,
    };
    /* Read Local Supported Features / Extended Features page 0. */
    static const uint8_t features[] = {
        0xff, 0xff, 0x8f, 0xfe, 0xdb, 0xff, 0x5b, 0x87,
    };
    static const uint8_t ext_features[] = {
        0x00, 0x00,
        0xff, 0xff, 0x8f, 0xfe, 0xdb, 0xff, 0x5b, 0x87,
    };
    /* Read Buffer Size: ACL len, SCO len, ACL count, SCO count. */
    static const uint8_t buffer_size[] = {
        0xfd, 0x03, 0x40, 0x08, 0x00, 0x08, 0x00,
    };
    /*
     * Read BD_ADDR. Deliberately in the locally-administered range: this is a
     * model, not a clone of anybody's radio, and the guest hashes the wifi and
     * bluetooth MACs into its UDID (see the device-identity notes), so it must
     * at least be stable across boots.
     */
    static const uint8_t bd_addr[] = { 0x66, 0x55, 0x44, 0x33, 0x22, 0x02 };
    static const uint8_t local_name[248] = "iPod";
    static const uint8_t class_of_device[] = { 0x00, 0x00, 0x00 };
    static const uint8_t voice_setting[] = { 0x60, 0x00 };
    static const uint8_t num_iac[] = { 0x01 };
    static const uint8_t iac_lap[] = { 0x01, 0x33, 0x8b, 0x9e };
    static const uint8_t scan_enable[] = { 0x00 };
    static const uint8_t page_timeout[] = { 0x00, 0x20 };
    static const uint8_t inquiry_scan[] = { 0x00, 0x08, 0x12, 0x00 };
    static const uint8_t page_scan[] = { 0x00, 0x08, 0x12, 0x00 };
    static const uint8_t link_policy[] = { 0x0f, 0x00 };
    static const uint8_t inquiry_mode[] = { 0x00 };
    static const uint8_t simple_pairing[] = { 0x00 };
    static const uint8_t tx_power[] = { 0x00 };
    static const uint8_t country_code[] = { 0x00 };

#define R(sym) do { *len = sizeof(sym); return sym; } while (0)
    switch (opcode) {
    case 0x1001: R(local_version);
    case 0x1002: R(supported_commands);
    case 0x1003: R(features);
    case 0x1004: R(ext_features);
    case 0x1005: R(buffer_size);
    case 0x1007: R(country_code);
    case 0x1009: R(bd_addr);
    case 0x0c14: R(local_name);
    case 0x0c15: R(page_timeout);
    case 0x0c19: R(scan_enable);
    case 0x0c1b: R(page_scan);
    case 0x0c1d: R(inquiry_scan);
    case 0x0c23: R(class_of_device);
    case 0x0c25: R(voice_setting);
    case 0x0c38: R(num_iac);
    case 0x0c39: R(iac_lap);
    case 0x0c44: R(inquiry_mode);
    case 0x0c55: R(simple_pairing);
    case 0x0c2e: R(tx_power);
    case 0x0f01: R(link_policy);
    default:
        *len = 0;
        return NULL;
    }
#undef R
}

/* Opcodes whose reply is status-only by definition; never trace these. */
static bool bt_is_write_command(uint16_t opcode)
{
    switch (opcode) {
    case 0x0c01:  /* Set Event Mask */
    case 0x0c03:  /* Reset */
    case 0x0c05:  /* Set Event Filter */
    case 0x0c13:  /* Write Local Name */
    case 0x0c16:  /* Write Connection Accept Timeout */
    case 0x0c1a:  /* Write Scan Enable */
    case 0x0c1c:  /* Write Page Scan Activity */
    case 0x0c1e:  /* Write Inquiry Scan Activity */
    case 0x0c24:  /* Write Class of Device */
    case 0x0c26:  /* Write Voice Setting */
    case 0x0c33:  /* Host Buffer Size */
    case 0x0c35:  /* Host Number of Completed Packets */
    case 0x0c3a:  /* Write Current IAC LAP */
    case 0x0c45:  /* Write Inquiry Mode */
    case 0x0c52:  /* Write Extended Inquiry Response */
    case 0x0c56:  /* Write Simple Pairing Mode */
    case 0x0c6d:  /* Write LE Host Supported */
    case 0x0f02:  /* Write Default Link Policy Settings */
        return true;
    default:
        return false;
    }
}

/* Controller turnaround, in microseconds. */
static int64_t bt_latency_ns(void)
{
    static int64_t ns = -1;
    if (ns < 0) {
        const char *v = getenv("IT_BT_LATENCY_US");
        ns = (v ? strtoll(v, NULL, 0) : 2000) * 1000;
    }
    return ns;
}

static void bt_timer(void *opaque);

static void bt_arm(ItBtChardev *bt)
{
    timer_mod(bt->timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + bt_latency_ns());
}

static void bt_flush(ItBtChardev *bt)
{
    Chardev *chr = CHARDEV(bt);

    while (bt->resp_head < bt->resp_tail) {
        int can = qemu_chr_be_can_write(chr);
        int have = bt->resp_tail - bt->resp_head;

        if (can <= 0) {
            return;
        }
        if (can > have) {
            can = have;
        }
        qemu_chr_be_write(chr, bt->resp + bt->resp_head, can);
        bt->resp_head += can;
    }
    bt->resp_head = bt->resp_tail = 0;
}

static void bt_queue(ItBtChardev *bt, const uint8_t *buf, unsigned len)
{
    if (bt->resp_tail + len > BT_RESP_MAX) {
        /* Nothing this model emits comes close; drop rather than corrupt. */
        fprintf(stderr, "[BT] response buffer full, dropping %u bytes\n", len);
        return;
    }
    memcpy(bt->resp + bt->resp_tail, buf, len);
    bt->resp_tail += len;
    bt_arm(bt);
}

static void bt_timer(void *opaque)
{
    bt_flush(IT_BT_CHARDEV(opaque));
}

static void bt_command_complete(ItBtChardev *bt, uint16_t opcode)
{
    uint8_t ev[4 + 4 + 248];
    unsigned rlen = 0;
    const uint8_t *ret = bt_ret_params(opcode, &rlen);

    if (!ret && !bt_is_write_command(opcode) && bt_trace()) {
        fprintf(stderr, "[BT] answering opcode 0x%04x status-only; if the "
                "guest expected return parameters, add it to "
                "bt_ret_params()\n", opcode);
    }

    ev[0] = H4_EVT;
    ev[1] = EVT_CMD_COMPLETE;
    ev[2] = 3 + 1 + rlen;          /* num_pkts + opcode + status + params */
    ev[3] = 1;                     /* the host may send one more command */
    ev[4] = opcode & 0xff;
    ev[5] = opcode >> 8;
    ev[6] = 0x00;                  /* success */
    if (rlen) {
        memcpy(ev + 7, ret, rlen);
    }
    bt_queue(bt, ev, 7 + rlen);
}

static int bt_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    ItBtChardev *bt = IT_BT_CHARDEV(chr);
    int i;

    for (i = 0; i < len; i++) {
        if (bt->cmd_len == 0 && buf[i] != H4_CMD) {
            /*
             * ACL and SCO are silently dropped: with no radio there is nothing
             * they could be addressed to, and the guest only sends them after a
             * connection this model can never report.
             */
            if (bt_trace() && buf[i] != H4_ACL && buf[i] != H4_SCO) {
                fprintf(stderr, "[BT] unexpected H4 indicator 0x%02x\n", buf[i]);
            }
            continue;
        }
        if (bt->cmd_len < sizeof(bt->cmd)) {
            bt->cmd[bt->cmd_len++] = buf[i];
        }
        /* 1 indicator + 2 opcode + 1 length, then that many parameter bytes. */
        if (bt->cmd_len >= 4 && bt->cmd_len == 4u + bt->cmd[3]) {
            uint16_t opcode = bt->cmd[1] | (bt->cmd[2] << 8);

            if (bt_trace()) {
                fprintf(stderr, "[BT] cmd ogf=0x%02x ocf=0x%03x (0x%04x) "
                        "plen=%u\n", opcode >> 10, opcode & 0x3ff, opcode,
                        bt->cmd[3]);
            }
            bt->cmd_len = 0;
            bt_command_complete(bt, opcode);
        }
    }
    return len;
}

static void bt_chr_accept_input(Chardev *chr)
{
    bt_arm(IT_BT_CHARDEV(chr));
}

static void bt_chr_open(Chardev *chr, ChardevBackend *backend,
                        bool *be_opened, Error **errp)
{
    IT_BT_CHARDEV(chr)->timer =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, bt_timer, chr);
    *be_opened = true;
}

static void bt_chr_class_init(ObjectClass *oc, void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->open = bt_chr_open;
    cc->chr_write = bt_chr_write;
    cc->chr_accept_input = bt_chr_accept_input;
}

static const TypeInfo bt_chr_type_info = {
    .name = TYPE_CHARDEV_IT_BT,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(ItBtChardev),
    .class_init = bt_chr_class_init,
};

static void bt_register_types(void)
{
    type_register_static(&bt_chr_type_info);
}

type_init(bt_register_types)

Chardev *it_bt_chardev(Chardev *user)
{
    /*
     * A chardev the user asked for on -serial wins, so UART1 can still be
     * pointed at a socket to watch or replace the HCI. IT_BT=0 leaves the port
     * bare, which is the pre-2026-08 behaviour and the way to bisect against
     * this model.
     */
    const char *env = getenv("IT_BT");

    if (user || (env && env[0] == '0')) {
        return user;
    }
    return qemu_chardev_new(NULL, TYPE_CHARDEV_IT_BT, NULL, NULL,
                            &error_abort);
}
