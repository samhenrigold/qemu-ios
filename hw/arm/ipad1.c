/*
 * iPad 1 (K48AP): Apple S5L8930 "A4", Cortex-A8, 256 MiB.
 *
 * Milestone 1 of docs/archive/ipad1-PLAN.md: enter the iOS 3.2.2 (7B500) kernel
 * directly, with no bootrom or iBoot, and get its console on UART0. Only the
 * devices the kernel's platform expert needs are modelled. Every other
 * peripheral address falls into an unimplemented-device window that logs the
 * access (-d unimp) instead of faulting, so bring-up can see what the kernel
 * reaches for next.
 *
 * Boot input is a K48KBOOT bundle from imgtools/ipad1_kboot.py: a flat image of
 * physical memory (kernel, filled device tree, boot_args) followed by a 24-byte
 * trailer {char magic[8]; u32 load_pa, entry_pa, bootargs_pa, image_len}.
 * Between the image and the trailer sit optional segments {"K48SEG\0\0";
 * u32 pa, len, flags; data unless flags bit 0 = zero-fill}: iBoot's boot-logo
 * framebuffer. We copy it into DRAM on every reset and start the CPU at entry_pa in ARM state
 * with the MMU off and r0 = bootargs_pa, which is the state the kernel's
 * _start expects from iBoot.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/config-file.h"
#include "qemu/option.h"
#include "net/net.h"
#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/irq.h"
#include "hw/core/split-irq.h"
#include "hw/misc/unimp.h"
#include "system/runstate.h"
#include "hw/usb/hcd-ehci.h"
#include "hw/usb/hcd-ohci.h"
#include "hw/sysbus.h"
#include "hw/arm/exynos4210.h"
#include "hw/arm/ipod_touch_buttons.h"
#include "hw/arm/ipod_touch_mipi_dsi.h"
#include "hw/arm/ipod_touch_usb_otg.h"
#include "hw/arm/ipod_touch_usb_phys.h"
#include "hw/arm/ipod_touch_spi.h"
#include "hw/arm/ipod_touch_amc.h"
#include "hw/arm/ipod_touch_pke.h"
#include "hw/arm/ipod_touch_lis302dl.h"
#include "hw/arm/ipod_touch_cs42l58.h"
#include "hw/arm/ipod_touch_cd3272_mikey.h"
#include "chardev/char.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/s5l8930.h"
#include "hw/intc/pl192.h"
#include "hw/qdev-properties.h"
#include "system/reset.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "target/arm/cpregs.h"
#include "hw/arm/guest-services/general.h"
#include "hw/arm/guest-services/gles.h"
#include "hw/arm/guest-pasteboard.h"
#include "hw/arm/guest-package.h"
#include "hw/arm/ipod-agent.h"
#include "hw/arm/it_iboot.h"
#include "qemu/guest-random.h"
#include "ui/console.h"
#include "ui/input.h"
#include "qapi/visitor.h"

#define TYPE_IPAD1_MACHINE MACHINE_TYPE_NAME("ipad1")
OBJECT_DECLARE_SIMPLE_TYPE(IPad1MachineState, IPAD1_MACHINE)

struct IPad1MachineState {
    MachineState parent;
    ARMCPU *cpu;
    MemoryRegion dram;
    MemoryRegion dram_hi;                /* DRAM mirror at 0x50000000 (iBoot) */
    MemoryRegion chipid;
    MemoryRegion sram;
    MemoryRegion bootrom;
    MemoryRegion bootrom_alias;
    MemoryRegion cpu_debug;
    DeviceState *vic[S5L8930_VIC_COUNT];
    DeviceState *gpio;
    DeviceState *pmu;
    DeviceState *ltc;                    /* charger: USB cable level */
    synopsys_usb_state *usb_otg;
    IPodTouchMultitouchState *mt;
    char *kboot_path;
    char *iboot_path;
    char *bootrom_path;
    bool development_fuses;
    char *gid_blobs_path;
    char *nand_path;
    char *nand_overlay_path;
    char *nor_path;
    char *nor_rw_path;                   /* private writable NOR copy (effaceable persists); empty = in-memory */
    char *die_id;                        /* ChipID words 2-3 of the unit, hex pair */
    char *usb_tcp_addr;                  /* host bridge, empty = no link */
    bool usb_cable;                      /* cable present; runtime qom-set */
    bool wifi;                           /* BCM4329 behind the IOP's SDIO ring */
    bool iop_core;                       /* run the IOP firmware on a second core (default; off: the HLE) */
    DeviceState *iopcore;
    bool gles_debug;                     /* paint what the GL bridge refuses magenta (tests) */
    bool kbd_cmd, kbd_shift;
    bool btn_hold, btn_home;             /* button-hold/-home properties */
    int kbd_btn_held[Q_KEY_CODE__MAX];   /* qcode -> 1 + button pin */
    int mtt_x[MT_MAX_FINGERS], mtt_y[MT_MAX_FINGERS];  /* latched per slot */
    bool mtt_seen[MT_MAX_FINGERS];
    GuestPasteboard pb;                  /* hw/arm/guest-pasteboard.c */
    GuestPackage pkg;                    /* hw/arm/guest-package.c */
    IPodAgent *agent;                    /* hw/arm/ipod-agent.c: it_agent, as on the iPod */
    /* App controls, same property names as the iPod machine. */
    LIS302DLState *accel;
    DeviceState *compass;                /* AK8973; takes its pose from accel */
    double accel_pitch, accel_roll;      /* degrees, as the app sends them */
    bool accel_flat;
    double battery_level;                /* % */
    int battery_mode;                    /* 0 auto (follow cable), 1 on, 2 off */
    double battery_drain;                /* accepted for the bridge; unused */
    QEMUTimer *pwroff_timer;             /* system_powerdown gesture */
    DeviceState *display;                /* its dart2 also serves the scaler */
    int pwroff_phase, pwroff_step, pwroff_orient;
    bool usb_charger;                    /* host grants high-power current */
    Chardev *gauge;
};

static const char *const ipad1_battery_modes[] = { "auto", "on", "off" };

/* GHWCFG1-4 of the DWC OTG core; same synthesis as the S5L8720's. */
static uint32_t s5l8930_usb_hwcfg[] = { 0, 0x7a8f60d0, 0x082000e8, 0x01f08024 };

#define KBOOT_MAGIC "K48KBOOT"
#define KBOOT_TRAILER_LEN 24
#define IPAD1_IBOOT_BASE 0x5ff00000     /* iBoot-817.29 link address */
#define KBOOT_SEGMENT_LEN 20

static qemu_irq ipad1_irq(IPad1MachineState *s, int irq)
{
    qemu_irq ap = qdev_get_gpio_in(s->vic[irq / 32], irq % 32);
    DeviceState *split;

    if (!s->iopcore) {
        return ap;
    }
    /* The IOP's VICs see the same sources under the same numbers. */
    split = qdev_new(TYPE_SPLIT_IRQ);
    qdev_prop_set_uint16(split, "num-lines", 2);
    qdev_realize_and_unref(split, NULL, &error_fatal);
    qdev_connect_gpio_out(split, 0, ap);
    qdev_connect_gpio_out(split, 1, s5l8930_iop_core_irq(s->iopcore, irq));
    return qdev_get_gpio_in(split, 0);
}

/*
 * Stage the K48KBOOT bundle and point the CPU at it. Done on every reset, like
 * the iPod machine's bootrom staging: iOS writes over the memory the bundle
 * occupies, so a reset that did not re-stage it would resume into junk.
 */
/*
 * The guest-services trap (mcr p15,3,Rn,c15,c15,0) the GLES shim uses, as on
 * the iPod machine. GLES, the pasteboard, guest packages (it_boot,
 * hw/arm/guest-package.c) and the guest agent (it_agent: the foreground app,
 * lock state, launch, sync; no stock service answers those); the iPod's
 * keyboard services are replaced by the USB keyboard here
 * (docs/ipad1/guest-services.md).
 */
static int ipad1_agent_copy(void *opaque, uint32_t address, uint8_t *data,
                            size_t length, bool write)
{
    if (length && length - 1 > UINT32_MAX - address) {
        return -1;
    }
    return cpu_memory_rw_debug(opaque, address, data, length, write);
}

static void ipad1_qemu_call(CPUARMState *env, const ARMCPRegInfo *ri,
                            uint64_t value)
{
    CPUState *cs = env_cpu(env);
    IPad1MachineState *s = IPAD1_MACHINE(qdev_get_machine());
    qemu_call_t q;
    int32_t err = 0;

    if (cpu_memory_rw_debug(cs, value, (uint8_t *)&q, sizeof(q), 0)) {
        return;
    }
    switch (q.call_number) {
    case QC_GLES:
        q.retval = qc_handle_gles(cs, &q.args.gles);
        break;
    case QC_GLES_PING:
        q.retval = QC_GLES_PING_MAGIC;
        break;
    case QC_AG_HELLO:
    case QC_AG_POLL:
    case QC_AG_READ:
    case QC_AG_WRITE:
    case QC_AG_DONE:
    case QC_AG_HOSTTIME:
    case QC_UI_POLL:
    case QC_UI_READ:
    case QC_UI_WRITE:
    case QC_UI_DONE:
    case QC_AG_UI_ROUTE: {
        uint64_t candidate = 0;
        if (q.call_number == QC_AG_HELLO || q.call_number == QC_AG_UI_ROUTE) {
            qemu_guest_getrandom_nofail(&candidate, sizeof(candidate));
        }
        q.retval = ipod_agent_call(s->agent, q.call_number, q.args.ag.token,
                                   q.args.ag.buffer_guest_ptr, q.args.ag.offset,
                                   q.args.ag.length,
                                   qemu_clock_get_ms(QEMU_CLOCK_REALTIME),
                                   candidate, ipad1_agent_copy, cs);
        err = q.retval < 0 ? EINVAL : 0;
        break;
    }
    default:
        if (!guest_pb_call(&s->pb, cs, &q, &err) &&
            !guest_pkg_call(&s->pkg, cs, &q, &err)) {
            return;
        }
    }
    q.error = err;
    cpu_memory_rw_debug(cs, value, (uint8_t *)&q, sizeof(q), 1);
}

static const ARMCPRegInfo ipad1_cp_reginfo[] = {
    { .name = "QEMU_CALL", .cp = 15, .opc1 = 3, .crn = 15, .crm = 15,
      .opc2 = 0, .access = PL0_RW, .state = ARM_CP_STATE_AA32,
      .type = ARM_CP_IO | ARM_CP_NO_RAW | ARM_CP_RAISES_EXC, /* gles_guest_rw */
      .readfn = qemu_call_status,
      .writefn = ipad1_qemu_call },
};

static void ipad1_cpu_reset(void *opaque)
{
    IPad1MachineState *s = IPAD1_MACHINE(opaque);
    CPUState *cs = CPU(s->cpu);
    g_autofree char *data = NULL;
    g_autoptr(GError) gerr = NULL;
    gsize size;
    const uint8_t *trailer;
    uint32_t load_pa, entry_pa, bootargs_pa, image_len;

    gles_host_set_debug(s->gles_debug);
    gles_host_reset();
    guest_pkg_reset(&s->pkg);
    ipod_agent_reset(s->agent);
    cpu_reset(cs);

    if (s->bootrom_path) {
        cpu_set_pc(cs, 0);
        return;
    }

    /*
     * iboot=: a decrypted iBoot image, entered at its link address the way
     * LLB hands off (r0-r3 = 0, privileged ARM, MMU off). iBoot rebuilds its
     * own MMU, stacks and BSS, so nothing else needs staging.
     */
    if (s->iboot_path) {
        if (!g_file_get_contents(s->iboot_path, &data, &size, &gerr)) {
            error_report("ipad1: cannot read iBoot '%s': %s",
                         s->iboot_path, gerr->message);
            exit(1);
        }
        address_space_write(&address_space_memory, IPAD1_IBOOT_BASE,
                            MEMTXATTRS_UNSPECIFIED, data, size);
        cpu_set_pc(cs, IPAD1_IBOOT_BASE);
        return;
    }

    if (!g_file_get_contents(s->kboot_path, &data, &size, &gerr)) {
        error_report("ipad1: cannot read kboot bundle '%s': %s",
                     s->kboot_path, gerr->message);
        exit(1);
    }
    if (size < KBOOT_TRAILER_LEN ||
        memcmp(data + size - KBOOT_TRAILER_LEN, KBOOT_MAGIC, 8) != 0) {
        error_report("ipad1: '%s' is not a K48KBOOT bundle", s->kboot_path);
        exit(1);
    }
    trailer = (const uint8_t *)data + size - KBOOT_TRAILER_LEN;
    load_pa = ldl_le_p(trailer + 8);
    entry_pa = ldl_le_p(trailer + 12);
    bootargs_pa = ldl_le_p(trailer + 16);
    image_len = ldl_le_p(trailer + 20);

    if (image_len > size - KBOOT_TRAILER_LEN ||
        load_pa < S5L8930_DRAM_BASE ||
        (uint64_t)load_pa + image_len > S5L8930_DRAM_BASE + S5L8930_DRAM_SIZE) {
        error_report("ipad1: kboot bundle does not fit in DRAM "
                     "(load 0x%x len 0x%x)", load_pa, image_len);
        exit(1);
    }
    if (address_space_write(&address_space_memory, load_pa,
                            MEMTXATTRS_UNSPECIFIED, data, image_len) != MEMTX_OK) {
        error_report("ipad1: cannot stage kboot bundle at 0x%x", load_pa);
        exit(1);
    }
    for (gsize off = image_len; off + KBOOT_SEGMENT_LEN <= size - KBOOT_TRAILER_LEN;) {
        const uint8_t *seg = (const uint8_t *)data + off;
        uint32_t pa = ldl_le_p(seg + 8), len = ldl_le_p(seg + 12);
        bool zero = ldl_le_p(seg + 16) & 1;

        off += KBOOT_SEGMENT_LEN + (zero ? 0 : len);
        if (memcmp(seg, "K48SEG\0\0", 8) != 0 || off > size - KBOOT_TRAILER_LEN) {
            error_report("ipad1: malformed segment in kboot bundle");
            exit(1);
        }
        if (zero) {
            address_space_set(&address_space_memory, pa, 0, len, MEMTXATTRS_UNSPECIFIED);
        } else {
            address_space_write(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                                seg + KBOOT_SEGMENT_LEN, len);
        }
    }

    /* cpu_reset leaves us in SVC mode, IRQ/FIQ masked, MMU and caches off. */
    s->cpu->env.regs[0] = bootargs_pa;
    cpu_set_pc(cs, entry_pa);
}

/*
 * Host mouse -> digitizer slot 0. QEMU's absolute coordinates are 0..0x7fff;
 * the digitizer wants 0..1 with y from the bottom (see set_finger()).
 * The panel scans out landscape (1024x768) with the portrait UI rotated; the
 * digitizer is portrait-native. Found by trying all eight axis maps against
 * slide-to-unlock: digitizer x = 1 - panel y, y-from-bottom = 1 - panel x.
 */
static void ipad1_map_touch(int x, int y, float *fx, float *fy)
{
    *fx = 1.0f - y / 32768.0f;
    *fy = 1.0f - x / 32768.0f;
}

static void ipad1_mouse_event(void *opaque, int x, int y, int z, int buttons)
{
    IPodTouchMultitouchState *mt = opaque;

    ipad1_map_touch(x, y, &mt->touch_x, &mt->touch_y);
    if (buttons && !mt->touch_down) {
        ipod_touch_multitouch_on_touch(mt);
    } else if (!buttons && mt->touch_down) {
        ipod_touch_multitouch_on_release(mt);
    } else if (buttons) {
        ipod_touch_multitouch_on_motion(mt);
    }
}

/*
 * Multi-touch from the host: QEMU's "mtt" events (QMP input-send-event), same
 * two-phase protocol as the iPod panel (ipod_touch_lcd_mtt_event): DATA
 * latches a slot's abs X/Y in panel coordinates, BEGIN/UPDATE/END commit it.
 */
static void ipad1_mtt_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    IPad1MachineState *s = IPAD1_MACHINE(qdev_get_machine());
    InputMultiTouchEvent *mtt = evt->u.mtt.data;
    int slot = mtt->slot;
    float fx, fy;

    if (slot < 0 || slot >= MT_MAX_FINGERS) {
        return;
    }
    switch (mtt->type) {
    case INPUT_MULTI_TOUCH_TYPE_DATA:
        if (mtt->axis == INPUT_AXIS_X) {
            s->mtt_x[slot] = mtt->value;
        } else {
            s->mtt_y[slot] = mtt->value;
        }
        s->mtt_seen[slot] = true;
        return;
    case INPUT_MULTI_TOUCH_TYPE_BEGIN:
    case INPUT_MULTI_TOUCH_TYPE_UPDATE:
    case INPUT_MULTI_TOUCH_TYPE_END:
    case INPUT_MULTI_TOUCH_TYPE_CANCEL:
        if (!s->mtt_seen[slot]) {
            return;
        }
        ipad1_map_touch(s->mtt_x[slot], s->mtt_y[slot], &fx, &fy);
        bool down = mtt->type == INPUT_MULTI_TOUCH_TYPE_BEGIN ||
                    mtt->type == INPUT_MULTI_TOUCH_TYPE_UPDATE;
        ipod_touch_multitouch_set_finger(s->mt, slot, fx, fy, down);
        if (!down) {
            s->mtt_seen[slot] = false;
        }
        return;
    default:
        return;
    }
}

static const QemuInputHandler ipad1_mtt_handler = {
    .name  = "iPad Multitouch",
    .mask  = INPUT_EVENT_MASK_MTT,
    .event = ipad1_mtt_event,
};

/* GPIO pin level for the awake path; Home/Hold also go to the PMU, which
 * is the wake source once the kernel has put the AP to sleep. */
static void ipad1_set_button(IPad1MachineState *s, int pin, bool down)
{
    qemu_set_irq(qdev_get_gpio_in(s->gpio, S5L8930_GPIO_PIN(pin)), !down);
    if (pin == S5L8930_GPIO_BTN_HOLD || pin == S5L8930_GPIO_BTN_MENU) {
        s5l8930_d1815_button(s->pmu, pin == S5L8930_GPIO_BTN_HOLD, down);
    }
}

/*
 * QMP system_powerdown -> the user's power-off gesture, as on the iPod machine
 * (ipod_touch_powerdown_req): the one clean shutdown path unmounts the
 * volumes, closes the FTL and ends in the PMU standby write, where QEMU exits.
 * Home first (wakes the panel, or quits a foreground app), hold the hold
 * button until SpringBoard raises "slide to power off", then drag its knob
 * along its track. All on QEMU_CLOCK_VIRTUAL, since
 * SpringBoard's hold threshold is guest time. One sequence at a time; the
 * phase goes back to idle afterwards so a repeat request works.
 */
enum { PWROFF_IDLE, PWROFF_HOME, PWROFF_WAKE, PWROFF_HOLD, PWROFF_SETTLE, PWROFF_DRAG,
       PWROFF_WATCH };
#define PWROFF_WATCH_MS     25000   /* from the request: warn if still running */
#define PWROFF_DRAG_STEPS   24
#define PWROFF_DRAG_LEN     430     /* knob to past the track's end */

/*
 * Where the knob sits on the landscape panel, and which way the track runs,
 * per interface orientation (UIDeviceOrientation 1-4): the sheet is at the
 * top of the UI, which the panel shows rotated. Measured off screendumps of
 * the sheet; 2 and 4 are 1 and 3 turned half way round.
 */
static const struct { int x, y, dx, dy; } pwroff_knob[5] = {
    /* With the LIS331 mounted flipped (eb5d4e5c58) accel-orientation is
     * UIDeviceOrientation, so portrait and upside down swap places here. */
    [1] = {  73, 477,  0, -1 },     /* portrait: track runs up the panel */
    [2] = { 950, 290,  0,  1 },     /* upside down */
    [3] = { 418,  69,  1,  0 },     /* landscape, home button right */
    [4] = { 605, 698, -1,  0 },     /* landscape, home button left */
};

static void ipad1_pwroff_arm(IPad1MachineState *s, int ms)
{
    timer_mod(s->pwroff_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (int64_t)ms * SCALE_MS);
}

static void ipad1_pwroff_touch(IPad1MachineState *s, int px, int py, bool down)
{
    ipad1_mouse_event(s->mt, px * 32767 / 1023, py * 32767 / 767, 0, down);
}

static void ipad1_pwroff_tick(void *opaque)
{
    IPad1MachineState *s = opaque;
    int o = s->pwroff_orient, d;

    switch (s->pwroff_phase) {
    case PWROFF_HOME:
        ipad1_set_button(s, S5L8930_GPIO_BTN_MENU, false);
        s->pwroff_phase = PWROFF_WAKE;
        ipad1_pwroff_arm(s, 2000);          /* panel on / app gone */
        break;
    case PWROFF_WAKE:
        ipad1_set_button(s, S5L8930_GPIO_BTN_HOLD, true);
        s->pwroff_phase = PWROFF_HOLD;
        ipad1_pwroff_arm(s, 3500);          /* > SpringBoard's hold threshold */
        break;
    case PWROFF_HOLD:
        ipad1_set_button(s, S5L8930_GPIO_BTN_HOLD, false);
        s->pwroff_phase = PWROFF_SETTLE;
        ipad1_pwroff_arm(s, 1500);          /* the sheet slides in */
        break;
    case PWROFF_SETTLE:
        ipad1_pwroff_touch(s, pwroff_knob[o].x, pwroff_knob[o].y, true);
        s->pwroff_phase = PWROFF_DRAG;
        s->pwroff_step = 0;
        ipad1_pwroff_arm(s, 80);
        break;
    case PWROFF_DRAG:
        d = PWROFF_DRAG_LEN * ++s->pwroff_step / PWROFF_DRAG_STEPS;
        ipad1_pwroff_touch(s, pwroff_knob[o].x + pwroff_knob[o].dx * d,
                           pwroff_knob[o].y + pwroff_knob[o].dy * d,
                           s->pwroff_step < PWROFF_DRAG_STEPS);
        if (s->pwroff_step < PWROFF_DRAG_STEPS) {
            ipad1_pwroff_arm(s, 80);
        } else {
            /* Now the guest halts: QEMU exits on the PMU standby write. */
            s->pwroff_phase = PWROFF_WATCH;
            ipad1_pwroff_arm(s, PWROFF_WATCH_MS - 7300 - 80 * PWROFF_DRAG_STEPS);
        }
        break;
    case PWROFF_WATCH:
        /* Still here: the gesture missed or the guest is stuck. Say so, and
         * accept another request; the caller decides whether to hard-stop. */
        warn_report("ipad1: system_powerdown: the guest has not halted %d s "
                    "after the request", PWROFF_WATCH_MS / 1000);
        s->pwroff_phase = PWROFF_IDLE;
        break;
    }
}

static void ipad1_powerdown_req(Notifier *n, void *opaque)
{
    IPad1MachineState *s = IPAD1_MACHINE(qdev_get_machine());

    if (s->pwroff_phase != PWROFF_IDLE && s->pwroff_phase != PWROFF_WATCH) {
        return;
    }
    /*
     * The sheet follows the interface orientation, which follows the
     * accelerometer: aim for it there instead of turning the device upright
     * (that visibly flipped the UI to portrait on quit).
     * ponytail: the last accel-orientation set; face up/down (5/6), 0, or a
     * pitch/roll attitude fall back to portrait. Track the UI's own
     * orientation if an app pins one that disagrees with the device.
     */
    s->pwroff_orient = 1;
    if (s->accel && s->accel->orientation >= 1 && s->accel->orientation <= 4) {
        s->pwroff_orient = s->accel->orientation;
    }
    ipad1_set_button(s, S5L8930_GPIO_BTN_MENU, true);
    s->pwroff_phase = PWROFF_HOME;
    ipad1_pwroff_arm(s, 300);
}

static Notifier ipad1_powerdown_notifier = { .notify = ipad1_powerdown_req };

/*
 * Buttons are GPIO port 0 pins 0-4, active low, idle high in the GPIO model.
 * Same host chords as the iPod machine: Cmd+L hold/power, Cmd+Shift+H
 * home/menu, Cmd+- volume down, Cmd+= volume up. A key that pressed a
 * button always releases it, even if Cmd went up first.
 */
static void ipad1_kbd_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    IPad1MachineState *s = IPAD1_MACHINE(qdev_get_machine());
    int q = qemu_input_key_value_to_qcode(evt->u.key.data->key);
    bool down = evt->u.key.data->down;
    int pin = -1;

    switch (q) {
    case Q_KEY_CODE_META_L:
    case Q_KEY_CODE_META_R:
        s->kbd_cmd = down;
        return;
    case Q_KEY_CODE_SHIFT:
    case Q_KEY_CODE_SHIFT_R:
        s->kbd_shift = down;
        return;
    default:
        break;
    }
    if (q < 0 || q >= Q_KEY_CODE__MAX) {
        return;
    }
    if (!down && s->kbd_btn_held[q]) {
        pin = s->kbd_btn_held[q] - 1;
    } else if (down && s->kbd_cmd) {
        switch (q) {
        case Q_KEY_CODE_L:     pin = S5L8930_GPIO_BTN_HOLD; break;
        case Q_KEY_CODE_H:     if (s->kbd_shift) pin = S5L8930_GPIO_BTN_MENU; break;
        case Q_KEY_CODE_MINUS: pin = S5L8930_GPIO_BTN_VOLDOWN; break;
        case Q_KEY_CODE_EQUAL: pin = S5L8930_GPIO_BTN_VOLUP; break;
        default: break;
        }
    }
    if (pin < 0) {
        return;
    }
    s->kbd_btn_held[q] = down ? pin + 1 : 0;
    ipad1_set_button(s, pin, down);
}

/* The app bridge's buttons (contrib/ios-app), on the same pins as the chords. */
void ipad1_press_button(IPodTouchButton button, bool down)
{
    static const int pins[] = {
        [IPOD_TOUCH_BUTTON_HOME] = S5L8930_GPIO_BTN_MENU,
        [IPOD_TOUCH_BUTTON_POWER] = S5L8930_GPIO_BTN_HOLD,
        [IPOD_TOUCH_BUTTON_VOLUP] = S5L8930_GPIO_BTN_VOLUP,
        [IPOD_TOUCH_BUTTON_VOLDOWN] = S5L8930_GPIO_BTN_VOLDOWN,
    };
    IPad1MachineState *s = (IPad1MachineState *)
        object_dynamic_cast(OBJECT(qdev_get_machine()), TYPE_IPAD1_MACHINE);

    if (!s || (unsigned)button >= ARRAY_SIZE(pins)) {
        return;
    }
    ipad1_set_button(s, pins[button], down);
}

static const QemuInputHandler ipad1_kbd_handler = {
    .name  = "iPad Buttons",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = ipad1_kbd_event,
};

static void ipad1_battery_update(IPad1MachineState *s);

static void ipad1_init(MachineState *machine)
{
    IPad1MachineState *s = IPAD1_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj;
    DeviceState *dev, *iop;
    SysBusDevice *sbd;
    int i;

    if (!!s->kboot_path + !!s->iboot_path + !!s->bootrom_path != 1) {
        error_report("ipad1: specify exactly one of iboot=, bootrom=, or kboot=");
        exit(1);
    }

    cpuobj = object_new(machine->cpu_type);
    s->cpu = ARM_CPU(cpuobj);
    object_property_set_link(cpuobj, "memory", OBJECT(sysmem), &error_abort);
    object_property_set_bool(cpuobj, "has_el3", false, NULL);
    object_property_set_bool(cpuobj, "has_el2", false, NULL);
    object_property_set_bool(cpuobj, "realized", true, &error_fatal);
    define_arm_cp_regs(s->cpu, ipad1_cp_reginfo);
    object_unref(cpuobj);

    memory_region_init_ram(&s->dram, NULL, "ipad1.dram", S5L8930_DRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, S5L8930_DRAM_BASE, &s->dram);
    /*
     * iBoot is linked at 0x5ff00000 and puts its framebuffer at 0x5f700000,
     * i.e. DRAM seen through a second window 256 MiB up. Not yet confirmed on
     * hardware (an iBEC read of 0x4ff00000 hung), but iBoot needs it.
     */
    memory_region_init_alias(&s->dram_hi, NULL, "ipad1.dram-hi", &s->dram, 0,
                             S5L8930_DRAM_SIZE);
    memory_region_add_subregion(sysmem, S5L8930_DRAM_BASE + S5L8930_DRAM_SIZE,
                                &s->dram_hi);
    memory_region_init_ram(&s->sram, NULL, "ipad1.sram", S5L8930_SRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, S5L8930_SRAM_BASE, &s->sram);

    /*
     * Everything in the peripheral window that has no model yet. Real devices
     * added below sit on top of this at higher priority.
     */
    create_unimplemented_device("s5l8930.periph", 0x80000000, 0x40000000);

    if (s->bootrom_path) {
        g_autofree char *rom = NULL;
        gsize size;
        g_autoptr(GError) error = NULL;

        if (!g_file_get_contents(s->bootrom_path, &rom, &size, &error)) {
            error_report("ipad1: cannot read SecureROM: %s", error->message);
            exit(1);
        }
        if (size != 0x10000) {
            error_report("ipad1: A4 SecureROM must be exactly 65536 bytes");
            exit(1);
        }
        memory_region_init_rom(&s->bootrom, NULL, "ipad1.bootrom",
                               size, &error_fatal);
        memcpy(memory_region_get_ram_ptr(&s->bootrom), rom, size);
        memory_region_add_subregion(sysmem, 0, &s->bootrom);
        memory_region_init_alias(&s->bootrom_alias, NULL,
                                 "ipad1.bootrom-alias", &s->bootrom, 0, size);
        memory_region_add_subregion(sysmem, 0xbf000000, &s->bootrom_alias);
    }

    /*
     * ChipID fuses as a real K48AP reads them (docs/ipad1/hw1-probes.log):
     * chip/revision words, then the unit's die-id. The die-id is per unit, so
     * it comes from the die-id machine property ("0xWORD2:0xWORD3", runners
     * take it from identity.json); zeros otherwise.
     */
    {
        uint32_t chipid[] = { 0x31800387, 0x80758000, 0, 0 };

        if (s->development_fuses) {
            /* Engineering security policy: development GID/certificates,
             * no per-device ECID personalization requirement. The ROM still
             * verifies the certificate and image signatures itself. */
            chipid[0] &= ~((1u << 0) | (1u << 7));
        }

        if (s->die_id && sscanf(s->die_id, "%" SCNx32 ":%" SCNx32,
                                &chipid[2], &chipid[3]) != 2) {
            error_report("ipad1: die-id must be \"0xWORD2:0xWORD3\", got \"%s\"",
                         s->die_id);
            exit(1);
        }
        memory_region_init_rom(&s->chipid, NULL, "ipad1.chipid", 0x1000,
                               &error_fatal);
        memcpy(memory_region_get_ram_ptr(&s->chipid), chipid, sizeof(chipid));
        memory_region_add_subregion(sysmem, S5L8930_CHIPID_BASE, &s->chipid);
    }

    /* The kernel maps the cpu-debug-interface but never writes it at boot. */
    memory_region_init_ram(&s->cpu_debug, NULL, "ipad1.cpu-debug", 0x1000,
                           &error_fatal);
    memory_region_add_subregion(sysmem, S5L8930_CPU_DEBUG_BASE, &s->cpu_debug);

    /*
     * Four PL192s, daisy-chained VIC3 -> VIC2 -> VIC1 -> VIC0 -> CPU. The kernel
     * reads VIC0's VECTADDR and expects the chained vectors to come through it.
     */
    s->vic[0] = pl192_manual_init((char *)"vic0",
                                  qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ),
                                  qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ),
                                  NULL);
    memory_region_add_subregion(sysmem, S5L8930_VIC_BASE(0),
                                &PL192(s->vic[0])->iomem);
    for (i = 1; i < S5L8930_VIC_COUNT; i++) {
        g_autofree char *name = g_strdup_printf("vic%d", i);
        s->vic[i] = pl192_manual_init(name, NULL);
        memory_region_add_subregion(sysmem, S5L8930_VIC_BASE(i),
                                    &PL192(s->vic[i])->iomem);
        PL192(s->vic[i])->daisy = PL192(s->vic[i - 1]);
    }
    if (s->iop_core) {
        s->iopcore = qdev_new("s5l8930.iop-core");
        object_property_set_link(OBJECT(s->iopcore), "dram", OBJECT(&s->dram), &error_abort);
        object_property_set_link(OBJECT(s->iopcore), "sysmem", OBJECT(sysmem), &error_abort);
        qdev_realize_and_unref(s->iopcore, NULL, &error_fatal);
    }

    sysbus_create_simple("s5l8930.dmc", 0xbf800000, NULL);

    dev = qdev_new(TYPE_S5L8930_PMGR);
    sbd = SYS_BUS_DEVICE(dev);
    /*
     * POWER_ID[31:24] is the boot security epoch LLB writes: the CHIPID fuse
     * field floored at the build's epoch, which iBoot's miu_init demands back
     * ("Epoch Mismatch"). iboot= starts past LLB, so write what the matching
     * LLB would (same floor as its iBoot), read off the image. bootrom= runs
     * LLB, and kboot= no iBoot: both keep the measured byte.
     */
    if (s->iboot_path) {
        g_autofree gchar *img = NULL;
        gsize len = 0;
        uint32_t fuse = (ldl_le_p(memory_region_get_ram_ptr(&s->chipid)) >> 9) & 0x7f;
        uint32_t epoch = 0;

        if (g_file_get_contents(s->iboot_path, &img, &len, NULL)) {
            epoch = it_iboot_find_miu_epoch((const uint8_t *)img, len, fuse);
        }
        if (!epoch || epoch > 0xff) {
            warn_report("ipad1: no security epoch found in iBoot '%s'; "
                        "POWER_ID keeps the measured epoch 1", s->iboot_path);
        } else {
            qdev_prop_set_uint8(dev, "security-epoch", epoch);
        }
    }
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_PMGR_BASE);
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_TIMER0));
    sysbus_connect_irq(sbd, 1, ipad1_irq(s, S5L8930_IRQ_TIMER1));

    dev = qdev_new(TYPE_S5L8930_GPIO);
    s->gpio = dev;
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_GPIO_BASE);
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_GPIO));

    /* I2C0 carries the D1815 PMU; its interrupt is GPIO pin 0x0D, active low. */
    dev = qdev_new(TYPE_S5L8930_I2C);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_I2C_BASE(0));
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_I2C(0)));
    {
        I2CBus *bus = I2C_BUS(qdev_get_child_bus(dev, "i2c"));
        DeviceState *pmu = DEVICE(i2c_slave_create_simple(bus, TYPE_S5L8930_D1815, 0x74));
        s->pmu = pmu;
        DeviceState *xp = DEVICE(i2c_slave_create_simple(bus, TYPE_S5L8930_TCA6408, 0x20));
        s->ltc = DEVICE(i2c_slave_create_simple(bus, TYPE_S5L8930_LTC4099, 0x09));
        s5l8930_ltc4099_set_usb(s->ltc, s->usb_cable);
        qdev_connect_gpio_out(pmu, 0,
                              qemu_irq_invert(qdev_get_gpio_in(s->gpio, 0x0d)));
        qdev_connect_gpio_out(xp, 0,
                              qemu_irq_invert(qdev_get_gpio_in(s->gpio, 0x11)));
        /*
         * CS42L61 codec (i2c0/audio0). AppleCS42L61Audio treats it as a plain
         * MAP-addressed register file (0x01-0x6f, read back for its register
         * dump) and never checks the chip ID, so the iPod's CS42L58 model
         * fits unchanged. Its MCLK comes from the PWM block, which stays in
         * the unimplemented window.
         */
        i2c_slave_create_simple(bus, TYPE_CS42L58, 0x4a);
        /*
         * AK8973 magnetometer (DT i2c0/compass, 0x1e). The DT also lists a
         * compass1 at the same address on i2c2 for the other board build;
         * with nothing there its probe fails its reset check, as on a unit
         * of this build. qom-set /machine compass-heading N (degrees).
         */
        {
            s->compass =
                DEVICE(i2c_slave_create_simple(bus, TYPE_S5L8930_AK8973, 0x1e));
            object_property_add_alias(OBJECT(machine), "compass-heading",
                                      OBJECT(s->compass), "heading");
        }
        /*
         * CD3282 "Mikey" headset controller (i2c0/mikey). AppleCS42L61Audio
         * resolves the codec's 'mikey' platform function during its power-up
         * (c08213d4 -> c082273c) and waits until AppleCD3282Mikey provides
         * it, so without this slave the codec never registers its "Codec"
         * IOAudio2 device and mediaserverd fails every sound with '!dev'.
         * The iPod's CD3272 model (all registers read 0: nothing plugged in)
         * is enough for the driver to start.
         */
        i2c_slave_create_simple(bus, TYPE_CD3272MIKEY, 0x39);
    }
    /*
     * I2C2: LIS331DLH accelerometer and TSL2581 light sensor. AppleLIS331DLH
     * (7B500 c0895000-c0895c00) probes WHO_AM_I without checking the value,
     * writes CTRL_REG1-4 and INT1_CFG/THS/DURATION, and reads OUT_X/Y/Z as
     * 16-bit bursts (sub-address | 0x80); the iPod LIS302DL model does all of
     * that once told the part's WHO_AM_I. The DT interrupt pins (0x24/0x26,
     * 0x25) are not driven: nothing before userland waits on them.
     */
    dev = qdev_new(TYPE_S5L8930_I2C);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_I2C_BASE(2));
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_I2C(2)));
    {
        I2CBus *bus = I2C_BUS(qdev_get_child_bus(dev, "i2c"));
        I2CSlave *accel = i2c_slave_new(TYPE_LIS302DL, 0x19);

        qdev_prop_set_uint8(DEVICE(accel), "whoami", 0x32);
        /* On the iPad the LIS331 sits turned 180 degrees about X relative to
         * the iPod's mounting: the iPod vectors read with Y (and Z) negated.
         * Without this, "portrait" (1) read as upside down (SpringBoard's
         * interface orientation 2) and portrait-only iPhone apps drew
         * upside down; checked against springboardservices: upright reads 1
         * and a clockwise turn 4, then 2, then 3, as on hardware. */
        qdev_prop_set_bit(DEVICE(accel), "mount-flipped", true);
        i2c_slave_realize_and_unref(accel, bus, &error_fatal);
        s->accel = LIS302DL(accel);
        lis302dl_apply_orientation(s->accel, 1);   /* init ran before mount-flipped */
        s5l8930_ak8973_set_accel(s->compass, s->accel);
        /* Same names as the iPod machine: UIDeviceOrientation 0-6, e.g.
         * qom-set path=/machine property=accel-orientation value=3; raw
         * counts; a shake. accel-pitch/-roll/-pose are machine properties. */
        object_property_add_alias(OBJECT(machine), "accel-orientation",
                                  OBJECT(accel), "orientation");
        object_property_set_description(OBJECT(machine), "accel-orientation",
            "UIDeviceOrientation 1-6 (1 portrait, 2 upside down, 3 landscape left = Home right, 4 landscape right = Home left)");
        object_property_add_alias(OBJECT(machine), "accel-x", OBJECT(accel), "x");
        object_property_add_alias(OBJECT(machine), "accel-y", OBJECT(accel), "y");
        object_property_add_alias(OBJECT(machine), "accel-z", OBJECT(accel), "z");
        object_property_add_alias(OBJECT(machine), "accel-shake", OBJECT(accel), "shake");
        i2c_slave_create_simple(bus, TYPE_S5L8930_TSL2581, 0x39);
    }

    /* Display pipe, CLCD, DART2, RGBOUT, TV-out; scanout starts at iBoot's FB. */
    dev = qdev_new(TYPE_S5L8930_DISPLAY);
    s->display = dev;
    qdev_prop_set_uint64(dev, "fb-base", 0x4f700000);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_DISP_PIPE0_BASE);
    sysbus_mmio_map(sbd, 1, S5L8930_CLCD_BASE);
    sysbus_mmio_map(sbd, 2, S5L8930_DART2_BASE);
    sysbus_mmio_map(sbd, 3, S5L8930_RGBOUT_BASE);
    sysbus_mmio_map(sbd, 4, S5L8930_TVOUT_BASE);
    sysbus_mmio_map(sbd, 5, S5L8930_RGBOUT2_BASE);
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_DISP_PIPE0));
    sysbus_connect_irq(sbd, 1, ipad1_irq(s, S5L8930_IRQ_CLCD));

    /* MIPI-DSIM: the same Samsung IP as the iPod's; reuse that model. */
    dev = qdev_new(TYPE_IPOD_TOUCH_MIPI_DSI);
    IPOD_TOUCH_MIPI_DSI(dev)->direct_boot = true;
    /* kboot= enters the kernel with no iBoot to bring the panel up: start the
     * link the way iBoot's pinot_init leaves it (HS clock running). */
    IPOD_TOUCH_MIPI_DSI(dev)->hs_clock_at_reset = s->kboot_path != NULL;
    qdev_prop_set_uint32(dev, "lanes", 4);      /* K48 DT #lanes */
    memory_region_add_subregion(sysmem, S5L8930_DSIM_BASE,
                                &IPOD_TOUCH_MIPI_DSI(dev)->iomem);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    /*
     * Wi-Fi: the iPod's Broadcom dongle model, dressed as the unit's BCM4329
     * (K48 USI board: the CIS strings pick AppleBCMWLAN's "K48 USI X17B"
     * personality), behind the SDHC and the IOP's SDIO task. Frames go to
     * -netdev ...,id=wifi0. docs/ipad1/wifi.md.
     */
    DeviceState *sdio = NULL;
    if (s->wifi) {
        static const BCMSDIOChip bcm4329 = {
            .manfid = 0x02d0, .prodid = 0x4329,
            .chipid = 0x00034329,                   /* rev 3 = B1 (c07a61d2) */
            .sdiod_base = 0x18011000,               /* where initDongle polls */
            .vers1 = { "", "", "s=B1", "P=K48 m=u80" },
            .mac = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 },  /* = DT */
            .no_common_funce = true,
            /* what the K48 image in wifiFirmwareLoader reports */
            .fw_version = "wl0: Jul 21 2010 21:58:50 version 4.218.175.43",
        };
        IPodTouchSDIOState *card = IPOD_TOUCH_SDIO(qdev_new(TYPE_IPOD_TOUCH_SDIO));

        ipod_touch_sdio_set_chip(card, &bcm4329);
        /* qom-set /machine wifi-bssid aa:bb:..: a new access point for
         * locationd, which caches a position per BSSID (location.md). */
        object_property_add_alias(OBJECT(machine), "wifi-bssid", OBJECT(card), "bssid");
        card->card_present = true;
        sysbus_realize_and_unref(SYS_BUS_DEVICE(card), &error_fatal);
        if (!qemu_find_netdev("wifi0")) {
            /* Wi-Fi is the iPad's network: with no backend given, NAT it. */
            QemuOpts *o = qemu_opts_parse_noisily(qemu_find_opts("netdev"),
                                                  "type=user,id=wifi0", false);
            Error *err = NULL;
            if (o) {
                netdev_add(o, &err);
            }
            if (err) {
                warn_reportf_err(err, "Wi-Fi has no network: ");
            }
        }
        ipod_touch_sdio_setup_net(card);

        sdio = qdev_new(TYPE_S5L8930_SDIO);
        object_property_set_link(OBJECT(sdio), "card", OBJECT(card), &error_fatal);
        sbd = SYS_BUS_DEVICE(sdio);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, S5L8930_SDIO_BASE);
        sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_SDIO));
        sysbus_connect_irq(SYS_BUS_DEVICE(card), 0, qdev_get_gpio_in(sdio, 0));
    }

    /*
     * IOP: high-level emulation of the second core. No IRQ line: like the real
     * firmware, the model raises the AP by writing VIC0's SOFTINT register.
     */
    dev = qdev_new(TYPE_S5L8930_IOP);
    iop = dev;
    if (sdio) {
        object_property_set_link(OBJECT(dev), "sdio", OBJECT(sdio), &error_fatal);
    }
    if (s->nand_path) {
        qdev_prop_set_string(dev, "nand", s->nand_path);
    }
    if (s->nand_overlay_path) {
        qdev_prop_set_string(dev, "nand-overlay", s->nand_overlay_path);
    }
    if (s->iopcore) {
        object_property_set_link(OBJECT(dev), "core", OBJECT(s->iopcore), &error_fatal);
    }
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_IOP_BASE);
    if (!s->iopcore) {   /* with the core, real VICs sit at 0xbf300000 */
        sysbus_mmio_map(sbd, 1, S5L8930_IOP_VIC_BASE);
    }

    /* SHA-1 engine; CDMA channel 4 streams the data into its FIFO. */
    dev = qdev_new(TYPE_S5L8930_SHA1);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_SHA1_BASE);

    /*
     * PKE (RSA) at 0x83100000: the same engine and driver shape as the
     * S5L8720's, so the iPod model is reused. Only iBoot uses it (img3
     * signatures); it computes exactly what it is asked.
     */
    dev = qdev_new(TYPE_IPOD_TOUCH_PKE);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(sysmem, S5L8930_PKE_BASE,
                                &IPOD_TOUCH_PKE(dev)->iomem);

    /* CDMA + AES filter; one interrupt line per channel. */
    dev = qdev_new(TYPE_S5L8930_CDMA);
    if (s->gid_blobs_path) {
        qdev_prop_set_string(dev, "gid-blobs", s->gid_blobs_path);
    }
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_CDMA_BASE);
    sysbus_mmio_map(sbd, 1, S5L8930_AES_BASE);
    for (i = 0; i < S5L8930_CDMA_CHANNELS; i++) {
        sysbus_connect_irq(sbd, i, ipad1_irq(s, S5L8930_IRQ_CDMA(i)));
    }

    /* H2FMI: iBoot's own NAND path, reading the IOP's page store. */
    {
        DeviceState *cdma = dev;

        dev = qdev_new(TYPE_S5L8930_H2FMI);
        object_property_set_link(OBJECT(dev), "iop", OBJECT(iop), &error_abort);
        object_property_set_link(OBJECT(dev), "cdma", OBJECT(cdma), &error_abort);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        for (i = 0; i < 2; i++) {
            sysbus_mmio_map(sbd, i, S5L8930_H2FMI_BASE + i * 0x100000);
            sysbus_connect_irq(sbd, i, ipad1_irq(s, S5L8930_IRQ_FMI(i)));
        }
    }
    /*
     * USB device mode: the same Synopsys DWC OTG core and PHY register layout
     * as the S5L8720 (gap-kernel-platform-mmio.md §6), so both iPod models are
     * reused unchanged. The host bridge (usbmuxd-qemu) is dialled from
     * usb-tcp-addr, or IT_USB_TCP=host:port when that is unset. Without a
     * host the guest would never be configured, the power source would see
     * < 500 mA and let the device deep-sleep a few minutes after SpringBoard,
     * so without a bridge the OTG's built-in host enumerates it; either way
     * it behaves like an iPad on a Mac (charging, idle sleep disabled).
     */
    dev = qdev_new(TYPE_IPOD_TOUCH_USB_PHYS);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_USB_PHY_BASE);

    dev = ipod_touch_init_usb_otg(ipad1_irq(s, S5L8930_IRQ_USB_OTG),
                                  s5l8930_usb_hwcfg);
    s->usb_otg = S5L8900USBOTG(dev);
    if (s->usb_tcp_addr && s->usb_tcp_addr[0]) {
        char *colon = strrchr(s->usb_tcp_addr, ':');

        s->usb_otg->server_port = colon ? atoi(colon + 1) : 0;
        if (!s->usb_otg->server_port) {
            s->usb_otg->server_port = 1235;
        }
        s->usb_otg->server_host = colon && colon > s->usb_tcp_addr
            ? g_strndup(s->usb_tcp_addr, colon - s->usb_tcp_addr)
            : g_strdup("127.0.0.1");
    }
    /* No bridge: a built-in host enumerates and configures the device, which
     * is what keeps an iPad on a Mac charging and out of deep sleep. */
    s->usb_otg->builtin_host = !s->usb_otg->server_host && !getenv("IT_USB_TCP");
    s->usb_otg->host_charge = s->usb_charger;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(sysmem, S5L8930_USB_OTG_BASE,
                                &S5L8900USBOTG(dev)->iomem);
    /*
     * USB host, next to device mode: the DT we build gives usb-complex
     * "hsic-enabled", which makes AppleS5L8930XUSBArbitrator publish the host
     * nubs (EHCI, OHCI0) at start and never tear them down on cable changes,
     * so usbmux and a USB keyboard coexist (docs/ipad1/usb-keyboard.md).
     * AppleUSBEHCIARM/AppleUSBOHCIARM are plain EHCI/OHCI; capabilities at +0.
     * Its USB_CTL read-modify-writes (0xbf108000 bit 0 host, bit 2 HSIC)
     * stay in the unimp window. Attach with -device usb-kbd,bus=ehci.0.
     */
    dev = qdev_new(TYPE_EXYNOS4210_EHCI);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_USB_EHCI_BASE);
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_USB_EHCI));

    dev = qdev_new(TYPE_SYSBUS_OHCI);
    qdev_prop_set_uint32(dev, "num-ports", 1);      /* DT rh-ports */
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_USB_OHCI0_BASE);
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_USB_OHCI0));

    /*
     * SPI: the K48 kernel drives these with the same AppleS5L8900X SPI kext
     * as the iPod, so the iPod controller model is reused; the peripheral on
     * each bus is its "peripheral" property.
     */
    dev = ipod_touch_spi_create(S5L8930_SPI_BASE(0), ipad1_irq(s, S5L8930_IRQ_SPI(0)), 0, "nor", false);
    IPOD_TOUCH_SPI(dev)->nor->nor_path = s->nor_path;
    if (s->nor_rw_path && s->nor_rw_path[0]) {
        /* Guest NOR writes (the effaceable region: format, lockers, keybag key)
         * persist to this private copy across boots; base nor= is optional. */
        ipod_touch_nor_spi_open_overlay(IPOD_TOUCH_SPI(dev)->nor, s->nor_rw_path,
                                        &error_fatal);
    }
    /* NOR chip select is GPIO 0x505 (function-spi_cs0), driven by the kernel. */
    qdev_connect_gpio_out(s->gpio, S5L8930_GPIO_PIN(S5L8930_GPIO_NOR_CS),
        qdev_get_gpio_in_named(DEVICE(IPOD_TOUCH_SPI(dev)->nor), SSI_GPIO_CS, 0));

    dev = ipod_touch_spi_create(S5L8930_SPI_BASE(1), ipad1_irq(s, S5L8930_IRQ_SPI(1)), 1, "multitouch", false);
    s->mt = IPOD_TOUCH_SPI(dev)->mt;
    s->mt->profile = &mt_profile_k48;
    /* Zephyr2 ATN -> GPIO 0x15; reset (0x204) and download (0x107) are ignored. */
    qdev_connect_gpio_out_named(DEVICE(s->mt), "atn", 0,
        qdev_get_gpio_in(s->gpio, S5L8930_GPIO_PIN(S5L8930_GPIO_MT_ATN)));
    qemu_add_mouse_event_handler(ipad1_mouse_event, s->mt, 1, "iPad Touchscreen");
    qemu_input_handler_register(s->gpio, &ipad1_kbd_handler);
    qemu_input_handler_register(s->gpio, &ipad1_mtt_handler);

    /*
     * SPI2 is the baseband link. The Wi-Fi iPad has the controller but no
     * baseband, and the real unit's IORegistry still shows
     * AppleS5L8920XBasebandSPIController/BasebandSPIDevice loaded on it, so
     * model exactly that: a controller with nothing on the bus (reads return
     * 0). Its DT interrupt is the SRDY GPIO, not a VIC line, so none is wired.
     */
    ipod_touch_spi_create(S5L8930_SPI_BASE(2), NULL, 2, "none", false);

    /*
     * M2 scaler/CSC: the iPod's (same scaler,s5l8720x driver). Absent, its
     * reset (+0x10 |= 1, then poll for bit 0) never completed, and turning
     * on Accessibility > Zoom, which puts the scaler on CA's display path,
     * hung the UI in "M2Scaler waiting for device reset step 2".
     */
    ipod_scaler_set_iommu(sysbus_create_simple("ipodtouch.scaler",
                                               S5L8930_SCALER_BASE,
                                               ipad1_irq(s, S5L8930_IRQ_SCALER)),
                          s5l8930_dart2_xlate, s->display, 2);

    /* SWI: backlight and DPSM core voltage; only the busy bit matters. */
    sysbus_create_simple("ipodtouch.swi", S5L8930_SWI_BASE, NULL);

    /*
     * I2S0-2. i2s0 carries the CS42L61 codec's PCM from CDMA channel 0x1a to
     * the host audio backend; i2s1 (voice) and i2s2 (baseband) are register
     * files whose FIFO data is dropped. The CDMA model paces all three.
     */
    for (i = 0; i < 3; i++) {
        dev = qdev_new(TYPE_S5L8930_I2S);
        qdev_prop_set_bit(dev, "audio-out", i == 0);
        qdev_prop_set_uint8(dev, "port", i);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, S5L8930_I2S_BASE(i));
    }

    /*
     * AMC (audio media codec, amc,s5l8920x): the same AppleAMC_r2 kext family
     * the iPod's 3.1.3 drives, one hardware revision up ("AMC 2.1"). Reuse the
     * iPod model's register/interrupt handshake; its buffer aperture is the
     * 256 KiB at 0x84000000 that the machine already backs as SRAM. The third
     * DT window (0x84300000, 0x5000) stays unimplemented; UI sounds and PCM
     * playback never reach the AMC (it is the hardware decode transformer).
     */
    dev = qdev_new(TYPE_IPOD_TOUCH_AMC);
    qdev_prop_set_uint64(dev, "buf-base", S5L8930_SRAM_BASE);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_AMC_BASE);
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_AMC));

    /* Same Samsung UART as the S5L8720, including its interrupt scheme. */
    exynos4210_uart_create(S5L8930_UART_BASE(0), 256, 0, serial_hd(0),
                           ipad1_irq(s, S5L8930_IRQ_UART(0)), true);
    /* UART1, 2, 4: iBoot sets them all up; the kernel's DT has them too
     * (UART3 is Bluetooth, below). */
    for (i = 1; i < 5; i++) {
        if (i == 3) {
            continue;
        }
        exynos4210_uart_create(S5L8930_UART_BASE(i), 256, i, serial_hd(i),
                               ipad1_irq(s, S5L8930_IRQ_UART(i)), true);
    }
    /*
     * UART5 is the bq27545 gas gauge's HDQ line (see s5l8930_hdq.c). A 16-byte
     * FIFO like the silicon: AppleS5L8900XSerial reads the Rx count as
     * UFSTAT[3:0] | full(bit 8) << 4 (c068bafa), so a deeper FIFO's count in
     * [7:0] reads as empty once it passes 15 and the echoes were never read.
     */
    s->gauge = qemu_chardev_new(NULL, TYPE_CHARDEV_S5L8930_HDQ, NULL, NULL,
                                &error_abort);
    exynos4210_uart_create(S5L8930_UART_BASE(5), 16, 5, s->gauge,
                           ipad1_irq(s, S5L8930_IRQ_UART(5)), true);
    /* UART3: the BCM4329's HCI link (uart3/bluetooth,n88); nothing answers yet. */
    exynos4210_uart_create(S5L8930_UART_BASE(3), 256, 3, NULL,
                           ipad1_irq(s, S5L8930_IRQ_UART(3)), true);
    ipad1_battery_update(s);

    s->pwroff_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ipad1_pwroff_tick, s);
    qemu_register_powerdown_notifier(&ipad1_powerdown_notifier);
    qemu_register_reset(ipad1_cpu_reset, s);
}

static char *ipad1_get_kboot(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->kboot_path);
}

static void ipad1_set_kboot(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    g_free(s->kboot_path);
    s->kboot_path = g_strdup(value);
}

static char *ipad1_get_iboot(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->iboot_path);
}

static void ipad1_set_iboot(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    g_free(s->iboot_path);
    s->iboot_path = g_strdup(value);
}

static char *ipad1_get_bootrom(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->bootrom_path);
}

static void ipad1_set_bootrom(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    g_free(s->bootrom_path);
    s->bootrom_path = g_strdup(value);
}

static char *ipad1_get_gid_blobs(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->gid_blobs_path);
}

static void ipad1_set_gid_blobs(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    g_free(s->gid_blobs_path);
    s->gid_blobs_path = g_strdup(value);
}

static char *ipad1_get_nand(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->nand_path);
}

static void ipad1_set_nand(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    g_free(s->nand_path);
    s->nand_path = g_strdup(value);
}

static char *ipad1_get_nand_overlay(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->nand_overlay_path);
}

static void ipad1_set_nand_overlay(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    g_free(s->nand_overlay_path);
    s->nand_overlay_path = g_strdup(value);
}

static char *ipad1_get_die_id(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->die_id);
}

static void ipad1_set_die_id(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    g_free(s->die_id);
    s->die_id = g_strdup(value);
}

static char *ipad1_get_nor(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->nor_path);
}

static void ipad1_set_nor(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    g_free(s->nor_path);
    s->nor_path = g_strdup(value);
}

static char *ipad1_get_nor_rw(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->nor_rw_path);
}

static void ipad1_set_nor_rw(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    g_free(s->nor_rw_path);
    s->nor_rw_path = g_strdup(value);
}

static char *ipad1_get_usb_tcp_addr(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->usb_tcp_addr);
}

static void ipad1_set_usb_tcp_addr(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    g_free(s->usb_tcp_addr);
    s->usb_tcp_addr = g_strdup(value);
}

static bool ipad1_get_usb_cable(Object *obj, Error **errp)
{
    return IPAD1_MACHINE(obj)->usb_cable;
}

/* Plug/unplug at any time (qom-set /machine usb-cable off): the charger's
 * usb_det level flips and the PMU raises the cable event that makes the
 * power source and the USB arbitrator re-evaluate. */
static void ipad1_set_usb_cable(Object *obj, bool value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    if (s->usb_cable == value) {
        return;
    }
    s->usb_cable = value;
    if (s->ltc) {
        s5l8930_ltc4099_set_usb(s->ltc, value);
        synopsys_usb_set_cable(s->usb_otg, value);
        s5l8930_d1815_usb_cable_event(s->pmu);
    }
    ipad1_battery_update(s);
}

/*
 * --- battery -------------------------------------------------------------
 * What the status bar shows is IOPMPowerSource as configd's AppleHDQGasGauge
 * fills it from the bq27545 over HDQ (s5l8930_hdq.c), polled while running;
 * AppleD1815PMUPowerSource's boot estimate from the PMU battery voltage
 * (ADC mux 4: 3.90 V showed 63%, 3.525 V 2%, hence 3.50 V + 6.35 mV per %)
 * only covers the seconds before the plugin starts. So level and charge
 * state go to all three: gauge, PMU voltage, LTC4099 charge bits.
 *
 * Charging, as on hardware: configured at 500 mA an iPad 1 reads "Not
 * Charging"; it charges once the host grants more with Apple's vendor power
 * request (0x40/0x40, 500 + 1600 mA), as a Mac's high-power port does, and
 * AppleD1815PMUPowerSource logs "usb stack power 2100mA". usb-charger (default
 * on) makes the built-in host send it; a usbmuxd bridge sends it or not
 * itself. The gauge's charge state follows usb-charger; battery-charging
 * on/off overrides that.
 */
static bool ipad1_battery_charging(IPad1MachineState *s)
{
    return s->battery_mode == 1 ||
           (s->battery_mode == 0 && s->usb_cable && s->usb_charger);
}

static void ipad1_battery_update(IPad1MachineState *s)
{
    if (!s->pmu || !s->ltc || !s->gauge) {
        return;
    }
    s5l8930_d1815_set_vbat(s->pmu, 3500 + lround(s->battery_level * 6.35));
    s5l8930_ltc4099_set_charging(s->ltc, ipad1_battery_charging(s));
    s5l8930_hdq_set_battery(s->gauge, lround(s->battery_level), ipad1_battery_charging(s));
}

static bool ipad1_get_usb_charger(Object *obj, Error **errp)
{
    return IPAD1_MACHINE(obj)->usb_charger;
}

/* Takes effect for the built-in host at its next enumeration (cable replug). */
static void ipad1_set_usb_charger(Object *obj, bool value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    s->usb_charger = value;
    if (s->usb_otg) {
        s->usb_otg->host_charge = value;
    }
    ipad1_battery_update(s);
}

static void ipad1_get_battery_level(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    int64_t value = lround(IPAD1_MACHINE(obj)->battery_level);
    visit_type_int(v, name, &value, errp);
}

static void ipad1_set_battery_level(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }
    if (value < 0 || value > 100) {
        error_setg(errp, "battery-level must be between 0 and 100");
        return;
    }
    s->battery_level = value;
    ipad1_battery_update(s);
}

static void ipad1_get_battery_drain(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    visit_type_number(v, name, &IPAD1_MACHINE(obj)->battery_drain, errp);
}

static void ipad1_set_battery_drain(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);
    double value;

    if (!visit_type_number(v, name, &value, errp)) {
        return;
    }
    if (!isfinite(value) || value < 0 || value > 100) {
        error_setg(errp, "battery-drain must be between 0 and 100 percent per minute");
        return;
    }
    s->battery_drain = value;
}

static char *ipad1_get_battery_charging(Object *obj, Error **errp)
{
    return g_strdup(ipad1_battery_modes[IPAD1_MACHINE(obj)->battery_mode]);
}

static void ipad1_set_battery_charging(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    for (int i = 0; i < ARRAY_SIZE(ipad1_battery_modes); i++) {
        if (!strcmp(value, ipad1_battery_modes[i])) {
            s->battery_mode = i;
            ipad1_battery_update(s);
            return;
        }
    }
    error_setg(errp, "battery-charging must be auto, on or off");
}

/* --- tilt --------------------------------------------------------------- */

/*
 * The app's attitude (see hw/arm/ipod-attitude.h), in the device's frame;
 * the sensor's flipped mounting is applied by the LIS331 model itself.
 * ponytail: pitch and the flat pose are unverified on the iPad.
 */
static void ipad1_apply_attitude(IPad1MachineState *s)
{
    lis302dl_apply_attitude(s->accel, s->accel_pitch, s->accel_roll, s->accel_flat);
}

static void ipad1_get_accel_angle(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);
    double value = !strcmp(name, "accel-pitch") ? s->accel_pitch : s->accel_roll;

    visit_type_number(v, name, &value, errp);
}

static void ipad1_set_accel_angle(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);
    double value;

    if (!visit_type_number(v, name, &value, errp)) {
        return;
    }
    if (!isfinite(value) || value < -180 || value > 180) {
        error_setg(errp, "%s must be finite and between -180 and 180 degrees", name);
        return;
    }
    *(!strcmp(name, "accel-pitch") ? &s->accel_pitch : &s->accel_roll) = value;
    ipad1_apply_attitude(s);
}

static char *ipad1_get_accel_pose(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->accel_flat ? "flat" : "upright");
}

static void ipad1_set_accel_pose(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    if (strcmp(value, "flat") && strcmp(value, "upright")) {
        error_setg(errp, "accel-pose must be upright or flat");
        return;
    }
    s->accel_flat = !strcmp(value, "flat");
    ipad1_apply_attitude(s);
}

static bool ipad1_get_development_fuses(Object *obj, Error **errp)
{
    return IPAD1_MACHINE(obj)->development_fuses;
}

static void ipad1_set_development_fuses(Object *obj, bool value, Error **errp)
{
    IPAD1_MACHINE(obj)->development_fuses = value;
}

static bool ipad1_get_iop_core(Object *obj, Error **errp)
{
    return IPAD1_MACHINE(obj)->iop_core;
}

static void ipad1_set_iop_core(Object *obj, bool value, Error **errp)
{
    IPAD1_MACHINE(obj)->iop_core = value;
}

static bool ipad1_get_wifi(Object *obj, Error **errp)
{
    return IPAD1_MACHINE(obj)->wifi;
}

static void ipad1_set_wifi(Object *obj, bool value, Error **errp)
{
    IPAD1_MACHINE(obj)->wifi = value;
}

static bool ipad1_get_gles_debug(Object *obj, Error **errp)
{
    return IPAD1_MACHINE(obj)->gles_debug;
}

static void ipad1_set_gles_debug(Object *obj, bool value, Error **errp)
{
    IPAD1_MACHINE(obj)->gles_debug = value;
    gles_host_set_debug(value);
}

static char *ipad1_get_gles_rejects(Object *obj, Error **errp)
{
    return gles_host_rejects();
}

/* The iPod machine's agent properties (tests drive the agent over QMP with these). */
static void ipad1_set_agent_request(Object *obj, const char *value, Error **errp)
{
    int error = ipod_agent_submit(IPAD1_MACHINE(obj)->agent, value);
    if (error) {
        error_setg(errp, "%s", ipod_agent_submit_error(error));
    }
}

static void ipad1_cancel_agent_request(Object *obj, const char *value, Error **errp)
{
    ipod_agent_cancel(IPAD1_MACHINE(obj)->agent, value);
}

static char *ipad1_get_agent_result(Object *obj, Error **errp)
{
    return ipod_agent_take_result(IPAD1_MACHINE(obj)->agent);
}

static char *ipad1_get_agent_status(Object *obj, Error **errp)
{
    return g_strdup(ipod_agent_status(IPAD1_MACHINE(obj)->agent,
                                      qemu_clock_get_ms(QEMU_CLOCK_REALTIME)));
}

static void ipad1_instance_init(Object *obj)
{
    IPAD1_MACHINE(obj)->usb_cable = true;
    IPAD1_MACHINE(obj)->iop_core = true;
    IPAD1_MACHINE(obj)->wifi = true;
    guest_pb_init(&IPAD1_MACHINE(obj)->pb, obj, "ipad1");
    guest_pkg_init(&IPAD1_MACHINE(obj)->pkg, obj);
    IPAD1_MACHINE(obj)->agent = ipod_agent_new();
    ipod_agent_publish(IPAD1_MACHINE(obj)->agent);
    object_property_add_str(obj, "agent-request", NULL, ipad1_set_agent_request);
    object_property_add_str(obj, "agent-cancel", NULL, ipad1_cancel_agent_request);
    object_property_add_str(obj, "agent-result", ipad1_get_agent_result, NULL);
    object_property_add_str(obj, "agent-status", ipad1_get_agent_status, NULL);
    object_property_add_str(obj, "gles-rejects", ipad1_get_gles_rejects, NULL);
    object_property_set_description(obj, "gles-rejects",
        "Every refusal the GL bridge made so far, one NAME<tab>COUNT per line");
    IPAD1_MACHINE(obj)->battery_level = 80;
    IPAD1_MACHINE(obj)->usb_charger = true;
}

static void ipad1_instance_finalize(Object *obj)
{
    ipod_agent_publish(NULL);
    ipod_agent_free(IPAD1_MACHINE(obj)->agent);
    g_free(IPAD1_MACHINE(obj)->usb_tcp_addr);
    g_free(IPAD1_MACHINE(obj)->kboot_path);
    g_free(IPAD1_MACHINE(obj)->iboot_path);
    g_free(IPAD1_MACHINE(obj)->bootrom_path);
    g_free(IPAD1_MACHINE(obj)->gid_blobs_path);
    g_free(IPAD1_MACHINE(obj)->nand_path);
    g_free(IPAD1_MACHINE(obj)->nand_overlay_path);
    g_free(IPAD1_MACHINE(obj)->nor_path);
    g_free(IPAD1_MACHINE(obj)->nor_rw_path);
    g_free(IPAD1_MACHINE(obj)->die_id);
}

/*
 * Hold and Home as machine properties, for when a usb-kbd owns the host
 * keyboard and the Cmd chords no longer reach ipad1_kbd_event:
 *   qom-set path=/machine property=button-hold value=true   (then false)
 */
static bool ipad1_get_button_hold(Object *obj, Error **errp)
{
    return IPAD1_MACHINE(obj)->btn_hold;
}

static void ipad1_set_button_hold(Object *obj, bool value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    s->btn_hold = value;
    ipad1_set_button(s, S5L8930_GPIO_BTN_HOLD, value);
}

static bool ipad1_get_button_home(Object *obj, Error **errp)
{
    return IPAD1_MACHINE(obj)->btn_home;
}

static void ipad1_set_button_home(Object *obj, bool value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    s->btn_home = value;
    ipad1_set_button(s, S5L8930_GPIO_BTN_MENU, value);
}

static void ipad1_class_init(ObjectClass *klass, void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);

    mc->desc = "iPad 1 (K48AP, S5L8930)";
    mc->init = ipad1_init;
    /* The AP plus the IOP core: TCG sizes its contexts from smp, and the board creates
     * both CPUs itself, so 2 costs nothing with iop-core=off. */
    mc->max_cpus = 2;
    mc->default_cpus = 2;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a8");
    mc->default_ram_size = S5L8930_DRAM_SIZE;

    object_class_property_add_str(klass, "kboot", ipad1_get_kboot,
                                  ipad1_set_kboot);
    object_class_property_set_description(klass, "kboot",
        "K48KBOOT bundle from imgtools/ipad1_kboot.py (this or iboot required)");
    object_class_property_add_str(klass, "gid-blobs", ipad1_get_gid_blobs,
                                  ipad1_set_gid_blobs);
    object_class_property_set_description(klass, "gid-blobs",
        "Per-IPSW AES-256 KBAG || IV-key records (96 bytes each)");
    object_class_property_add_str(klass, "bootrom", ipad1_get_bootrom,
                                  ipad1_set_bootrom);
    object_class_property_set_description(klass, "bootrom",
        "A4 SecureROM dump (65536 bytes), entered at reset");
    object_class_property_add_str(klass, "iboot", ipad1_get_iboot,
                                  ipad1_set_iboot);
    object_class_property_set_description(klass, "iboot",
        "decrypted iBoot image, entered at 0x5ff00000 instead of kboot");
    object_class_property_add_str(klass, "nand", ipad1_get_nand, ipad1_set_nand);
    object_class_property_set_description(klass, "nand",
        "NAND page-store directory (geometry.json + bus<b>-ce<c>.pages); blank chips if unset");
    object_class_property_add_str(klass, "nand-overlay", ipad1_get_nand_overlay,
                                  ipad1_set_nand_overlay);
    object_class_property_set_description(klass, "nand-overlay",
        "Copy-on-write directory for guest NAND writes; the nand store is then read-only");
    object_class_property_add_str(klass, "die-id", ipad1_get_die_id, ipad1_set_die_id);
    object_class_property_set_description(klass, "die-id",
        "the unit's ChipID die-id words 2-3, \"0xWORD2:0xWORD3\" (identity.json); zeros if unset");
    object_class_property_add_str(klass, "nor", ipad1_get_nor, ipad1_set_nor);
    object_class_property_set_description(klass, "nor",
        "1 MiB SPI NOR image (nvram, syscfg); erased flash if unset");
    object_class_property_add_str(klass, "nor-rw", ipad1_get_nor_rw, ipad1_set_nor_rw);
    object_class_property_set_description(klass, "nor-rw",
        "1 MiB private writable NOR copy; guest writes (effaceable) persist here across boots");
    object_class_property_add_str(klass, "usb-tcp-addr", ipad1_get_usb_tcp_addr,
                                  ipad1_set_usb_tcp_addr);
    object_class_property_set_description(klass, "usb-tcp-addr",
        "usbmuxd-qemu host bridge host:port (default port 1235); unset = IT_USB_TCP or no link");
    object_class_property_add_bool(klass, "development-fuses",
                                  ipad1_get_development_fuses,
                                  ipad1_set_development_fuses);
    object_class_property_set_description(klass, "development-fuses",
        "engineering production/ECID fuse policy for unpersonalized IPSW images (default off)");
    object_class_property_add_bool(klass, "iop-core", ipad1_get_iop_core, ipad1_set_iop_core);
    object_class_property_set_description(klass, "iop-core",
        "Run the kernel's EmbeddedIOP firmware on a second core (arm946) (default on); off = the IOP HLE");
    object_class_property_add_bool(klass, "wifi", ipad1_get_wifi, ipad1_set_wifi);
    object_class_property_set_description(klass, "wifi",
        "The BCM4329 Wi-Fi card, the iPad's network (default on). Frames go to "
        "-netdev id=wifi0, or to user networking when none is given; off = no card");
    object_class_property_add_bool(klass, "gles-debug", ipad1_get_gles_debug, ipad1_set_gles_debug);
    object_class_property_set_description(klass, "gles-debug",
        "Paint what the GL bridge refuses magenta instead of black (default off; tests turn it on)");
    object_class_property_add_bool(klass, "usb-cable", ipad1_get_usb_cable,
                                   ipad1_set_usb_cable);
    object_class_property_set_description(klass, "usb-cable",
        "USB cable present (default on); settable at runtime to plug/unplug");
    /* The iPod machine's names, so the app bridge drives both unchanged. */
    object_class_property_add_bool(klass, "usb-attached", ipad1_get_usb_cable,
                                   ipad1_set_usb_cable);
    object_class_property_add_bool(klass, "usb-charger", ipad1_get_usb_charger,
                                   ipad1_set_usb_charger);
    object_class_property_set_description(klass, "usb-charger",
        "the built-in USB host grants a high-power port's 2.1 A, so the iPad charges (default on); off = 500 mA, \"Not Charging\"");
    object_class_property_add(klass, "battery-level", "int", ipad1_get_battery_level,
                              ipad1_set_battery_level, NULL, NULL);
    object_class_property_add(klass, "battery-drain", "number", ipad1_get_battery_drain,
                              ipad1_set_battery_drain, NULL, NULL);
    object_class_property_add_str(klass, "battery-charging", ipad1_get_battery_charging,
                                  ipad1_set_battery_charging);
    object_class_property_add(klass, "accel-pitch", "number", ipad1_get_accel_angle,
                              ipad1_set_accel_angle, NULL, NULL);
    object_class_property_add(klass, "accel-roll", "number", ipad1_get_accel_angle,
                              ipad1_set_accel_angle, NULL, NULL);
    object_class_property_add_str(klass, "accel-pose", ipad1_get_accel_pose,
                                  ipad1_set_accel_pose);
    object_class_property_add_bool(klass, "button-hold", ipad1_get_button_hold,
                                   ipad1_set_button_hold);
    object_class_property_set_description(klass, "button-hold",
        "Hold/power button pressed; set true then false");
    object_class_property_add_bool(klass, "button-home", ipad1_get_button_home,
                                   ipad1_set_button_home);
    object_class_property_set_description(klass, "button-home",
        "Home button pressed; set true then false");
}

static const TypeInfo ipad1_machine_info = {
    .name = TYPE_IPAD1_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(IPad1MachineState),
    .instance_init = ipad1_instance_init,
    .instance_finalize = ipad1_instance_finalize,
    .class_init = ipad1_class_init,
};

static void ipad1_machine_types(void)
{
    type_register_static(&ipad1_machine_info);
}

type_init(ipad1_machine_types)
