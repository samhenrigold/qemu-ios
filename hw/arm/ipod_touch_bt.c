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
#include "hw/core/sysbus.h"
#include "system/reset.h"
#include "migration/vmstate.h"
#include "hw/arm/ipod_touch_2g.h"
#include "hw/arm/ipod_touch_1g.h"

/* chardev_new() asserts on the prefix; every chardev type carries it. */
#define TYPE_CHARDEV_IT_BT "chardev-ipodtouch-bt-hci"

/* H4 packet indicators. */
#define H4_CMD   0x01
#define H4_ACL   0x02
#define H4_SCO   0x03
#define H4_EVT   0x04

#define EVT_CMD_COMPLETE 0x0e

#define BT_RESP_MAX 1024

/* Unprovisioned controller default, in HCI least-significant-byte order. */
static const uint8_t bt_default_addr[6] = { 0x66, 0x55, 0x44, 0x33, 0x22, 0x02 };

struct ItBtChardev {
    Chardev parent;

    uint8_t cmd[260];       /* one H4 command: 3 header + up to 255 payload */
    unsigned cmd_len;
    uint8_t bd_addr[6];

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
     * The machine bt-latency-us option tunes it; the default is comfortably inside the 10 s
     * the guest allows per HCI_Reset and well clear of its setup path.
     */
    QEMUTimer *timer;
    int64_t latency_ns;

    /*
     * The M68's CSR BlueCore (BC4) on UART3, H4 until BlueTool's warm reset: it answers
     * BCCMD (HCI vendor command 0xFC00, channel descriptor 0xC2) with a vendor event 0xFF
     * carrying the GETRESP, and a byte that starts no H4 packet (BlueTool's autobaud
     * training pattern) with one Hardware Error event, the BlueCore's report of an H4
     * framing error, which is what BlueTool's autobaud waits for.
     */
    bool csr;
    bool framing_reported;
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
static const uint8_t *bt_ret_params(ItBtChardev *bt, uint16_t opcode,
                                    unsigned *len)
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
    case 0x1009:
        *len = sizeof(bt->bd_addr);
        return bt->bd_addr;
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
    case 0xfc01:  /* BCM Write BD_ADDR */
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

static void bt_timer(void *opaque);

static void bt_arm(ItBtChardev *bt)
{
    timer_mod(bt->timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + bt->latency_ns);
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
    const uint8_t *ret = bt_ret_params(bt, opcode, &rlen);
    uint8_t status = 0;

    /* BlueTool supplies the provisioned address after downloading the BCM
     * firmware, then sends HCI_Reset. Retain that programmed identity across
     * HCI_Reset; a board reset starts a new controller initialization instead.
     * Linux btbcm_set_bdaddr uses the same six-byte vendor command. */
    if (opcode == 0xfc01) {
        if (bt->cmd[3] != sizeof(bt->bd_addr)) {
            status = 0x12; /* Invalid HCI Command Parameters */
        } else {
            memcpy(bt->bd_addr, bt->cmd + 4, sizeof(bt->bd_addr));
            if (bt_trace()) {
                fprintf(stderr, "[BT] programmed address %02x:%02x:%02x:"
                        "%02x:%02x:%02x\n", bt->bd_addr[5], bt->bd_addr[4],
                        bt->bd_addr[3], bt->bd_addr[2], bt->bd_addr[1],
                        bt->bd_addr[0]);
            }
        }
    } else if (opcode == 0x1009 && bt->cmd[3] != 0) {
        status = 0x12;
        ret = NULL;
        rlen = 0;
    }

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
    ev[6] = status;
    if (rlen) {
        memcpy(ev + 7, ret, rlen);
    }
    bt_queue(bt, ev, 7 + rlen);

    /*
     * BCM_DOWNLOAD_MINIDRIVER is followed by a LAUNCH ANNOUNCEMENT: two ASCII
     * digits the controller emits once the minidriver is running. BlueTool
     * writes the command, waits for this Command Complete, sleeps 50 ms, and
     * then does a RAW read(fd, buf, 2) on the port -- not a packet read. Short
     * of two bytes it prints "Didn't receive enough data"; if either byte is
     * outside '0'-'9' it prints "Bad response from launch anouncement". Either
     * way it abandons the script, and BTServer re-runs the whole thing ten
     * seconds later, forever. That is exactly where this model used to stop.
     *
     * It validates the shape and never reads the value -- 0x3f14..0x406c in
     * the 7E18 /usr/sbin/BlueTool -- so the digits carry no information and
     * there is nothing here to get subtly wrong. Two bytes, both digits, is
     * the whole contract. It has to be exactly two: BlueTool goes straight
     * back to packet reads afterwards, so a longer banner would desync it.
     */
    if (opcode == 0xfc2e) {
        static const uint8_t launch_announcement[] = { '0', '0' };

        bt_queue(bt, launch_announcement, sizeof(launch_announcement));
    }
}

/*
 * BCCMD (CSR BlueCore Command): type, length (16-bit words, header included), seq,
 * varid, status, then the value, all little endian. GETREQ and SETREQ are answered
 * with GETRESP (type 1), status OK and the request's value; the PS keys BlueTool
 * writes (clock, UART rate, coexistence, patches) are accepted. Warm and cold reset
 * (varid 0x4002, 0x4001) restart the chip with no answer, as the chip does.
 */
static void bt_csr_bccmd(ItBtChardev *bt)
{
    const uint8_t *p = bt->cmd + 5;          /* past the H4 header and the 0xC2 descriptor */
    unsigned n = bt->cmd[3] - 1;
    uint8_t ev[3 + 255];

    if (n < 10) {
        return;
    }
    uint16_t varid = p[6] | p[7] << 8;
    if (bt_trace()) {
        fprintf(stderr, "[BT] BCCMD type %u varid 0x%04x\n", p[0] | p[1] << 8, varid);
    }
    if (varid == 0x4001 || varid == 0x4002) {
        return;
    }
    ev[0] = H4_EVT;
    ev[1] = 0xff;
    ev[2] = 1 + n;
    ev[3] = 0xc2;
    memcpy(ev + 4, p, n);
    ev[4] = 1;                               /* GETRESP */
    ev[5] = 0;
    ev[12] = ev[13] = 0;                     /* status OK */
    bt_queue(bt, ev, 4 + n);
}

static int bt_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    ItBtChardev *bt = IT_BT_CHARDEV(chr);
    int i;

    for (i = 0; i < len; i++) {
        if (bt->csr && bt->cmd_len == 0 && buf[i] != H4_CMD && buf[i] != H4_ACL && buf[i] != H4_SCO) {
            if (!bt->framing_reported) {
                static const uint8_t hw_error[] = { H4_EVT, 0x10, 0x01, 0x00 };

                bt->framing_reported = true;
                bt_queue(bt, hw_error, sizeof(hw_error));
            }
            continue;
        }
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
            bt->framing_reported = false;
            if (bt->csr && opcode == 0xfc00 && bt->cmd[3] && bt->cmd[4] == 0xc2) {
                bt_csr_bccmd(bt);
            } else {
                bt_command_complete(bt, opcode);
            }
        }
    }
    return len;
}

static void bt_chr_accept_input(Chardev *chr)
{
    bt_arm(IT_BT_CHARDEV(chr));
}

/*
 * A chardev gets no machine reset of its own, and this one carries state that
 * MUST NOT outlive a guest reboot: a half-parsed command, and queued reply
 * bytes for a command the previous boot asked. Delivered to the newly booted
 * guest they land in the middle of BlueTool's packet stream and desync it, so
 * bring-up fails and Bluetooth reads "unavailable" -- on that boot only, which
 * is what made this look intermittent. Reset with the machine instead.
 */
static void bt_machine_reset(void *opaque)
{
    ItBtChardev *bt = IT_BT_CHARDEV(opaque);

    bt->cmd_len = 0;
    bt->framing_reported = false;
    memcpy(bt->bd_addr, bt_default_addr, sizeof(bt->bd_addr));
    bt->resp_head = bt->resp_tail = 0;
    timer_del(bt->timer);
}

static int bt_post_load(void *opaque, int version_id)
{
    ItBtChardev *bt = opaque;
    if (version_id < 2) {
        memcpy(bt->bd_addr, bt_default_addr, sizeof(bt->bd_addr));
    }
    if (bt->cmd_len > sizeof(bt->cmd) || bt->resp_head > bt->resp_tail ||
        bt->resp_tail > sizeof(bt->resp)) return -EINVAL;
    return 0;
}

static const VMStateDescription vmstate_it_bt = {
    .name = "ipodtouch-bt-hci",
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = bt_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(cmd, ItBtChardev, 260),
        VMSTATE_UINT32(cmd_len, ItBtChardev),
        VMSTATE_UINT8_ARRAY(resp, ItBtChardev, BT_RESP_MAX),
        VMSTATE_UINT32(resp_head, ItBtChardev),
        VMSTATE_UINT32(resp_tail, ItBtChardev),
        VMSTATE_TIMER_PTR(timer, ItBtChardev),
        VMSTATE_UINT8_ARRAY_V(bd_addr, ItBtChardev, 6, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void bt_chr_finalize(Object *obj)
{
    ItBtChardev *bt = IT_BT_CHARDEV(obj);
    if (bt->timer) {
        vmstate_unregister(NULL, &vmstate_it_bt, bt);
        qemu_unregister_reset(bt_machine_reset, bt);
        timer_free(bt->timer);
    }
}

static bool bt_chr_open(Chardev *chr, ChardevBackend *backend,
                        Error **errp)
{
    IT_BT_CHARDEV(chr)->latency_ns = 2000000;
    IT_BT_CHARDEV(chr)->timer =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, bt_timer, chr);
    bt_machine_reset(chr);
    qemu_register_reset(bt_machine_reset, chr);
    vmstate_register(NULL, 0, &vmstate_it_bt, chr);
    qemu_chr_be_event(chr, CHR_EVENT_OPENED);
    return true;
}

static void bt_chr_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->chr_open = bt_chr_open;
    cc->chr_write = bt_chr_write;
    cc->chr_accept_input = bt_chr_accept_input;
}

static const TypeInfo bt_chr_type_info = {
    .name = TYPE_CHARDEV_IT_BT,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(ItBtChardev),
    .class_init = bt_chr_class_init,
    .instance_finalize = bt_chr_finalize,
};

static void bt_register_types(void)
{
    type_register_static(&bt_chr_type_info);
}

type_init(bt_register_types)

Chardev *it_bt_chardev(Chardev *user, bool enabled, uint32_t latency_us)
{
    /*
     * A chardev the user asked for on -serial wins, so UART1 can still be
     * pointed at a socket to watch or replace the HCI. bt=off leaves the port
     * bare, which is the pre-2026-08 behavior and the way to bisect against
     * this model.
     */
    if (user || !enabled) {
        return user;
    }
    Chardev *chr = qemu_chardev_new(NULL, TYPE_CHARDEV_IT_BT, NULL, NULL,
                                    &error_abort);
    IT_BT_CHARDEV(chr)->latency_ns = (int64_t)latency_us * 1000;
    return chr;
}

/* The M68's CSR BlueCore on UART3 (see ItBtChardev.csr); a -serial chardev for that port wins. */
Chardev *it_bt_csr_chardev(Chardev *user)
{
    if (user) {
        return user;
    }
    Chardev *chr = qemu_chardev_new(NULL, TYPE_CHARDEV_IT_BT, NULL, NULL, &error_abort);
    IT_BT_CHARDEV(chr)->csr = true;
    return chr;
}
