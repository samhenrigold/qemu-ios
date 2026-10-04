/*
 * TI bq27545 gas gauge on the iPad 1's HDQ line, which is UART5's TX pin
 * (DT /arm-io/uart5/gas-gauge, "gas-gauge,bq27540|gas-gauge,hdq").
 *
 * Nothing in the kernel speaks HDQ: AppleHDQGasGaugeControl only exposes
 * the battery SWI GPIO and the DT tables, and AppleS5L8900XSerial publishes
 * /dev/tty.gas-gauge. The protocol is bit-banged by configd's
 * AppleHDQGasGauge.bundle (7B500 shared cache 0x319f5000, transaction at
 * 0x319f5ae4): raw 19200 8N1 port; TIOCSBRK, 200 us, TIOCCBRK, 40 us,
 * TCIOFLUSH; then one UART byte per HDQ bit, LSB first, 1 = 0xFE and
 * 0 = 0xC0; it reads its own symbols back off the shared wire and fails with
 * "read mismatch" unless they are byte-identical; for a read it then takes 8
 * more bytes and decodes each as bit = (byte > 0xF8). A read is the 7-bit
 * register; a write is register | 0x80 followed by 8 data bits.
 *
 * So this chardev echoes every byte it is given, and after the 8th symbol
 * of a read command appends the register's 8 symbols. Replies go out from a
 * timer, not from inside chr_write (see ipod_touch_bt.c for why).
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "chardev/char.h"
#include "hw/arm/s5l8930.h"
#include "system/reset.h"

#define HDQ_SYM_1   0xfe
#define HDQ_SYM_0   0xc0
#define HDQ_RESP_MAX 64

/* bq27545 standard commands (16-bit LE at even addresses) */
#define BQ_CNTL     0x00
#define BQ_TEMP     0x06    /* 0.1 K */
#define BQ_VOLT     0x08    /* mV */
#define BQ_FLAGS    0x0a
#define BQ_NAC      0x0c    /* mAh */
#define BQ_FAC      0x0e
#define BQ_RM       0x10
#define BQ_FCC      0x12
#define BQ_AI       0x14    /* mA, signed */
#define BQ_TTE      0x16    /* min */
#define BQ_TTF      0x18
#define BQ_SOH      0x28    /* % */
#define BQ_CC       0x2a
#define BQ_SOC      0x2c    /* % */
#define BQ_DCAP     0x3c
#define BQ_DFCLS    0x3e
#define BQ_DFBLK    0x3f
#define BQ_BLOCK    0x40    /* 32 bytes of data flash */
#define BQ_BLOCKSUM 0x60

#define BQ_FLAG_DSG     (1u << 0)
#define BQ_FLAG_BAT_DET (1u << 3)
#define BQ_FLAG_CHG     (1u << 8)   /* (fast) charging allowed */
#define BQ_FLAG_FC      (1u << 9)   /* full charge */

#define CNTL_DEVICE_TYPE 0x0001
#define CNTL_FW_VERSION  0x0002

struct HdqGaugeChardev {
    Chardev parent;
    QEMUTimer *timer;

    uint8_t regs[0x80];
    uint32_t bits;          /* symbols decoded so far, LSB first */
    unsigned nbits;

    uint8_t resp[HDQ_RESP_MAX];
    unsigned resp_head, resp_tail;

    int level;              /* %, as the machine last set it */
    bool charging;
};
typedef struct HdqGaugeChardev HdqGaugeChardev;

DECLARE_INSTANCE_CHECKER(HdqGaugeChardev, HDQ_GAUGE_CHARDEV,
                         TYPE_CHARDEV_S5L8930_HDQ)

static void hdq_flush(HdqGaugeChardev *g)
{
    Chardev *chr = CHARDEV(g);

    while (g->resp_head < g->resp_tail) {
        int can = qemu_chr_be_can_write(chr);
        int have = g->resp_tail - g->resp_head;


        if (can <= 0) {
            return;
        }
        qemu_chr_be_write(chr, g->resp + g->resp_head, MIN(can, have));
        g->resp_head += MIN(can, have);
    }
    g->resp_head = g->resp_tail = 0;
}

static void hdq_timer(void *opaque)
{
    hdq_flush(HDQ_GAUGE_CHARDEV(opaque));
}

static void hdq_queue(HdqGaugeChardev *g, uint8_t byte)
{
    if (g->resp_tail < HDQ_RESP_MAX) {
        g->resp[g->resp_tail++] = byte;
    }
    /* A real bit slot is ~190 us; one delay per burst is plenty. */
    timer_mod(g->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS);
}

static uint8_t hdq_read_reg(HdqGaugeChardev *g, uint8_t reg)
{
    if (reg == BQ_BLOCKSUM) {
        unsigned sum = 0, i;

        for (i = 0; i < 32; i++) {
            sum += g->regs[BQ_BLOCK + i];
        }
        return 0xff - (sum & 0xff);
    }
    return g->regs[reg];
}

static void hdq_write_reg(HdqGaugeChardev *g, uint8_t reg, uint8_t data)
{
    g->regs[reg] = data;
    if (reg == BQ_CNTL + 1) {
        /* Control(): the subcommand's answer reads back from CNTL. */
        uint16_t sub = lduw_le_p(&g->regs[BQ_CNTL]);
        uint16_t ans = sub == CNTL_DEVICE_TYPE ? 0x0545 :
                       sub == CNTL_FW_VERSION  ? 0x0109 : 0;

        stw_le_p(&g->regs[BQ_CNTL], ans);
    }
}

static int hdq_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    HdqGaugeChardev *g = HDQ_GAUGE_CHARDEV(chr);
    int i;

    for (i = 0; i < len; i++) {
        if (getenv("S5L8930_HDQ_TRACE")) {
            fprintf(stderr, "[HDQ] tx 0x%02x nbits %u bits 0x%04x\n",
                    buf[i], g->nbits, g->bits);
        }
        hdq_queue(g, buf[i]);                    /* the wire echo */
        g->bits |= (buf[i] > 0xf8) << g->nbits;
        g->nbits++;
        if (g->nbits == 8 && !(g->bits & 0x80)) {
            uint8_t v = hdq_read_reg(g, g->bits & 0x7f);
            int b;

            for (b = 0; b < 8; b++) {
                hdq_queue(g, (v >> b) & 1 ? HDQ_SYM_1 : HDQ_SYM_0);
            }
            g->bits = g->nbits = 0;
        } else if (g->nbits == 16) {
            hdq_write_reg(g, g->bits & 0x7f, g->bits >> 8);
            g->bits = g->nbits = 0;
        }
    }
    return len;
}

static void hdq_chr_accept_input(Chardev *chr)
{
    hdq_flush(HDQ_GAUGE_CHARDEV(chr));
}

#define BQ_CAPACITY_MAH 6500     /* K48 DesignCapacity from ioreg */
#define BQ_CURRENT_MA   300

/*
 * The registers that follow the level and charge state; configd's gauge
 * plugin polls them and publishes them on IOPMPowerSource, which is what the
 * status bar shows. ponytail: voltage is a line from 3.5 V empty to 4.2 V
 * full and the current a fixed 300 mA either way; model a discharge curve if
 * a guest ever keys off voltage rather than capacity.
 */
static void hdq_apply_battery(HdqGaugeChardev *g)
{
    int rm = BQ_CAPACITY_MAH * g->level / 100;
    uint16_t flags = BQ_FLAG_BAT_DET;

    if (g->charging) {
        flags |= BQ_FLAG_CHG | (g->level >= 100 ? BQ_FLAG_FC : 0);
    } else {
        flags |= BQ_FLAG_DSG;
    }
    stw_le_p(&g->regs[BQ_VOLT], 3500 + 7 * g->level);
    stw_le_p(&g->regs[BQ_FLAGS], flags);
    stw_le_p(&g->regs[BQ_NAC], rm);
    stw_le_p(&g->regs[BQ_FAC], rm);
    stw_le_p(&g->regs[BQ_RM], rm);
    stw_le_p(&g->regs[BQ_AI], (uint16_t)(g->charging ? BQ_CURRENT_MA : -BQ_CURRENT_MA));
    stw_le_p(&g->regs[BQ_TTE], g->charging ? 0xffff : rm * 60 / BQ_CURRENT_MA);
    stw_le_p(&g->regs[BQ_TTF], g->charging ? (BQ_CAPACITY_MAH - rm) * 60 / BQ_CURRENT_MA : 0xffff);
    stw_le_p(&g->regs[BQ_SOC], g->level);
}

void s5l8930_hdq_set_battery(Chardev *chr, int level, bool charging)
{
    HdqGaugeChardev *g = HDQ_GAUGE_CHARDEV(chr);

    g->level = MIN(MAX(level, 0), 100);
    g->charging = charging;
    hdq_apply_battery(g);
}

/* A healthy battery at whatever level the machine last set (80% at power-on). */
static void hdq_reset(void *opaque)
{
    HdqGaugeChardev *g = opaque;

    timer_del(g->timer);
    memset(g->regs, 0, sizeof(g->regs));
    stw_le_p(&g->regs[BQ_TEMP], 2981);
    stw_le_p(&g->regs[BQ_FCC], BQ_CAPACITY_MAH);
    stw_le_p(&g->regs[BQ_SOH], 100);
    stw_le_p(&g->regs[BQ_CC], 10);
    stw_le_p(&g->regs[BQ_DCAP], BQ_CAPACITY_MAH);
    hdq_apply_battery(g);
    g->bits = g->nbits = 0;
    g->resp_head = g->resp_tail = 0;
}

static void hdq_chr_open(Chardev *chr, ChardevBackend *backend,
                         bool *be_opened, Error **errp)
{
    HdqGaugeChardev *g = HDQ_GAUGE_CHARDEV(chr);

    g->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, hdq_timer, chr);
    g->level = 80;
    qemu_register_reset(hdq_reset, g);
    hdq_reset(g);
    *be_opened = true;
}

static void hdq_chr_finalize(Object *obj)
{
    HdqGaugeChardev *g = HDQ_GAUGE_CHARDEV(obj);

    if (g->timer) {
        qemu_unregister_reset(hdq_reset, g);
        timer_free(g->timer);
    }
}

static void hdq_chr_class_init(ObjectClass *oc, void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->open = hdq_chr_open;
    cc->chr_write = hdq_chr_write;
    cc->chr_accept_input = hdq_chr_accept_input;
}

static const TypeInfo hdq_chr_type_info = {
    .name = TYPE_CHARDEV_S5L8930_HDQ,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(HdqGaugeChardev),
    .instance_finalize = hdq_chr_finalize,
    .class_init = hdq_chr_class_init,
};

static void hdq_register_types(void)
{
    type_register_static(&hdq_chr_type_info);
}

type_init(hdq_register_types)
