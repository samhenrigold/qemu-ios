/*
 * Apple S5L8950 (A6), iPhone 5 N42AP.
 *
 * Bring-up uses the original N42 kernel and device tree. Hardware addresses
 * and interrupt assignments come from that DT; no A4 board substitution.
 * Initial milestone: Swift execution, DRAM, AIC v1 and the native UART6.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/arm/ios-boot-args.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/or-irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/exynos4210.h"
#include "hw/arm/s5l8930.h"
#include "hw/arm/ipod_touch_buttons.h"
#include "hw/arm/ipod_touch_mipi_dsi.h"
#include "hw/arm/ipod_touch_spi.h"
#include "hw/arm/ipod_touch_usb_otg.h"
#include "hw/arm/ipod_touch_usb_phys.h"
#include "hw/i2c/i2c.h"
#include "hw/usb/hcd-ehci.h"
#include "hw/usb/hcd-ohci.h"
#include "hw/misc/ios_baseband.h"
#include "hw/usb/mdm9x15.h"
#include "hw/arm/machines-qom.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/rtc.h"
#include "qemu/cutils.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "hw/arm/guest-services/general.h"
#include "hw/arm/guest-services/gles.h"
#include "hw/arm/guest-package.h"
#include "hw/arm/guest-pasteboard.h"
#include "hw/arm/ipod-agent.h"
#include "qemu/guest-random.h"
#include "target/arm/cpregs.h"
#include "target/arm/arm-powerctl.h"
#include "chardev/char.h"
#include "ui/console.h"
#include "ui/input.h"
#include "trace.h"

#define TYPE_S5L8950_MACHINE MACHINE_TYPE_NAME("iPhone-5")
OBJECT_DECLARE_SIMPLE_TYPE(S5L8950State, S5L8950_MACHINE)
#define A6_RAM_BASE 0x80000000ULL
#define A6_RAM_SIZE 0x40000000ULL
#define A6_IRQS 192
#define A6_CDMA_CHANNELS 0x2c

/* N42.mtprops supplies C1F57,1 / firmware 0x0264. Sensor geometry below
 * describes the virtual panel; physical N42 electrode geometry is unmeasured.
 * frame_* converts that virtual surface to the coordinates consumed by the
 * original N42 driver. Measured from native UIKit's UITouch location setter
 * on 11A465: raw (1085,1258)->(80,441.5), (4421,1258)->(307.5,441.5),
 * (1335,6406)->(97,0 clipped), and (3804,2703)->(266.5,312), in points.
 * Fit the unclipped samples per axis, then refine against a nine-point grid;
 * the panel is 320 x 568 points. */
static const MTSensorProfile a6_mt_profile = {
    .family_id = 0x57,
    .bcd_version = 0x264,
    .rows = 15,
    .cols = 10,
    .surface_width = 5000,
    .surface_height = 8875,
    .region_desc_len = 1,
    .region_param_len = 1,
    .frame_x0 = -75,
    .frame_width = 4654,
    .frame_y0 = -169,
    .frame_height = 6374,
    .contact_flags = true,
};

static void a6_pmu_button(DeviceState *dev, bool hold);
static bool a6_pmu_take_suspend(DeviceState *dev);
static void a6_pmu_cancel_resume(DeviceState *dev);
static void a6_pmu_reassert(DeviceState *dev);

static void a6_mouse_event(void *opaque, int x, int y, int z, int buttons)
{
    IPodTouchMultitouchState *mt = opaque;
    mt->touch_x = x / 32768.0f;
    mt->touch_y = 1.0f - y / 32768.0f;
    if (buttons && !mt->touch_down) {
        ipod_touch_multitouch_on_touch(mt);
    } else if (!buttons && mt->touch_down) {
        ipod_touch_multitouch_on_release(mt);
    } else if (buttons) {
        ipod_touch_multitouch_on_motion(mt);
    }
}

/* A5/A6 use the PA Semi command-FIFO I2C block, rather than A4's block.
 * Register/command definitions are cross-checked against Linux's
 * drivers/i2c/busses/i2c-pasemi-core.c and the original N42 driver traffic. */
typedef struct A6I2C {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    I2CBus *bus;
    qemu_irq irq;
    uint32_t status, imask, ctl, timing;
    uint8_t rx[256];
    unsigned count, pos;
    bool nack;
} A6I2C;

static void a6_i2c_irq(A6I2C *s)
{
    uint32_t status = s->status | (s->pos < s->count ? 1u << 20 : 0);
    qemu_set_irq(s->irq, !!(status & s->imask));
}
static uint64_t a6_i2c_read(void *opaque, hwaddr off, unsigned size)
{
    A6I2C *s = opaque;
    switch (off) {
    case 4: {
        uint32_t data = s->pos < s->count ? s->rx[s->pos++] : 0x100;
        a6_i2c_irq(s);
        return data;
    }
    case 8:
        return (s->count - s->pos) << 16;
    case 0xc:
        return (s->count - s->pos) << 16;
    case 0x10:
        return s->timing;
    case 0x14:
        return s->status | (1u << 16) |
               (s->pos < s->count ? (1u << 19) | (1u << 20) : 0);
    case 0x18:
        return s->imask;
    case 0x1c:
        return s->ctl;
    case 0x28:
        return 4;
    default:
        return 0;
    }
}
static void a6_i2c_write(void *opaque, hwaddr off, uint64_t value,
                         unsigned size)
{
    A6I2C *s = opaque;
    switch (off) {
    case 0:
        if (value & 0x100) {
            s->nack =
                i2c_start_transfer(s->bus, (value & 0xff) >> 1, value & 1);
        } else if (value & 0x400) {
            s->pos = s->count = 0;
            if (!s->nack) {
                s->count = value & 0xff;
                for (unsigned i = 0; i < s->count; i++)
                    s->rx[i] = i2c_recv(s->bus);
            }
        } else if (!s->nack) {
            s->nack = i2c_send(s->bus, value & 0xff);
        }
        if (value & 0x200) {
            i2c_end_transfer(s->bus);
            s->status |= (1u << 27) | (s->nack ? 1u << 21 : 0);
        }
        break;
    case 0x10:
        s->timing = value;
        break;
    case 0x14:
        s->status &= ~value;
        break;
    case 0x18:
        s->imask = value;
        break;
    case 0x1c:
        s->ctl = value;
        if (value & 4) {
            s->pos = s->count = 0;
            s->nack = false;
        }
        break;
    }
    a6_i2c_irq(s);
}
static const MemoryRegionOps a6_i2c_ops = {
    .read = a6_i2c_read,
    .write = a6_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4},
};

struct S5L8950State {
    MachineState parent_obj;
    ARMCPU *cpu[2];
    ARMCPU *iop_cpu;
    MemoryRegion iop_mem, iop_dram, iop_phys_dram, iop_periph, iop_fw, iop_ctrl,
        iop_private;
    bool iop_fw_mapped;
    uint32_t iop_regs[0x1000 / 4];
    uint32_t local_timer[2][3];
    QEMUTimer *iop_timer;
    int64_t iop_timer_deadline;
    uint32_t iop_timer_control, iop_timer_reload, iop_timer_status;
    uint32_t iop_other_mask;
    uint32_t nand_dll_control;
    MemoryRegion ram, low_ram, sram, peripheral, aic;
    MemoryRegion ae2_sram;
    unsigned ae2_accesses;
    unsigned ae2_unimplemented_accesses;
    unsigned audio_accesses;
    ARMCPU *ae2_cpu;
    MemoryRegion ae2_mem, ae2_low_ram, ae2_dram, ae2_cached_dram;
    MemoryRegion ae2_peripheral, ae2_high_sram, ae2_unimplemented, ae2_control;
    MemoryRegion ae2_work_sram;
    MemoryRegion display_dma_fifo;
    bool ae2_fw_mapped;
    MemoryRegion ae2_irq_control;
    uint32_t ae2_pending, ae2_irq_enable, ae2_fiq_enable;
    uint32_t ae2_host_pending;
    bool ae2_wfi_seen;
    QEMUTimer *ae2_wfi_timer;
    uint32_t ae2_regs[0x2000 / 4];
    DeviceState *gpio;
    DeviceState *pmu;
    QEMUTimer *ap_off_timer;
    bool resume_boot;
    bool button_down[IPOD_TOUCH_BUTTON_VOLDOWN + 1];
    char *kboot, *nand, *restore_image;
    int restore_fd;
    uint64_t restore_size;
    bool ppn, display_probe, ae2_probe, pmu_adc_probe, tristar_probe,
        usb_host_probe;
    bool audio_ports_probe;
    bool camera;
    bool quiet_boot; /* omit -v in the RAM boot handoff */
    bool battery_probe;
    bool backlight_probe;
    bool hsic_ready_probe;
    bool baseband;
    DeviceState *baseband_usb;
    USBBus *hsic_bus;
    Chardev *gauge;
    uint32_t level[6], software[6], mask[6], destination[A6_IRQS];
    uint32_t global_config, ipi[2], ipi_mask[2];
    uint32_t pmgr[0x7000 / 4];
    A6I2C *i2c[3];
    unsigned unimplemented, unimplemented_iop;
    QemuConsole *console;
    uint32_t framebuffer;
    GuestPackage pkg;
    GuestPasteboard pb;
    IPodAgent *agent;
    uint64_t powerdown_sequence;
};

/* Native N42 DT handshake: HOST_READY GPIO2.1 /6.6, DEVICE_READY GPIO26.3
 * /7.1, EHCI ports2/4. An attached HSIC peer acknowledges host readiness;
 * an absent peer cannot assert DEVICE_READY. Wire both handshake levels
 * by default, as required by the older native HSIC state machine. */
static void a6_hsic_ready(void *opaque, int link, int level)
{
    S5L8950State *s = opaque;
    if (!link && s->baseband_usb) {
        qemu_set_irq(qdev_get_gpio_in_named(s->baseband_usb, "host-ready", 0),
                     level);
        return;
    }
    const char *path = link ? "4" : "2";
    bool attached = false;
    USBPort *port;
    if (s->hsic_bus) {
        QTAILQ_FOREACH(port, &s->hsic_bus->used, next)
        {
            if (!strcmp(port->path, path) && port->dev && port->dev->attached) {
                attached = true;
                break;
            }
        }
    }
    qemu_set_irq(qdev_get_gpio_in(s->gpio, link ? 57 : 211), level && attached);
}

/* N42 PMU GPIO10 is the active-low BCM4334 module reset. A powered,
 * attached peer may announce readiness after reset release before the AP
 * raises HOST_READY; subsequent host-ready transitions use the usual path. */
static void a6_wlan_reset(void *opaque, int pin, int level)
{
    S5L8950State *s = opaque;
    bool ready = false;
    USBPort *port;

    if (s->hsic_bus) {
        QTAILQ_FOREACH(port, &s->hsic_bus->used, next)
        {
            if (!strcmp(port->path, "4") && port->dev &&
                object_dynamic_cast(OBJECT(port->dev), "usb-bcm-hsic")) {
                qemu_set_irq(
                    qdev_get_gpio_in_named(DEVICE(port->dev), "reset", 0),
                    !level);
                ready = level && port->dev->attached;
                break;
            }
        }
    }
    qemu_set_irq(qdev_get_gpio_in(s->gpio, 57), ready);
}

/* The existing qemu-ios guest-services ABI, shared with the other machines.
 * Graphics uses the common host executor and request validation. The optional
 * factory-imaging transport supplies one read-only stream only when explicitly
 * configured; its writes use the guest's native block/FTL drivers. */
static void a6_qemu_call(CPUARMState *env, const ARMCPRegInfo *ri,
                         uint64_t value)
{
    S5L8950State *s = S5L8950_MACHINE(qdev_get_machine());
    CPUState *cpu = env_cpu(env);
    qemu_call_t q;
    if (qemu_call_rw(cpu, value, &q, false))
        return;
    q.retval = -1;
    q.error = QC_ERR_ENOSYS;
    if (q.call_number == QC_GLES || q.call_number == QC_GLES_PING) {
        q.retval = q.call_number == QC_GLES ? qc_handle_gles(cpu, &q.args.gles)
                                            : QC_GLES_PING_MAGIC;
        q.error = 0;
    } else if (q.call_number >= QC_AG_HELLO &&
               q.call_number <= QC_AG_UI_ROUTE) {
        uint64_t candidate = 0;
        if (q.call_number == QC_AG_HELLO || q.call_number == QC_AG_UI_ROUTE) {
            qemu_guest_getrandom_nofail(&candidate, sizeof(candidate));
        }
        q.retval = ipod_agent_call(
            s->agent, q.call_number, q.args.ag.token,
            q.args.ag.buffer_guest_ptr, q.args.ag.offset, q.args.ag.length,
            qemu_clock_get_ms(QEMU_CLOCK_REALTIME), candidate, guest_copy, cpu);
        q.error = q.retval < 0 ? EINVAL : 0;
    } else {
        int32_t err = 0;
        if (guest_pb_call(&s->pb, cpu, &q, &err) ||
            guest_pkg_call(&s->pkg, cpu, &q, &err)) {
            q.error = err;
        }
    }
    if (s->restore_image && s->restore_fd >= 0 && q.call_number == 0x500) {
        uint32_t n = q.args.ag.length;
        uint64_t off = q.args.ag.token;
        if (n && n <= 65536 && off <= s->restore_size &&
            n <= s->restore_size - off) {
            g_autofree uint8_t *buffer = g_malloc(n);
            ssize_t count = pread(s->restore_fd, buffer, n, off);
            if (count >= 0 && !guest_copy(cpu, q.args.ag.buffer_guest_ptr,
                                          buffer, count, true)) {
                q.retval = count;
                q.error = 0;
            } else
                q.error = 5;
        } else
            q.error = 22;
    }
    qemu_call_rw(cpu, value, &q, true);
}

static const ARMCPRegInfo a6_guest_cpregs[] = {
    {.name = "QEMU_CALL",
     .cp = 15,
     .opc1 = 3,
     .crn = 15,
     .crm = 15,
     .opc2 = 0,
     .access = PL0_RW,
     .state = ARM_CP_STATE_AA32,
     .type = ARM_CP_IO | ARM_CP_NO_RAW | ARM_CP_RAISES_EXC,
     .readfn = qemu_call_status,
     .writefn = a6_qemu_call},
};

static void a6_display_update(void *opaque)
{
    S5L8950State *s = opaque;
    if (!s->framebuffer)
        return;
    DisplaySurface *surface = qemu_console_surface(s->console);
    uint8_t *ram = memory_region_get_ram_ptr(&s->ram);
    for (int y = 0; y < 1136; y++) {
        memcpy(surface_data(surface) + y * surface_stride(surface),
               ram + s->framebuffer - A6_RAM_BASE + y * 640 * 4, 640 * 4);
    }
    dpy_gfx_update(s->console, 0, 0, 640, 1136);
}
static const GraphicHwOps a6_display_ops = {.gfx_update = a6_display_update};

static hwaddr a6_scaler_xlate(void *opaque, uint32_t va, unsigned sid)
{
    return s5l8930_dart_dev_xlate(opaque, sid, va);
}

static hwaddr a6_jpeg_xlate(void *opaque, unsigned sid, uint32_t va)
{
    return s5l8930_dart_dev_xlate(opaque, sid, va);
}

static void a6_irq_update(S5L8950State *s)
{
    if (s->iop_cpu) {
        bool pending = (s->iop_timer_status & 1) && !(s->iop_other_mask & 2);
        for (int irq = 0; irq < A6_IRQS; irq++) {
            uint32_t bit = 1u << (irq & 31);
            pending |= ((s->level[irq / 32] | s->software[irq / 32]) &
                        ~s->mask[irq / 32] & bit) &&
                       (s->destination[irq] & 4);
        }
        qemu_set_irq(qdev_get_gpio_in(DEVICE(s->iop_cpu), ARM_CPU_IRQ),
                     pending);
    }
    for (int cpu = 0; cpu < 2; cpu++) {
        bool pending = (s->ipi[cpu] & ~s->ipi_mask[cpu]) != 0;
        for (int irq = 0; irq < A6_IRQS; irq++) {
            uint32_t bit = 1u << (irq & 31);
            pending |= ((s->level[irq / 32] | s->software[irq / 32]) &
                        ~s->mask[irq / 32] & bit) &&
                       (s->destination[irq] & (1u << cpu));
        }
        qemu_set_irq(qdev_get_gpio_in(DEVICE(s->cpu[cpu]), ARM_CPU_IRQ),
                     pending);
    }
}

/* AIC and the AP generic timer share a clock. QEMU represents nominal
 * 24 MHz with an integer 41 ns period; using exact 24 MHz for AIC instead
 * makes the guest observe two counters drifting apart. Match the generic
 * timer's conversion and its QEMUTimer inverse, as ARM's timer model does. */
static unsigned a6_counter_period_ns(S5L8950State *s)
{
    return gt_cntfrq_period_ns(s->cpu[0]);
}

/* Original N42 IOP 247c programs a relative countdown at 2014 in the
 * same 24 MHz domain as the AIC counter (251c/2540). 2010 bit0 enables it,
 * 2018 bit0 is W1C, and other-interrupt mask bit1 gates event 70001.
 * The storage worker uses this timer for timed sleeps after reset. */
static void a6_iop_timer_expired(void *opaque)
{
    S5L8950State *s = opaque;
    s->iop_timer_status |= 1;
    trace_a6_iop_timer_log(s->iop_timer_control, s->iop_timer_reload,
                           s->iop_timer_status, s->iop_other_mask);
    a6_irq_update(s);
}

static void a6_iop_timer_schedule(S5L8950State *s)
{
    timer_del(s->iop_timer);
    if ((s->iop_timer_control & 1) && !(s->iop_timer_status & 1)) {
        uint64_t ns = (uint64_t)s->iop_timer_reload * a6_counter_period_ns(s);
        s->iop_timer_deadline =
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MAX(ns, 1);
        timer_mod(s->iop_timer, s->iop_timer_deadline);
    }
    a6_irq_update(s);
}

static bool a6_iop_timer_write(S5L8950State *s, hwaddr offset, uint32_t value)
{
    switch (offset) {
    case 0x2010:
        s->iop_timer_control = value;
        break;
    case 0x2014:
        s->iop_timer_reload = value;
        break;
    case 0x2018:
        s->iop_timer_status &= ~(value & 1);
        break;
    case 0x201c:
        s->iop_other_mask |= value;
        a6_irq_update(s);
        return true;
    case 0x2020:
        s->iop_other_mask &= ~value;
        a6_irq_update(s);
        return true;
    default:
        return false;
    }
    trace_a6_iop_timer_log(s->iop_timer_control, s->iop_timer_reload,
                           s->iop_timer_status, s->iop_other_mask);
    a6_iop_timer_schedule(s);
    return true;
}

static uint32_t a6_iop_timer_ack(S5L8950State *s)
{
    if ((s->iop_timer_status & 1) && !(s->iop_other_mask & 2)) {
        s->iop_other_mask |= 2;
        a6_irq_update(s);
        return 0x70001;
    }
    return 0;
}

static int a6_aic_cpu(S5L8950State *s, hwaddr *offset)
{
    if (*offset >= 0x5000 && *offset < 0x5100) {
        int cpu = (*offset - 0x5000) / 0x80;
        *offset = 0x2000 + ((*offset - 0x5000) % 0x80);
        return cpu;
    }
    return current_cpu == CPU(s->cpu[1]) ? 1 : 0;
}

static void a6_irq(void *opaque, int irq, int level)
{
    S5L8950State *s = opaque;
    uint32_t bit = 1u << (irq & 31);
    if (level)
        s->level[irq / 32] |= bit;
    else
        s->level[irq / 32] &= ~bit;
    a6_irq_update(s);
}

/* AIC v1's interrupt acknowledgement masks the selected source. A subsequent
 * MASK_CLR write rearms it; level sources remain pending until deasserted. */
static uint64_t a6_aic_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8950State *s = opaque;
    if (s->iop_cpu && current_cpu == CPU(s->iop_cpu)) {
        if (offset == 0x2000)
            return 2;
        if (offset == 0x2010)
            return s->iop_timer_control;
        if (offset == 0x2014) {
            int64_t ns =
                s->iop_timer_deadline - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            return (s->iop_timer_control & 1) && ns > 0
                       ? ns / a6_counter_period_ns(s)
                       : 0;
        }
        if (offset == 0x2018)
            return s->iop_timer_status;
        if (offset == 0x201c || offset == 0x2020)
            return s->iop_other_mask;
        if (offset == 0x2004) {
            for (int irq = 0; irq < A6_IRQS; irq++) {
                uint32_t bit = 1u << (irq & 31);
                if ((s->destination[irq] & 4) &&
                    ((s->level[irq / 32] | s->software[irq / 32]) &
                     ~s->mask[irq / 32] & bit)) {
                    if (irq == 3) {
                        trace_a6_iop_doorbell_log(
                            true, 0x2004, irq, s->software[0], s->mask[0],
                            s->level[0], s->destination[3]);
                    }
                    s->mask[irq / 32] |= bit;
                    a6_irq_update(s);
                    return 0x10000 | irq;
                }
            }
            return a6_iop_timer_ack(s);
        }
    }
    int cpu = a6_aic_cpu(s, &offset);
    if (offset == 0x20 || offset == 0x28) {
        uint64_t ticks =
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / a6_counter_period_ns(s);
        return offset == 0x20 ? (uint32_t)ticks : ticks >> 32;
    }
    if (offset == 0)
        return 1;
    if (offset == 4)
        return A6_IRQS;
    if (offset == 0x10)
        return s->global_config;
    if (offset == 0x2000)
        return cpu;
    if (offset >= 0x2010 && offset <= 0x2018)
        return s->local_timer[cpu][(offset - 0x2010) / 4];
    if (offset == 0x2020)
        return s->ipi_mask[cpu];
    if (offset == 0x2004) {
        for (int i = 0; i < 6; i++) {
            uint32_t pending = (s->level[i] | s->software[i]) & ~s->mask[i];
            for (int bit = 0; bit < 32; bit++)
                if ((pending & (1u << bit)) &&
                    (s->destination[i * 32 + bit] & (1u << cpu))) {
                    s->mask[i] |= 1u << bit;
                    a6_irq_update(s);
                    return 0x10000 | (i * 32 + bit);
                }
        }
        uint32_t ipi = s->ipi[cpu] & ~s->ipi_mask[cpu];
        if (ipi & 0x80000000u) {
            s->ipi_mask[cpu] |= 0x80000000u;
            a6_irq_update(s);
            return 0x40002;
        }
        if (ipi & 1) {
            s->ipi_mask[cpu] |= 1;
            a6_irq_update(s);
            return 0x40001;
        }
        return 0;
    }
    if (offset >= 0x3000 && offset < 0x3000 + A6_IRQS * 4)
        return s->destination[(offset - 0x3000) / 4];
    if (offset >= 0x4100 && offset < 0x4118)
        return s->mask[(offset - 0x4100) / 4];
    if (offset >= 0x4200 && offset < 0x4218)
        return s->level[(offset - 0x4200) / 4];
    qemu_log_mask(LOG_UNIMP, "A6 AIC read +0x%04" HWADDR_PRIx "\n", offset);
    return 0;
}

static void a6_aic_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    S5L8950State *s = opaque;
    if (current_cpu == CPU(s->iop_cpu) &&
        a6_iop_timer_write(s, offset, value)) {
        return;
    }
    int cpu = a6_aic_cpu(s, &offset);
    qemu_log_mask(CPU_LOG_INT,
                  "A6 AIC CPU%d write +0x%04" HWADDR_PRIx " = 0x%08" PRIx64
                  "\n",
                  cpu, offset, value);
    if (offset == 0x10)
        s->global_config = value;
    else if (offset == 0x2008) {
        if (value & 0x80000000u)
            s->ipi[cpu] |= 0x80000000u;
        for (int target = 0; target < 2; target++)
            if (value & (1u << target))
                s->ipi[target] |= 1;
    } else if (offset == 0x200c)
        s->ipi[cpu] &= ~value;
    else if (offset >= 0x2010 && offset <= 0x2018)
        s->local_timer[cpu][(offset - 0x2010) / 4] = value;
    else if (offset == 0x2020)
        s->ipi_mask[cpu] = value;
    else if (offset == 0x2024)
        s->ipi_mask[cpu] |= value;
    else if (offset == 0x2028)
        s->ipi_mask[cpu] &= ~value;
    else if (offset >= 0x3000 && offset < 0x3000 + A6_IRQS * 4)
        s->destination[(offset - 0x3000) / 4] = value;
    else if (offset >= 0x4000 && offset < 0x4018)
        s->software[(offset - 0x4000) / 4] |= value;
    else if (offset >= 0x4080 && offset < 0x4098)
        s->software[(offset - 0x4080) / 4] &= ~value;
    else if (offset >= 0x4100 && offset < 0x4118)
        s->mask[(offset - 0x4100) / 4] |= value;
    else if (offset >= 0x4180 && offset < 0x4198)
        s->mask[(offset - 0x4180) / 4] &= ~value;
    else
        qemu_log_mask(LOG_UNIMP,
                      "A6 AIC write +0x%04" HWADDR_PRIx " = 0x%08" PRIx64 "\n",
                      offset, value);
    a6_irq_update(s);
    if (offset == 0x300c || ((offset == 0x4000 || offset == 0x4080 ||
                              offset == 0x4100 || offset == 0x4180) &&
                             (value & 8))) {
        trace_a6_iop_doorbell_log(current_cpu == CPU(s->iop_cpu), offset, value,
                                  s->software[0], s->mask[0], s->level[0],
                                  s->destination[3]);
    }
}

static const MemoryRegionOps a6_aic_ops = {
    .read = a6_aic_read,
    .write = a6_aic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4},
};

static uint64_t a6_unimplemented_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    S5L8950State *s = opaque;
    /* Keep MCA observations independent of unrelated boot-time MMIO noise.
     * This is tracing only: unimplemented registers retain their old result. */
    if (s->audio_ports_probe && offset >= 0x4196000 && offset < 0x4198000 &&
        s->audio_accesses++ < 2048) {
        qemu_log_mask(LOG_UNIMP,
                      "A6 MCA read %08" HWADDR_PRIx " size%u PC=%08x\n",
                      0x30000000 + offset, size,
                      current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
    }
    if (s->ae2_cpu && current_cpu == CPU(s->ae2_cpu) &&
        s->ae2_unimplemented_accesses++ < 2048) {
        qemu_log_mask(LOG_UNIMP,
                      "A6 AE2 peripheral read %08" HWADDR_PRIx
                      " size%u PC=%08x\n",
                      0x30000000 + offset, size, s->ae2_cpu->env.regs[15]);
    }
    if (((offset >= 0x41e0000 && offset < 0x41e2000) ||
         (offset >= 0x41a0000 && offset < 0x41a1000) ||
         (offset >= 0x41c0100 && offset < 0x41c2100)) &&
        s->ae2_accesses++ < 4096) {
        qemu_log_mask(LOG_UNIMP,
                      "A6 AE2 unimplemented read %08" HWADDR_PRIx
                      " size%u PC=%08x\n",
                      0x30000000 + offset, size,
                      current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
    }
    if (offset == 0x1000000)
        return s->nand_dll_control;
    if (offset == 0x1000008) {
        /* Fixed board clock: training converges at tap 16. */
        return s->nand_dll_control ? 16u << 7 : 0;
    }
    if (offset >= 0xf100000 && offset < 0xf107000 && size == 4) {
        return s->pmgr[(offset - 0xf100000) / 4];
    }
    if (current_cpu == CPU(s->iop_cpu) && s->unimplemented_iop++ < 2048)
        qemu_log_mask(LOG_UNIMP,
                      "A6 IOP MMIO read %08" HWADDR_PRIx " PC=%08x\n",
                      0x30000000 + offset, s->iop_cpu->env.regs[15]);
    if (s->unimplemented++ < 8192)
        qemu_log_mask(LOG_UNIMP,
                      "A6 unimplemented read 0x%08" HWADDR_PRIx " size %u\n",
                      0x30000000 + offset, size);
    return 0;
}

static void a6_unimplemented_write(void *opaque, hwaddr offset, uint64_t value,
                                   unsigned size)
{
    S5L8950State *s = opaque;
    if (s->audio_ports_probe && offset >= 0x4196000 && offset < 0x4198000 &&
        s->audio_accesses++ < 2048) {
        qemu_log_mask(LOG_UNIMP,
                      "A6 MCA write %08" HWADDR_PRIx " = %08x size%u PC=%08x\n",
                      0x30000000 + offset, (unsigned)value, size,
                      current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
    }
    if (s->ae2_cpu && current_cpu == CPU(s->ae2_cpu) &&
        s->ae2_unimplemented_accesses++ < 2048) {
        qemu_log_mask(LOG_UNIMP,
                      "A6 AE2 peripheral write %08" HWADDR_PRIx
                      " = %08x size%u PC=%08x\n",
                      0x30000000 + offset, (unsigned)value, size,
                      s->ae2_cpu->env.regs[15]);
    }
    if (((offset >= 0x41e0000 && offset < 0x41e2000) ||
         (offset >= 0x41a0000 && offset < 0x41a1000) ||
         (offset >= 0x41c0100 && offset < 0x41c2100)) &&
        s->ae2_accesses++ < 4096) {
        qemu_log_mask(LOG_UNIMP,
                      "A6 AE2 unimplemented write %08" HWADDR_PRIx
                      " = %08x size%u PC=%08x\n",
                      0x30000000 + offset, (unsigned)value, size,
                      current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
    }
    if (offset == 0x1000000) {
        s->nand_dll_control = value;
        return;
    }
    if (offset == 0xf101220 && (value & 2)) {
        arm_set_cpu_on(1, A6_RAM_BASE, 0, 1, false);
    }
    if (offset >= 0xf100000 && offset < 0xf107000 && size == 4) {
        uint32_t reg = offset - 0xf100000;
        if (reg >= 0x1000 && reg < 0x1200) {
            value = (value & ~0xf0u) | ((value & 0xf) << 4);
        }
        /* Original IOP b618/b644 writes bit 6 here then waits for it to
         * clear. It is a completed clock transition command, not a latch. */
        if (reg == 0x1204 && (value & 0x40)) {
            value &= ~0x40u;
        }
        s->pmgr[reg / 4] = value;
        return;
    }
    if (current_cpu == CPU(s->iop_cpu) && s->unimplemented_iop++ < 2048)
        qemu_log_mask(
            LOG_UNIMP, "A6 IOP MMIO write %08" HWADDR_PRIx " = %08x PC=%08x\n",
            0x30000000 + offset, (unsigned)value, s->iop_cpu->env.regs[15]);
    if (s->unimplemented++ < 8192)
        qemu_log_mask(LOG_UNIMP,
                      "A6 unimplemented write 0x%08" HWADDR_PRIx
                      " = 0x%08" PRIx64 " size %u\n",
                      0x30000000 + offset, value, size);
}

static const MemoryRegionOps a6_unimplemented_ops = {
    .read = a6_unimplemented_read,
    .write = a6_unimplemented_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 1, .max_access_size = 8},
};

static uint64_t a6_iop_read(void *opaque, hwaddr off, unsigned size)
{
    S5L8950State *s = opaque;
    uint32_t value = s->iop_regs[off / 4];
    qemu_log_mask(LOG_UNIMP, "A6 IOP %s read +%03x = %08x PC=%08x\n",
                  current_cpu == CPU(s->iop_cpu) ? "core" : "AP", (unsigned)off,
                  value, current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
    return value;
}
static void a6_iop_write(void *opaque, hwaddr off, uint64_t value,
                         unsigned size)
{
    S5L8950State *s = opaque;
    qemu_log_mask(LOG_UNIMP, "A6 IOP %s write +%03x = %08x\n",
                  current_cpu == CPU(s->iop_cpu) ? "core" : "AP", (unsigned)off,
                  (unsigned)value);
    s->iop_regs[off / 4] = value;
    if (off == 0 && value == 0 && current_cpu != CPU(s->iop_cpu)) {
        /* The native AP stops the IOP before replacing its firmware on wake.
         * Retaining DRAM does not permit the old core to keep executing while
         * that image and its message-ring configuration are being rewritten. */
        s->iop_cpu->power_state = PSCI_OFF;
        s->iop_cpu->env.halt_reason = HALT_PSCI;
        CPU(s->iop_cpu)->halted = 1;
        qemu_cpu_kick(CPU(s->iop_cpu));
    }
    if (off == 0 && value == 1 && current_cpu != CPU(s->iop_cpu)) {
        uint32_t base = s->iop_regs[1];
        if (base < A6_RAM_BASE ||
            (uint64_t)base + 0x100000 > A6_RAM_BASE + A6_RAM_SIZE) {
            error_report("A6 IOP firmware base outside DRAM");
            return;
        }
        if (s->iop_fw_mapped) {
            memory_region_del_subregion(&s->iop_mem, &s->iop_fw);
            object_unparent(OBJECT(&s->iop_fw));
        }
        memory_region_init_alias(&s->iop_fw, OBJECT(s), "a6.iop-firmware",
                                 &s->ram, base - A6_RAM_BASE, 0x100000);
        memory_region_add_subregion_overlap(&s->iop_mem, 0, &s->iop_fw, 1);
        s->iop_fw_mapped = true;
        CPUState *cs = CPU(s->iop_cpu);
        cpu_reset(cs);
        s->iop_cpu->power_state = PSCI_ON;
        s->iop_cpu->env.halt_reason = NOT_HALTED;
        cs->halted = 0;
        qemu_cpu_kick(cs);
    }
}
static const MemoryRegionOps a6_iop_ops = {
    .read = a6_iop_read,
    .write = a6_iop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4},
};

static void a6_ae2_irq_update(S5L8950State *s)
{
    if (!s->ae2_cpu) {
        return;
    }
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->ae2_cpu), ARM_CPU_IRQ),
                 (s->ae2_pending & s->ae2_irq_enable) != 0);
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->ae2_cpu), ARM_CPU_FIQ),
                 (s->ae2_pending & s->ae2_fiq_enable) != 0);
}

/* Original AE2 firmware: IRQ handler 2d24 reads 1000/1004, FIQ handler
 * 2d68 reads 1010/1014, mask/unmask 2bb0/2c98, soft set/clear 2dac/2dc0.
 * Message doorbell 4 is registered at 339c and acknowledged at 33d0. */
static uint64_t a6_ae2_irq_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8950State *s = opaque;
    switch (offset) {
    case 0x1000:
    case 0x1010:
        return s->ae2_pending;
    case 0x1004:
        return s->ae2_irq_enable;
    case 0x1014:
        return s->ae2_fiq_enable;
    default:
        return 0;
    }
}

static void a6_ae2_irq_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    S5L8950State *s = opaque;
    if (s->ae2_accesses++ < 4096) {
        qemu_log_mask(LOG_UNIMP,
                      "A6 AE2 IRQ write +%04x = %08x cpu%d PC=%08x\n",
                      (unsigned)offset, (unsigned)value,
                      current_cpu ? current_cpu->cpu_index : -1,
                      current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
    }
    switch (offset) {
    case 0x1008:
        s->ae2_irq_enable |= value;
        break;
    case 0x100c:
        s->ae2_irq_enable &= ~value;
        break;
    case 0x1018:
        s->ae2_fiq_enable |= value;
        break;
    case 0x101c:
        s->ae2_fiq_enable &= ~value;
        break;
    case 0x1020:
        s->ae2_pending |= value;
        break;
    case 0x1024:
        s->ae2_pending &= ~value;
        break;
    default:
        /* Clock/reset words are still unmodeled. AP startup writes reset
         * bit zero and polls for it to clear; no IRQ status is fabricated. */
        return;
    }
    a6_ae2_irq_update(s);
}

static const MemoryRegionOps a6_ae2_irq_ops = {
    .read = a6_ae2_irq_read,
    .write = a6_ae2_irq_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4},
};

/* WFI notification timing is an explicit AE2 bring-up hypothesis. The AP
 * registers IRQ 97 and its original handler (804628d4) clears event bit 4
 * at 341e1024 before invoking EmbeddedIOP's completion callback. Firmware
 * completes its work and enters WFI without an observed host-doorbell
 * write. Probe a latched idle transition, without inspecting commands or
 * manufacturing completion records. Physical timing still needs checking. */
static void a6_ae2_wfi_tick(void *opaque)
{
    S5L8950State *s = opaque;
    CPUState *cpu = CPU(s->ae2_cpu);
    bool idle = s->ae2_cpu->power_state == PSCI_ON && cpu->halted &&
                s->ae2_cpu->env.halt_reason == HALT_WFI;
    if (idle && !(s->ae2_pending & 0x10)) {
        if (!s->ae2_wfi_seen) {
            s->ae2_host_pending |= 0x10;
            a6_irq(s, 97, 1);
            qemu_log_mask(LOG_UNIMP, "A6 AE2 WFI host event\n");
        }
        s->ae2_wfi_seen = true;
    } else {
        s->ae2_wfi_seen = false;
    }
    timer_mod(s->ae2_wfi_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
}

static uint64_t a6_ae2_control_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8950State *s = opaque;
    uint32_t value =
        offset == 0x1024 ? s->ae2_host_pending : s->ae2_regs[offset / 4];
    qemu_log_mask(LOG_UNIMP, "A6 AE2 control read +%03x = %08x\n",
                  (unsigned)offset, value);
    return value;
}

static void a6_ae2_hold(S5L8950State *s)
{
    CPUState *cpu = CPU(s->ae2_cpu);

    cpu_reset(cpu);
    s->ae2_cpu->power_state = PSCI_OFF;
    cpu->halted = 1;
    s->ae2_pending = 0;
    s->ae2_irq_enable = 0;
    s->ae2_fiq_enable = 0;
    s->ae2_host_pending = 0;
    s->ae2_wfi_seen = false;
    a6_ae2_irq_update(s);
    a6_irq(s, 97, 0);
}

static void a6_ae2_control_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    S5L8950State *s = opaque;
    uint32_t previous = s->ae2_regs[offset / 4];
    s->ae2_regs[offset / 4] = value;
    qemu_log_mask(LOG_UNIMP, "A6 AE2 control write +%03x = %08x\n",
                  (unsigned)offset, (unsigned)value);
    if (offset == 4 && !(value & 3)) {
        a6_ae2_hold(s);
    }
    if (offset == 0x1020) {
        /* AppleA5AE2::_signalInterrupt, 80462e1a..2e36, sets doorbell
         * bit 4/5. The original AE2 callback clears it through 341a1024. */
        s->ae2_pending |= value;
        s->ae2_wfi_seen = false;
        a6_ae2_irq_update(s);
    }
    if (offset == 0x1024) {
        s->ae2_host_pending &= ~value;
        a6_irq(s, 97, s->ae2_host_pending != 0);
    }
    /* AppleA5AE2::startCPU, original 11A465 at 80462c54..80462d46:
     * hold control+4 at zero, write firmware entry to +8, then release it
     * with 00ff0003. The AP's EmbeddedIOP control pointers reference the
     * firmware's data/BSS in this same allocation, so the low firmware
     * window must share that memory rather than use a disconnected copy.
     * This does not manufacture the EmbeddedIOP 'nop ' response. */
    if (offset == 4 && (value & 3) == 3 && (previous & 3) != 3) {
        uint32_t entry = s->ae2_regs[8 / 4];
        if (entry < A6_RAM_BASE ||
            (uint64_t)entry + 0x100000 > A6_RAM_BASE + A6_RAM_SIZE) {
            error_report("A6 AE2 firmware entry outside DRAM");
            return;
        }
        if (s->ae2_fw_mapped) {
            memory_region_del_subregion(&s->ae2_mem, &s->ae2_low_ram);
            object_unparent(OBJECT(&s->ae2_low_ram));
        }
        memory_region_init_alias(&s->ae2_low_ram, OBJECT(s), "a6.ae2-firmware",
                                 &s->ram, entry - A6_RAM_BASE, 0x100000);
        memory_region_add_subregion(&s->ae2_mem, 0, &s->ae2_low_ram);
        s->ae2_fw_mapped = true;
        s->ae2_wfi_seen = false;
        CPUState *cpu = CPU(s->ae2_cpu);
        cpu_reset(cpu);
        cpu_set_pc(cpu, 0);
        s->ae2_cpu->power_state = PSCI_ON;
        s->ae2_cpu->env.halt_reason = NOT_HALTED;
        cpu->halted = 0;
        qemu_cpu_kick(cpu);
    }
}

static const MemoryRegionOps a6_ae2_control_ops = {
    .read = a6_ae2_control_read,
    .write = a6_ae2_control_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4},
};

static uint64_t a6_ae2_unimplemented_read(void *opaque, hwaddr offset,
                                          unsigned size)
{
    S5L8950State *s = opaque;
    if (s->ae2_accesses++ < 4096) {
        qemu_log_mask(LOG_UNIMP,
                      "A6 AE2 core read %08" HWADDR_PRIx " size%u PC=%08x\n",
                      offset, size, s->ae2_cpu->env.regs[15]);
    }
    return 0;
}

static void a6_ae2_unimplemented_write(void *opaque, hwaddr offset,
                                       uint64_t value, unsigned size)
{
    S5L8950State *s = opaque;
    if (s->ae2_accesses++ < 4096) {
        qemu_log_mask(LOG_UNIMP,
                      "A6 AE2 core write %08" HWADDR_PRIx
                      " = %08x size%u PC=%08x\n",
                      offset, (unsigned)value, size, s->ae2_cpu->env.regs[15]);
    }
}

static const MemoryRegionOps a6_ae2_unimplemented_ops = {
    .read = a6_ae2_unimplemented_read,
    .write = a6_ae2_unimplemented_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 1, .max_access_size = 8},
};

static void a6_ae2_init(S5L8950State *s, MemoryRegion *memory)
{
    memory_region_init(&s->ae2_mem, OBJECT(s), "a6.ae2-memory", 1ULL << 32);
    memory_region_init_io(&s->ae2_unimplemented, OBJECT(s),
                          &a6_ae2_unimplemented_ops, s, "a6.ae2-unimplemented",
                          1ULL << 32);
    memory_region_add_subregion_overlap(&s->ae2_mem, 0, &s->ae2_unimplemented,
                                        -1);
    memory_region_init_alias(&s->ae2_high_sram, OBJECT(s),
                             "a6.ae2-physical-sram", &s->ae2_sram, 0, 0x300000);
    memory_region_add_subregion(&s->ae2_mem, 0x40000000, &s->ae2_high_sram);
    memory_region_init_alias(&s->ae2_dram, OBJECT(s), "a6.ae2-dram", &s->ram, 0,
                             A6_RAM_SIZE);
    memory_region_add_subregion(&s->ae2_mem, A6_RAM_BASE, &s->ae2_dram);
    memory_region_init_alias(&s->ae2_cached_dram, OBJECT(s),
                             "a6.ae2-cached-dram", &s->ram, 0, A6_RAM_SIZE);
    memory_region_add_subregion(&s->ae2_mem, 0xc0000000, &s->ae2_cached_dram);
    memory_region_init_alias(&s->ae2_peripheral, OBJECT(s),
                             "a6.ae2-peripherals", memory, 0x30000000,
                             0x10000000);
    memory_region_add_subregion(&s->ae2_mem, 0x30000000, &s->ae2_peripheral);
    /* The original firmware maps one section at 34000000 and allocates
     * its heap there. The probe backs that local view with ae2/reg entry
     * 4's AP-visible SRAM; the full physical banking remains unqualified. */
    memory_region_init_alias(&s->ae2_work_sram, OBJECT(s), "a6.ae2-work-sram",
                             &s->ae2_sram, 0, 0x100000);
    memory_region_add_subregion_overlap(&s->ae2_mem, 0x34000000,
                                        &s->ae2_work_sram, 1);
    /* ISA execution probe; the physical AE2 core identity remains unqualified. */
    Object *core = object_new(ARM_CPU_TYPE_NAME("cortex-a9"));
    s->ae2_cpu = ARM_CPU(core);
    object_property_set_link(core, "memory", OBJECT(&s->ae2_mem), &error_abort);
    object_property_set_bool(core, "start-powered-off", true, &error_abort);
    qdev_realize(DEVICE(core), NULL, &error_fatal);
    object_unref(core);
    memory_region_init_io(&s->ae2_irq_control, OBJECT(s), &a6_ae2_irq_ops, s,
                          "a6.ae2-interrupts", 0x3000);
    memory_region_add_subregion_overlap(memory, 0x341a0000, &s->ae2_irq_control,
                                        1);
    memory_region_init_io(&s->ae2_control, OBJECT(s), &a6_ae2_control_ops, s,
                          "a6.ae2-control", 0x2000);
    memory_region_add_subregion_overlap(memory, 0x341e0000, &s->ae2_control, 1);
    s->ae2_wfi_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, a6_ae2_wfi_tick, s);
    timer_mod(s->ae2_wfi_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
}

static void a6_reset_soc(S5L8950State *s)
{
    memset(s->level, 0, sizeof(s->level));
    memset(s->software, 0, sizeof(s->software));
    memset(s->mask, 0xff, sizeof(s->mask));
    memset(s->ipi, 0, sizeof(s->ipi));
    memset(s->ipi_mask, 0, sizeof(s->ipi_mask));
    memset(s->pmgr, 0, sizeof(s->pmgr));
    s->audio_accesses = 0;
    memset(s->iop_regs, 0, sizeof(s->iop_regs));
    if (s->iop_timer)
        timer_del(s->iop_timer);
    s->iop_timer_control = s->iop_timer_reload = s->iop_timer_status = 0;
    s->iop_timer_deadline = 0;
    s->iop_other_mask = UINT32_MAX;
    if (s->iop_cpu) {
        cpu_reset(CPU(s->iop_cpu));
        s->iop_cpu->power_state = PSCI_OFF;
        CPU(s->iop_cpu)->halted = 1;
    }
    if (s->ae2_cpu) {
        memset(s->ae2_regs, 0, sizeof(s->ae2_regs));
        a6_ae2_hold(s);
    }
    for (int i = 0; i < A6_IRQS; i++)
        s->destination[i] = 1;
}

/* AP hibernate retains the independently powered IOP and its NAND queues,
 * AE2 and peripheral devices. Clearing them after retained-RAM resume leaves
 * native drivers waiting on transactions whose firmware state was lost. */
static void a6_reset_ap(S5L8950State *s)
{
    memset(s->software, 0, sizeof(s->software));
    memset(s->mask, 0xff, sizeof(s->mask));
    memset(s->ipi, 0, sizeof(s->ipi));
    memset(s->ipi_mask, 0, sizeof(s->ipi_mask));
    for (unsigned i = 0; i < A6_IRQS; i++) {
        s->destination[i] = 1;
    }
}

/* Same kboot AP-rail lifecycle as ipad1.c, using the original N42 LLB
 * handoff at 1001ac0c: XSOM/PSUS at DRAM+80, clear, entry0 and r0=0.
 * Retain DRAM and host-agent tokens; this is not a fresh guest boot. */
static bool a6_resume(S5L8950State *s)
{
    uint32_t marker[2];
    address_space_read(&address_space_memory, A6_RAM_BASE + 0x80,
                       MEMTXATTRS_UNSPECIFIED, marker, sizeof(marker));
    if (le32_to_cpu(marker[0]) != 0x4d4f5358 ||
        le32_to_cpu(marker[1]) != 0x53555350 || !a6_pmu_take_suspend(s->pmu)) {
        return false;
    }
    address_space_set(&address_space_memory, A6_RAM_BASE + 0x80, 0,
                      sizeof(marker), MEMTXATTRS_UNSPECIFIED);
    a6_reset_ap(s);
    for (unsigned i = 0; i < 2; i++) {
        cpu_reset(CPU(s->cpu[i]));
        s->cpu[i]->env.cp15.c14_timer[GTIMER_VIRT].cval = UINT64_MAX;
        s->cpu[i]->env.cp15.c14_timer[GTIMER_PHYS].cval = UINT64_MAX;
        s->cpu[i]->env.regs[0] = 0;
        cpsr_write(&s->cpu[i]->env, ARM_CPU_MODE_SVC | CPSR_I | CPSR_F,
                   0xffffffff, CPSRWriteRaw);
        cpu_set_pc(CPU(s->cpu[i]), 0);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->button_down); i++) {
        qemu_set_irq(qdev_get_gpio_in(s->gpio, i), !s->button_down[i]);
    }
    a6_pmu_reassert(s->pmu);
    fprintf(stderr, "A6 N42AP: resume through retained low-DRAM trampoline\n");
    return true;
}

static void a6_ap_cut(void *opaque)
{
    S5L8950State *s = opaque;
    for (unsigned i = 0; i < 2; i++) {
        arm_set_cpu_off(arm_cpu_mp_affinity(s->cpu[i]));
    }
}

static void a6_ap_power(void *opaque, int n, int on)
{
    S5L8950State *s = opaque;
    if (on) {
        timer_del(s->ap_off_timer);
        s->resume_boot = true;
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    } else {
        /* As on older kboot boards, leave time for XNU to write its
         * suspend markers after the PMU command. Physical delay unmeasured. */
        timer_mod(s->ap_off_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 20000000);
    }
}

static void a6_machine_reset(MachineState *machine, ResetType type)
{
    S5L8950State *s = S5L8950_MACHINE(machine);
    if (s->resume_boot) {
        s->resume_boot = false;
        timer_del(s->ap_off_timer);
        if (a6_resume(s)) {
            return;
        }
        /* Standby wake has no retained-RAM handoff: let the PMU's reset
         * take its cold path rather than preserve a stale wake latch. */
        a6_pmu_cancel_resume(s->pmu);
    }
    qemu_devices_reset(type);
}

/* N42's original iBoot copies this product identity into the IPSW device
 * tree's reserved slot. MobileGestalt's MGIsDeviceOfType uses these bytes,
 * separately from hw.model, to select native hardware such as the GPS. */
static const uint8_t a6_n42_product_id[20] = {
    0x93, 0xea, 0x0f, 0xa9, 0x8b, 0x3e, 0x08, 0x14, 0xa0, 0x74,
    0xa3, 0x4f, 0xd5, 0x59, 0x84, 0x84, 0x30, 0xe8, 0xa7, 0xca,
};

static void a6_dt_identity(uint8_t *image, size_t length, uint32_t load,
                           uint32_t arguments)
{
    a4_dt_edit(image, length, load, arguments,
               &(A4DTEdit){"product", "product-id", a6_n42_product_id,
                           sizeof(a6_n42_product_id)});
}

static void a6_reset(void *opaque)
{
    S5L8950State *s = opaque;
    bool resume = s->resume_boot;
    s->resume_boot = false;
    timer_del(s->ap_off_timer);
    if (resume && a6_resume(s)) {
        return;
    }
    g_autofree char *data = NULL;
    gsize size;
    GError *error = NULL;
    if (!g_file_get_contents(s->kboot, &data, &size, &error) || size < 24 ||
        memcmp(data + size - 24, "K48KBOOT", 8)) {
        error_report("iPhone-5: invalid kboot input: %s",
                     error ? error->message : s->kboot);
        exit(1);
    }
    const uint8_t *trailer = (const uint8_t *)data + size - 24;
    uint32_t load = ldl_le_p(trailer + 8), entry = ldl_le_p(trailer + 12);
    uint32_t arguments = ldl_le_p(trailer + 16),
             length = ldl_le_p(trailer + 20);
    if (length > size - 24 || load < A6_RAM_BASE ||
        (uint64_t)load + length > A6_RAM_BASE + A6_RAM_SIZE) {
        error_report("iPhone-5: kboot image outside A6 DRAM");
        exit(1);
    }
    if (s->quiet_boot &&
        !ios_boot_args_quiet((uint8_t *)data, length, load, arguments)) {
        error_report("quiet-boot: invalid ARM32 boot command line");
        exit(1);
    }
    /* iBoot supplies fresh entropy in the IPSW DT's reserved random-seed
     * slot. XNU rejects an all-zero seed; regenerate it on every reset,
     * without storing host entropy in the prepared firmware artifact. */
    uint8_t random_seed[64];
    qemu_guest_getrandom_nofail(random_seed, sizeof(random_seed));
    a4_dt_edit(
        (uint8_t *)data, length, load, arguments,
        &(A4DTEdit){"chosen", "random-seed", random_seed, sizeof(random_seed)});
    /* As on ipad1 when camera=off, an absent ISP must not bind its native
     * driver. A silent H4 provider blocks media/flashlight requests and hence
     * SpringBoard input until their RPCs time out. Keep the original node and
     * prepared image intact; describe the hardware this board implements. */
    if (!s->camera) {
        a4_dt_edit((uint8_t *)data, length, load, arguments,
                   &(A4DTEdit){"isp", "compatible", "none", 5});
    }
    a6_dt_identity((uint8_t *)data, length, load, arguments);
    a6_reset_soc(s);
    guest_pkg_reset(&s->pkg);
    ipod_agent_reset(s->agent);
    address_space_write(&address_space_memory, load, MEMTXATTRS_UNSPECIFIED,
                        data, length);
    for (gsize off = length; off + 20 <= size - 24;) {
        const uint8_t *seg = (const uint8_t *)data + off;
        uint32_t pa = ldl_le_p(seg + 8), bytes = ldl_le_p(seg + 12);
        bool zero = ldl_le_p(seg + 16) & 1;
        off += 20 + (zero ? 0 : bytes);
        if (memcmp(seg, "K48SEG\0\0", 8) || off > size - 24 ||
            pa < A6_RAM_BASE ||
            (uint64_t)pa + bytes > A6_RAM_BASE + A6_RAM_SIZE) {
            error_report("iPhone-5: invalid auxiliary RAM segment");
            exit(1);
        }
        if (zero)
            address_space_set(&address_space_memory, pa, 0, bytes,
                              MEMTXATTRS_UNSPECIFIED);
        else
            address_space_write(&address_space_memory, pa,
                                MEMTXATTRS_UNSPECIFIED, seg + 20, bytes);
    }
    s->framebuffer = ldl_le_p((uint8_t *)data + arguments - load + 0x14);
    for (int i = 0; i < 2; i++) {
        cpu_reset(CPU(s->cpu[i]));
        s->cpu[i]->env.cp15.c14_timer[GTIMER_VIRT].cval = UINT64_MAX;
        s->cpu[i]->env.cp15.c14_timer[GTIMER_PHYS].cval = UINT64_MAX;
    }
    s->cpu[0]->env.regs[0] = arguments;
    cpsr_write(&s->cpu[0]->env, ARM_CPU_MODE_SVC | CPSR_I | CPSR_F, 0xffffffff,
               CPSRWriteRaw);
    cpu_set_pc(CPU(s->cpu[0]), entry);
    fprintf(
        stderr,
        "A6 N42AP: original kernel entry=0x%08x bootargs=0x%08x DRAM=0x80000000+1GiB\n",
        entry, arguments);
}

/* Use the existing native launchd halt RPC. The request itself is not proof
 * of shutdown: QEMU exits on PMU standby or the completed charging-halt restart. */
static void a6_powerdown_req(Notifier *notifier, void *opaque)
{
    S5L8950State *s = S5L8950_MACHINE(qdev_get_machine());
    g_autofree char *request = g_strdup_printf(
        "qemu-powerdown-%" PRIu64 " halt\n", ++s->powerdown_sequence);
    int error = ipod_agent_submit(s->agent, request);
    if (error) {
        warn_report("iPhone-5: guest shutdown request: %s",
                    ipod_agent_submit_error(error));
    }
}

static Notifier a6_powerdown_notifier = {.notify = a6_powerdown_req};

static uint32_t a6_uart_rx_avail(void *opaque, hwaddr addr, bool to_device)
{
    uint32_t status =
        address_space_ldl_le(&address_space_memory, (addr & ~0xffull) + 0x18,
                             MEMTXATTRS_UNSPECIFIED, NULL);

    /* Original AppleSamsungSerial80a4c3a4: low four count bits, full=16. */
    return to_device ? 0 : (status & 0x100) ? 16 : (status & 0xf);
}

static void a6_uart_cdma_kick(void *opaque)
{
    s5l8930_cdma_kick(DEVICE(opaque));
}

static void a6_uart_rxdma(void *opaque, int pin, int level)
{
    if (level) {
        qemu_bh_schedule(opaque);
    }
}

static void a6_init(MachineState *machine)
{
    S5L8950State *s = S5L8950_MACHINE(machine);
    MemoryRegion *memory = get_system_memory();
    DeviceState *modem_uart = NULL;
    s->restore_fd = -1;
    gles_host_set_a6(true);
    if (s->restore_image) {
        struct stat st;
        s->restore_fd = open(s->restore_image, O_RDONLY | O_CLOEXEC);
        if (s->restore_fd < 0 || fstat(s->restore_fd, &st) ||
            !S_ISREG(st.st_mode)) {
            error_report("Cannot open regular restore-image file");
            exit(1);
        }
        s->restore_size = st.st_size;
    }
    if (!s->kboot) {
        error_report("iPhone-5 requires kboot= matching N42AP firmware");
        exit(1);
    }
    memory_region_init_ram(&s->ram, NULL, "a6.dram", A6_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(memory, A6_RAM_BASE, &s->ram);
    memory_region_init_alias(&s->low_ram, NULL, "a6.reset-vectors", &s->ram, 0,
                             0x1000);
    memory_region_add_subregion(memory, 0, &s->low_ram);
    memory_region_init_io(&s->peripheral, OBJECT(s), &a6_unimplemented_ops, s,
                          "a6.unimplemented", 0x10000000);
    memory_region_add_subregion_overlap(memory, 0x30000000, &s->peripheral, -1);
    memory_region_init_ram(&s->sram, NULL, "a6.sram", 0x100000, &error_fatal);
    memory_region_add_subregion_overlap(memory, 0x3f000000, &s->sram, 1);
    /* Original N42 ae2/reg entry 4: the audio IOP's dedicated 3 MiB SRAM.
     * The AP loads original s5l8950xae2 firmware here. Its processor and
     * control/interrupt contracts are still under bring-up. */
    memory_region_init_ram(&s->ae2_sram, NULL, "a6.ae2-sram", 0x300000,
                           &error_fatal);
    memory_region_add_subregion(memory, 0x40000000, &s->ae2_sram);
    for (int i = 0; i < 2; i++) {
        Object *cpu = object_new(machine->cpu_type);
        s->cpu[i] = ARM_CPU(cpu);
        object_property_set_link(cpu, "memory", OBJECT(memory), &error_abort);
        object_property_set_int(cpu, "cntfrq", 24000000, &error_abort);
        object_property_set_bool(cpu, "start-powered-off", i != 0,
                                 &error_abort);
        qdev_realize(DEVICE(cpu), NULL, &error_fatal);
        define_arm_cp_regs(s->cpu[i], a6_guest_cpregs);
        object_unref(cpu);
    }
    memory_region_init_io(&s->aic, OBJECT(s), &a6_aic_ops, s, "a6.aic-v1",
                          0x10000);
    memory_region_add_subregion(memory, 0x3f200000, &s->aic);
    DeviceState *gpio = qdev_new(TYPE_S5L8930_GPIO);
    s->gpio = gpio;
    qdev_prop_set_uint32(gpio, "ports", 29);
    qdev_prop_set_uint32(gpio, "int-groups", 8);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 0, 0x3fa00000);
    sysbus_connect_irq(SYS_BUS_DEVICE(gpio), 0,
                       qemu_allocate_irq(a6_irq, s, 119));
    /* N42 DT spi1: 0x32100000, IRQ 29; multi-touch,n41 ATN is
     * GPIO interrupt 201 (port 25, pin 1). The shared Samsung SPI model
     * accepts the native C1F57 firmware's 64452-byte download burst. */
    DeviceState *touch_spi = qdev_new(TYPE_IPOD_TOUCH_SPI);
    qdev_prop_set_uint8(touch_spi, "index", 1);
    qdev_prop_set_string(touch_spi, "peripheral", "multitouch");
    qdev_prop_set_uint32(touch_spi, "tx-fifo-depth", 0x10000);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(touch_spi), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(touch_spi), 0, 0x32100000);
    sysbus_connect_irq(SYS_BUS_DEVICE(touch_spi), 0,
                       qemu_allocate_irq(a6_irq, s, 29));
    IPodTouchMultitouchState *mt = IPOD_TOUCH_SPI(touch_spi)->mt;
    mt->profile = &a6_mt_profile;
    mt->hold_atn = true;
    qdev_connect_gpio_out_named(DEVICE(mt), "atn", 0,
                                qdev_get_gpio_in(gpio, 201));
    qemu_add_mouse_event_handler(a6_mouse_event, mt, 1, "iPhone 5 Touchscreen");
    ipod_touch_multitouch_register_input(mt);
    if (s->audio_ports_probe) {
        /* Original N42 DT: SPI3 uses the same spi-1,samsung controller
         * already used for touch. Its codec register-port protocol is
         * qualified separately in the CS42L65 model. */
        DeviceState *spi = qdev_new(TYPE_IPOD_TOUCH_SPI);
        qdev_prop_set_uint8(spi, "index", 3);
        qdev_prop_set_string(spi, "peripheral", "cs42l65");
        qdev_prop_set_uint32(spi, "trace-transfer-limit", 512);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(spi), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(spi), 0, 0x32300000);
        sysbus_connect_irq(SYS_BUS_DEVICE(spi), 0,
                           qemu_allocate_irq(a6_irq, s, 31));
        /* Native i2s-1,samsung ports. Reuse the existing PCM/rate model;
         * codec LRCLK and A6 clock-source routing need qualification. */
        for (int i = 0; i < 3; i++) {
            DeviceState *port = qdev_new(TYPE_S5L8930_I2S);
            qdev_prop_set_bit(port, "audio-out", i == 0);
            qdev_prop_set_uint8(port, "port", i);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(port), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(port), 0, 0x34190000 + i * 0x1000);
        }
        /* MCA speaker/voice FIFOs use the same PCM sink and DMA pacing.
         * The speaker stream is 44.1 kHz on 11A465 and on 14G61, whose
         * lock sound (a 48 kHz file), Opening ringtone and charging chime
         * all play at 1.000x here. The MCA is the frame-clock slave on
         * that port and no register the guest writes carries the rate: the
         * speaker's DT audio-data word 0 bit 0 is clear, so AppleAE2MCA
         * never programs its NCO (function-mclk_frequency, NCOf 6) except
         * to 0 at power-down, and the MCA and CS42L65 writes are the same
         * for every stream. Hence a fixed external clock. */
        for (int i = 0; i < 2; i++) {
            DeviceState *port = qdev_new(TYPE_S5L8930_I2S);
            qdev_prop_set_bit(port, "mca", true);
            qdev_prop_set_uint32(port, "frame-rate", 44100);
            qdev_prop_set_bit(port, "audio-out", i == 0);
            qdev_prop_set_bit(port, "audio-in", i == 1);
            qdev_prop_set_uint8(port, "port", 6 + i);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(port), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(port), 0, 0x34196000 + i * 0x1000);
        }
    }
    for (int i = 0; i < 3; i++) {
        DeviceState *dev = qdev_new("a6.i2c");
        s->i2c[i] = (A6I2C *)dev;
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x33200000 + i * 0x100000);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                           qemu_allocate_irq(a6_irq, s, 18 + i));
    }
    I2CSlave *pmu = i2c_slave_new("a6.d1972-registers", 0x74);
    s->pmu = DEVICE(pmu);
    qdev_prop_set_bit(DEVICE(pmu), "adc-probe", s->pmu_adc_probe);
    qdev_prop_set_bit(DEVICE(pmu), "battery-swi", s->battery_probe);
    i2c_slave_realize_and_unref(pmu, s->i2c[0]->bus, &error_fatal);
    qdev_connect_gpio_out(DEVICE(pmu), 0, qdev_get_gpio_in(gpio, 6));
    s->ap_off_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, a6_ap_cut, s);
    qdev_connect_gpio_out_named(DEVICE(pmu), "ap-power", 0,
                                qemu_allocate_irq(a6_ap_power, s, 0));
    if (s->backlight_probe) {
        I2CSlave *backlight = i2c_slave_new("a6.backlight-registers", 0x63);
        i2c_slave_realize_and_unref(backlight, s->i2c[0]->bus, &error_fatal);
    }
    if (s->audio_ports_probe) {
        I2CSlave *amp = i2c_slave_new("cs35l19", 0x40);
        qdev_prop_set_bit(DEVICE(amp), "trace-regs", true);
        i2c_slave_realize_and_unref(amp, s->i2c[0]->bus, &error_fatal);
    }
    if (s->tristar_probe) {
        /* Original N42 DT: i2c0/tristar reg=0x1a. */
        I2CSlave *tristar = i2c_slave_new("a6.tristar-registers", 0x1a);
        i2c_slave_realize_and_unref(tristar, s->i2c[0]->bus, &error_fatal);
        /* N42 DT GPIO31, native cfg204: active-high level interrupt.
         * An absent accessory has no pending status. The GPIO model's
         * default high is appropriate for older active-low devices only. */
        s5l8930_gpio_set_rest_level(gpio, 31, false);
        qdev_connect_gpio_out(DEVICE(tristar), 0, qdev_get_gpio_in(gpio, 31));
    }
    if (s->usb_host_probe) {
        /* N42's DT advertises the A4 PHY and S5L8900 device-controller
         * compatibility strings, at 36000000/36100000 and IRQ11. Reuse
         * qemu-ios's existing OTG and built-in host, including enumeration
         * and the normal USB charging-current grant (not just VBUS). */
        DeviceState *usb = qdev_new(TYPE_IPOD_TOUCH_USB_PHYS);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(usb), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(usb), 0, 0x36000000);
        uint32_t hwcfg[] = {0, 0x7a8f60d0, 0x082000e8, 0x01f08024};
        usb = ipod_touch_init_usb_otg(qemu_allocate_irq(a6_irq, s, 11), hwcfg);
        synopsys_usb_state *otg = S5L8900USBOTG(usb);
        synopsys_usb_set_tcp_addr(otg, NULL);
        otg->builtin_host = !otg->server_host && !getenv("IT_USB_TCP");
        otg->host_charge = true;
        otg->withhold_charge = false;
        sysbus_realize_and_unref(SYS_BUS_DEVICE(usb), &error_fatal);
        memory_region_add_subregion(memory, 0x36100000, &otg->iomem);
        /* The baseband's native UART2 TX platform function polls the UART
         * status before publishing. Reuse the existing Samsung UART model
         * with no external endpoint; an absent modem supplies no RX bytes. */
        exynos4210_uart_create(0x32700000, 256, 2, NULL,
                               qemu_allocate_irq(a6_irq, s, 0x17), true);
        /* N42's native DT usb-complex range starts at 0x36100000:
         * EHCI child +0x300000, OHCI children +0x400000/+0x500000.
         * Reuse qemu-ios's existing A4 host-controller models. */
        DeviceState *host = qdev_new(TYPE_EXYNOS4210_EHCI);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(host), &error_fatal);
        s->hsic_bus = &SYS_BUS_EHCI(host)->ehci.bus;
        sysbus_mmio_map(SYS_BUS_DEVICE(host), 0, 0x36400000);
        sysbus_connect_irq(SYS_BUS_DEVICE(host), 0,
                           qemu_allocate_irq(a6_irq, s, 12));
        if (s->hsic_ready_probe) {
            s5l8930_gpio_set_rest_level(gpio, 211, false);
            s5l8930_gpio_set_rest_level(gpio, 57, false);
            qdev_connect_gpio_out(gpio, 17,
                                  qemu_allocate_irq(a6_hsic_ready, s, 0));
            qdev_connect_gpio_out(gpio, 54,
                                  qemu_allocate_irq(a6_hsic_ready, s, 1));
            qdev_connect_gpio_out_named(s->pmu, "gpio", 10,
                                        qemu_allocate_irq(a6_wlan_reset, s, 0));
        }
        if (s->baseband) {
            DeviceState *bb = qdev_new(TYPE_IOS_BASEBAND);
            USBDevice *modem = USB_DEVICE(qdev_new("usb-mdm9x15"));

            object_property_set_int(OBJECT(bb), "answer-delay-ms", 1000,
                                    &error_fatal);
            object_property_set_int(OBJECT(bb), "signal-dbm", -51,
                                    &error_fatal);
            object_property_set_str(OBJECT(bb), "smsc", "+10000000000",
                                    &error_fatal);
            object_property_set_bool(OBJECT(bb), "gps", true, &error_fatal);
            object_property_add_child(OBJECT(s), "baseband-modem", OBJECT(bb));
            qdev_realize_and_unref(bb, NULL, &error_fatal);
            s->baseband_usb = DEVICE(modem);
            object_property_set_link(OBJECT(modem), "baseband", OBJECT(bb),
                                     &error_fatal);
            object_property_set_bool(OBJECT(modem), "managed-power", true,
                                     &error_fatal);
            qdev_prop_set_string(DEVICE(modem), "port", "2");
            object_property_add_child(OBJECT(s), "baseband-usb", OBJECT(modem));
            /* Original N42 DT: PMU GPIO2 supplies the modem; GPIO25.4
             * releases active-low reset; GPIO5.5 is active-low reset detect
             * (10A405 resetDetectInterrupt reports a reset on its falling edge). */
            qdev_connect_gpio_out_named(
                s->pmu, "gpio", 2,
                qdev_get_gpio_in_named(DEVICE(modem), "power", 0));
            /* nRESET is open-drain: input mode releases the pull-up. */
            qdev_connect_gpio_out_named(gpio, "pad", 204,
                                        qemu_irq_invert(qdev_get_gpio_in_named(
                                            DEVICE(modem), "reset", 0)));
            qdev_connect_gpio_out_named(DEVICE(modem), "device-ready", 0,
                                        qdev_get_gpio_in(gpio, 211));
            s5l8930_gpio_set_rest_level(gpio, 45, false);
            qdev_connect_gpio_out_named(
                DEVICE(modem), "reset-detect", 0,
                qemu_irq_invert(qdev_get_gpio_in(gpio, 45)));
            usb_realize_and_unref(modem, s->hsic_bus, &error_fatal);
            /* N42's /dev/cu.debug is the modem UART, not the boot console
             * on UART6. ETL bootstraps the H5 control link here, while the
             * same modem exposes its auxiliary QMI/data channels on HSIC. */
            /* The native UART driver reports16-byte FIFOs (80a4c2ac),
             * unlike the256-byte UARTs used by the older Bluetooth board. */
            modem_uart = qdev_new("exynos4210.uart");
            qdev_prop_set_bit(modem_uart, "s5l8720-irq", true);
            /* As with the existing M68 UART modem, this bounded virtual
             * peer accepts AP writes and holds clear-to-send asserted. */
            qdev_prop_set_bit(modem_uart, "cts", true);
            qdev_prop_set_chr(modem_uart, "chardev",
                              mdm9x15_chardev(DEVICE(modem)));
            qdev_prop_set_uint32(modem_uart, "channel", 1);
            qdev_prop_set_uint32(modem_uart, "rx-size", 16);
            qdev_prop_set_uint32(modem_uart, "tx-size", 16);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(modem_uart), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(modem_uart), 0, 0x32600000);
            sysbus_connect_irq(SYS_BUS_DEVICE(modem_uart), 0,
                               qemu_allocate_irq(a6_irq, s, 0x16));
        }
        for (int i = 0; i < 2; i++) {
            host = qdev_new(TYPE_SYSBUS_OHCI);
            qdev_prop_set_uint32(host, "num-ports", 1);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(host), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(host), 0, 0x36500000 + i * 0x100000);
            sysbus_connect_irq(SYS_BUS_DEVICE(host), 0,
                               qemu_allocate_irq(a6_irq, s, 13 + i));
        }
    }
    /* N42AP's boot-console is UART6, not A4's UART0. */
    exynos4210_uart_create(0x32b00000, 256, 6, serial_hd(0),
                           qemu_allocate_irq(a6_irq, s, 0x1b), true);
    if (s->battery_probe) {
        /* Original N42 10A405 DT: UART5 +02a00000, IRQ26, and child
         * gas-gauge,bq27540|gas-gauge,hdq. Reuse the same UART/HDQ wire
         * protocol and 16-byte FIFO as the older qemu-ios boards.
         * Capacity is an explicit virtual 1440mAh fixture, not a measured
         * N42 pack's calibrated data-flash contents. */
        s->gauge = qemu_chardev_new(NULL, TYPE_CHARDEV_S5L8930_HDQ, NULL, NULL,
                                    &error_fatal);
        s5l8930_hdq_set_capacity(s->gauge, 1440);
        s5l8930_hdq_set_battery(s->gauge, 100, true);
        exynos4210_uart_create(0x32a00000, 16, 5, s->gauge,
                               qemu_allocate_irq(a6_irq, s, 26), true);
    }
    for (int i = 0; i < 2; i++) {
        DeviceState *timers = qdev_new(TYPE_OR_IRQ);
        qdev_prop_set_uint16(timers, "num-lines", 2);
        qdev_realize_and_unref(timers, NULL, &error_fatal);
        qdev_connect_gpio_out(DEVICE(s->cpu[i]), GTIMER_PHYS,
                              qdev_get_gpio_in(timers, 0));
        qdev_connect_gpio_out(DEVICE(s->cpu[i]), GTIMER_VIRT,
                              qdev_get_gpio_in(timers, 1));
        qdev_connect_gpio_out(timers, 0,
                              qdev_get_gpio_in(DEVICE(s->cpu[i]), ARM_CPU_FIQ));
    }
    DeviceState *cdma = qdev_new(TYPE_S5L8930_CDMA);
    qdev_prop_set_uint64(cdma, "dram-base", A6_RAM_BASE);
    qdev_prop_set_uint64(cdma, "dram-size", A6_RAM_SIZE);
    /* N42 UART0..6 retain URXH +0x24 at their A6 MMIO addresses. An
     * unconnected UART RX must wait for bytes, as on the older boards. */
    qdev_prop_set_uint64(cdma, "uart-base", 0x32500000);
    qdev_prop_set_uint32(cdma, "uart-ports", 7);
    /* Original N42 CDMA v5: max-channel 0x2b, IRQs 51..93. */
    qdev_prop_set_uint32(cdma, "num-channels", A6_CDMA_CHANNELS);
    qdev_prop_set_uint8(cdma, "version", 5);
    qdev_prop_set_bit(cdma, "nand-file-aes", true);
    if (s->audio_ports_probe) {
        qdev_prop_set_uint32(cdma, "paced-base", 0x34190000);
        qdev_prop_set_uint32(cdma, "paced-ports", 8);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(cdma), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(cdma), 0, 0x37000000);
    sysbus_mmio_map(SYS_BUS_DEVICE(cdma), 1, 0x37800000);
    for (int i = 1; i < A6_CDMA_CHANNELS; i++)
        sysbus_connect_irq(SYS_BUS_DEVICE(cdma), i,
                           qemu_allocate_irq(a6_irq, s, 50 + i));
    if (modem_uart) {
        /* Match the existing Samsung UART/CDMA receive path used by the
         * older boards. N42's UART1 descriptor names URXH32600024. Kick
         * outside the UART callback because CDMA reads re-enter the UART. */
        QEMUBH *kick = qemu_bh_new(a6_uart_cdma_kick, cdma);

        s5l8930_cdma_set_source(cdma, 0x32600024, 4, a6_uart_rx_avail, NULL);
        sysbus_connect_irq(SYS_BUS_DEVICE(modem_uart), 2,
                           qemu_allocate_irq(a6_uart_rxdma, kick, 0));
    }
    DeviceState *nand_store = qdev_new(TYPE_S5L8930_IOP);
    if (s->nand)
        qdev_prop_set_string(nand_store, "nand", s->nand);
    if (s->ppn) {
        qdev_prop_set_uint32(nand_store, "nand-id", 0x014e5050);
        qdev_prop_set_uint8(nand_store, "nand-ce-mask", 1);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(nand_store), &error_fatal);
    DeviceState *fmi = qdev_new(TYPE_S5L8930_H2FMI);
    qdev_prop_set_uint64(fmi, "mmio-base", 0x31200000);
    qdev_prop_set_bit(fmi, "explicit-start", true);
    qdev_prop_set_bit(fmi, "ppn", s->ppn);
    object_property_set_link(OBJECT(fmi), "cdma", OBJECT(cdma), &error_abort);
    object_property_set_link(OBJECT(fmi), "iop", OBJECT(nand_store),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(fmi), &error_fatal);
    for (int i = 0; i < 2; i++) {
        sysbus_mmio_map(SYS_BUS_DEVICE(fmi), i, 0x31200000 + i * 0x100000);
        sysbus_connect_irq(SYS_BUS_DEVICE(fmi), i,
                           qemu_allocate_irq(a6_irq, s, 33 + i));
    }
    /* Separate IOP address space: shared DRAM uses the firmware's c0000000
     * window. Cortex-A9 is an ISA bring-up probe pending core-ID validation. */
    memory_region_init(&s->iop_mem, OBJECT(s), "a6.iop-memory", 1ULL << 32);
    memory_region_init_alias(&s->iop_dram, OBJECT(s), "a6.iop-dram", &s->ram, 0,
                             A6_RAM_SIZE);
    memory_region_add_subregion(&s->iop_mem, 0xc0000000, &s->iop_dram);
    memory_region_init_alias(&s->iop_phys_dram, OBJECT(s),
                             "a6.iop-physical-dram", &s->ram, 0, A6_RAM_SIZE);
    memory_region_add_subregion(&s->iop_mem, A6_RAM_BASE, &s->iop_phys_dram);
    memory_region_init_alias(&s->iop_periph, OBJECT(s), "a6.iop-peripherals",
                             memory, 0x30000000, 0x10000000);
    memory_region_add_subregion(&s->iop_mem, 0x30000000, &s->iop_periph);
    Object *iop = object_new(ARM_CPU_TYPE_NAME("cortex-a9"));
    s->iop_cpu = ARM_CPU(iop);
    s->iop_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, a6_iop_timer_expired, s);
    object_property_set_link(iop, "memory", OBJECT(&s->iop_mem), &error_abort);
    object_property_set_bool(iop, "start-powered-off", true, &error_abort);
    qdev_realize(DEVICE(iop), NULL, &error_fatal);
    object_unref(iop);
    memory_region_init_io(&s->iop_ctrl, OBJECT(s), &a6_iop_ops, s,
                          "a6.iop-control", 0x1000);
    memory_region_add_subregion_overlap(memory, 0x3f300000, &s->iop_ctrl, 1);
    memory_region_init_alias(&s->iop_private, OBJECT(s),
                             "a6.iop-private-control", &s->iop_ctrl, 0, 0x1000);
    memory_region_add_subregion(&s->iop_mem, 0x10040000, &s->iop_private);
    if (s->ae2_probe) {
        a6_ae2_init(s, memory);
    }
    if (s->camera) {
        sysbus_create_simple("img.topaz-hp", 0x38000000,
                             qemu_allocate_irq(a6_irq, s, 48));
        /* N42's original DT names the same MSVDX/MTX decoder family.
         * Camera Roll also needs it to decode recorded-movie thumbnails. */
        sysbus_create_simple("s5l8920.vxd", 0x38100000,
                             qemu_allocate_irq(a6_irq, s, 50));
        DeviceState *isp_dart =
            sysbus_create_simple(TYPE_S5L8930_DART, 0x3a400000, NULL);
        DeviceState *isp = qdev_new("apple-h4-isp");
        object_property_set_link(OBJECT(isp), "dart", OBJECT(isp_dart),
                                 &error_fatal);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(isp), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(isp), 0, 0x3a000000);
        sysbus_connect_irq(SYS_BUS_DEVICE(isp), 0,
                           qemu_allocate_irq(a6_irq, s, 7));
    }
    if (s->display_probe) {
        /* H5P shares the pipe's UI address/stride, packet FIFO and basic
         * CLCD ENVID/size registers with H3. This probe reuses those modeled
         * blocks while qualifying the additional H5P register/IRQ contract.
         * It is not yet a claim of complete A6 display hardware support. */
        DeviceState *display = qdev_new(TYPE_S5L8930_DISPLAY);
        qdev_prop_set_bit(display, "h5p", true);
        qdev_prop_set_uint64(display, "fb-base", 0xbf700000);
        qdev_prop_set_uint16(display, "width", 640);
        qdev_prop_set_uint16(display, "height", 1136);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(display), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(display), 0, 0x3a100000);
        sysbus_mmio_map(SYS_BUS_DEVICE(display), 1, 0x39200000);
        /* N42 clcd/dma-channels names channel 40's destination explicitly
         * as 3a20103c. Route that DMA FIFO aperture to the same pipe command
         * FIFO the AP accesses at 3a10103c. Other offsets in the DMA aperture
         * remain unmodeled rather than aliasing an unqualified register bank. */
        memory_region_init_alias(
            &s->display_dma_fifo, OBJECT(s), "a6.display-dma-fifo",
            sysbus_mmio_get_region(SYS_BUS_DEVICE(display), 0), 0x103c, 4);
        memory_region_add_subregion_overlap(memory, 0x3a20103c,
                                            &s->display_dma_fifo, 1);
        sysbus_connect_irq(SYS_BUS_DEVICE(display), 0,
                           qemu_allocate_irq(a6_irq, s, 43));
        sysbus_connect_irq(SYS_BUS_DEVICE(display), 1,
                           qemu_allocate_irq(a6_irq, s, 42));
        /* Original N42 DT: Samsung DSIM-1, four lanes, first reg window
         * 0x39500000, IRQ 41. The native AppleSamsungMIPIDSIController uses
         * the shared STATUS/CLKCTRL/PKTHDR register contract. The second
         * DT window (0x3f110000) is the separate, unmodeled SAC block. */
        DeviceState *dsi = qdev_new(TYPE_IPOD_TOUCH_MIPI_DSI);
        IPOD_TOUCH_MIPI_DSI(dsi)->hs_clock_at_reset = true;
        qdev_prop_set_uint32(dsi, "lanes", 4);
        qdev_prop_set_uint32(dsi, "panel-id", 0x00a1d13c);
        qdev_prop_set_uint32(dsi, "panel-id-len", 4);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dsi), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dsi), 0, 0x39500000);
        sysbus_connect_irq(SYS_BUS_DEVICE(dsi), 0,
                           qemu_allocate_irq(a6_irq, s, 41));
        /* Original N42 scaler/dart-nrt compatibility includes the shared
         * Samsung M2 and S5L8930 DART interfaces. Mapper-scaler uses SIDs 2/3;
         * the existing scaler model consumes SID 2. Its reported 0x20007 is
         * the implemented virtual block version, not a measured N42 value. */
        DeviceState *dart =
            sysbus_create_simple(TYPE_S5L8930_DART, 0x38b00000, NULL);
        /* N42's two JPEG providers retain the Samsung codec interface and
         * share mapper-jpeg (streams 0/1) on dart-nrt. */
        for (unsigned i = 0; i < 2; i++) {
            DeviceState *jpeg = qdev_new("s5l8920.jpeg");
            qdev_prop_set_uint32(jpeg, "dart-sid", i);
            s5l8920_jpeg_set_iommu(jpeg, a6_jpeg_xlate, dart);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(jpeg), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(jpeg), 0,
                            i ? 0x38500000 : 0x38200000);
            sysbus_connect_irq(SYS_BUS_DEVICE(jpeg), 0,
                               qemu_allocate_irq(a6_irq, s, i ? 126 : 118));
        }
        /* Both DT siblings are named "scaler": retain their distinct reg,
         * interrupt and device_type properties. Native HAL objects confirm
         * scaler0 at 0x38300000 and scaler1 at 0x38400000. Both providers
         * reference the same mapper-scaler (streams 2/3). */
        for (unsigned i = 0; i < 2; i++) {
            DeviceState *scaler = sysbus_create_simple(
                "ipodtouch.scaler", 0x38300000 + i * 0x100000,
                qemu_allocate_irq(a6_irq, s, 9 + i));
            ipod_scaler_set_iommu(scaler, a6_scaler_xlate, dart, 2);
            ipod_scaler_set_version(scaler, 0x20007);
        }
    } else {
        s->console = graphic_console_init(NULL, 0, &a6_display_ops, s);
        qemu_console_resize(s->console, 640, 1136);
    }
    qemu_register_reset(a6_reset, s);
    qemu_register_powerdown_notifier(&a6_powerdown_notifier);
}

static char *a6_get_kboot(Object *obj, Error **errp)
{
    return g_strdup(S5L8950_MACHINE(obj)->kboot);
}
static void a6_set_kboot(Object *obj, const char *path, Error **errp)
{
    S5L8950State *s = S5L8950_MACHINE(obj);
    g_free(s->kboot);
    s->kboot = g_strdup(path);
}
static char *a6_get_nand(Object *obj, Error **errp)
{
    return g_strdup(S5L8950_MACHINE(obj)->nand);
}
static void a6_set_nand(Object *obj, const char *path, Error **errp)
{
    S5L8950State *s = S5L8950_MACHINE(obj);
    g_free(s->nand);
    s->nand = g_strdup(path);
}
static char *a6_get_restore_image(Object *obj, Error **errp)
{
    return g_strdup(S5L8950_MACHINE(obj)->restore_image);
}
static void a6_set_restore_image(Object *obj, const char *path, Error **errp)
{
    S5L8950State *s = S5L8950_MACHINE(obj);
    g_free(s->restore_image);
    s->restore_image = g_strdup(path);
}
static bool a6_get_ppn(Object *obj, Error **errp)
{
    return S5L8950_MACHINE(obj)->ppn;
}
static void a6_set_ppn(Object *obj, bool value, Error **errp)
{
    S5L8950_MACHINE(obj)->ppn = value;
}
static bool a6_get_camera(Object *obj, Error **errp)
{
    return S5L8950_MACHINE(obj)->camera;
}
static void a6_set_camera(Object *obj, bool value, Error **errp)
{
    S5L8950_MACHINE(obj)->camera = value;
}
static bool a6_get_display_probe(Object *obj, Error **errp)
{
    return S5L8950_MACHINE(obj)->display_probe;
}
static void a6_set_display_probe(Object *obj, bool value, Error **errp)
{
    S5L8950_MACHINE(obj)->display_probe = value;
}
static bool a6_get_ae2_probe(Object *obj, Error **errp)
{
    return S5L8950_MACHINE(obj)->ae2_probe;
}
static void a6_set_ae2_probe(Object *obj, bool value, Error **errp)
{
    S5L8950_MACHINE(obj)->ae2_probe = value;
}
static bool a6_get_pmu_adc_probe(Object *obj, Error **errp)
{
    return S5L8950_MACHINE(obj)->pmu_adc_probe;
}
static void a6_set_pmu_adc_probe(Object *obj, bool value, Error **errp)
{
    S5L8950_MACHINE(obj)->pmu_adc_probe = value;
}
static bool a6_get_tristar_probe(Object *obj, Error **errp)
{
    return S5L8950_MACHINE(obj)->tristar_probe;
}
static void a6_set_tristar_probe(Object *obj, bool value, Error **errp)
{
    S5L8950_MACHINE(obj)->tristar_probe = value;
}
static bool a6_get_usb_host_probe(Object *obj, Error **errp)
{
    return S5L8950_MACHINE(obj)->usb_host_probe;
}
static void a6_set_usb_host_probe(Object *obj, bool value, Error **errp)
{
    S5L8950_MACHINE(obj)->usb_host_probe = value;
}
static bool a6_get_hsic_ready_probe(Object *obj, Error **errp)
{
    return S5L8950_MACHINE(obj)->hsic_ready_probe;
}
static void a6_set_hsic_ready_probe(Object *obj, bool value, Error **errp)
{
    S5L8950_MACHINE(obj)->hsic_ready_probe = value;
}
static bool a6_get_baseband(Object *obj, Error **errp)
{
    return S5L8950_MACHINE(obj)->baseband;
}
static void a6_set_baseband(Object *obj, bool value, Error **errp)
{
    S5L8950_MACHINE(obj)->baseband = value;
}
static bool a6_get_audio_ports_probe(Object *obj, Error **errp)
{
    return S5L8950_MACHINE(obj)->audio_ports_probe;
}
static bool a6_get_backlight_probe(Object *obj, Error **errp)
{
    return S5L8950_MACHINE(obj)->backlight_probe;
}
static void a6_set_backlight_probe(Object *obj, bool value, Error **errp)
{
    S5L8950_MACHINE(obj)->backlight_probe = value;
}
static bool a6_get_battery_probe(Object *obj, Error **errp)
{
    return S5L8950_MACHINE(obj)->battery_probe;
}
static void a6_set_battery_probe(Object *obj, bool value, Error **errp)
{
    S5L8950_MACHINE(obj)->battery_probe = value;
}
static void a6_set_audio_ports_probe(Object *obj, bool value, Error **errp)
{
    if (S5L8950_MACHINE(obj)->cpu[0]) {
        error_setg(
            errp,
            "audio-ports-probe must be set before machine initialization");
        return;
    }
    S5L8950_MACHINE(obj)->audio_ports_probe = value;
}
static char *a6_get_timer_status(Object *obj, Error **errp)
{
    S5L8950State *s = S5L8950_MACHINE(obj);
    int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t ticks = ns / a6_counter_period_ns(s);
    GString *out = g_string_new(NULL);
    g_string_append_printf(out,
                           "{\"virtual_ns\":%" PRId64
                           ",\"counter_ticks\":%" PRIu64 ",\"cpus\":[",
                           ns, ticks);
    for (int i = 0; i < 2; i++) {
        CPUARMState *env = &s->cpu[i]->env;
        /* Expose the actual shared AIC/generic clock so CVAL comparisons
         * use the same domain as the guest's timebase counter. */
        unsigned period = gt_cntfrq_period_ns(s->cpu[i]);
        g_string_append_printf(
            out,
            "%s{\"counter_period_ns\":%u"
            ",\"generic_counter_ticks\":%" PRIu64 ",\"physical_ctl\":%" PRIu64
            ",\"physical_cval\":%" PRIu64 ",\"virtual_ctl\":%" PRIu64
            ",\"virtual_cval\":%" PRIu64 ",\"virtual_offset\":%" PRIu64
            ",\"aic_local\":[%u,%u,%u]}",
            i ? "," : "", period, (uint64_t)ns / period,
            env->cp15.c14_timer[GTIMER_PHYS].ctl,
            env->cp15.c14_timer[GTIMER_PHYS].cval,
            env->cp15.c14_timer[GTIMER_VIRT].ctl,
            env->cp15.c14_timer[GTIMER_VIRT].cval, env->cp15.cntvoff_el2,
            s->local_timer[i][0], s->local_timer[i][1], s->local_timer[i][2]);
    }
    g_string_append(out, "]}");
    return g_string_free(out, false);
}
/* N42's original DT buttons functions use GPIO 0/1/2/3 for menu, hold,
 * volume up and volume down, respectively. All have active-low flags.
 * This drives the external pads through the GPIO controller; the original
 * button driver handles debounce, interrupts and lock/wake behavior.
 * Home/Hold also latch PMU event bits; the deep AP power cycle remains
 * separate from these awake GPIO pads. */
void s5l8950_press_button(IPodTouchButton button, bool down)
{
    static const unsigned pins[] = {0, 1, 2, 3};
    S5L8950State *s = (S5L8950State *)object_dynamic_cast(
        OBJECT(qdev_get_machine()), TYPE_S5L8950_MACHINE);

    if (!s || !s->gpio || (unsigned)button >= ARRAY_SIZE(pins)) {
        return;
    }
    bool was_down = s->button_down[button];
    s->button_down[button] = down;
    if (down && !was_down && s->pmu &&
        (button == IPOD_TOUCH_BUTTON_HOME ||
         button == IPOD_TOUCH_BUTTON_POWER)) {
        a6_pmu_button(s->pmu, button == IPOD_TOUCH_BUTTON_POWER);
    }
    qemu_set_irq(qdev_get_gpio_in(s->gpio, pins[button]), !down);
}

static bool a6_get_button_home(Object *obj, Error **errp)
{
    return S5L8950_MACHINE(obj)->button_down[IPOD_TOUCH_BUTTON_HOME];
}

static void a6_set_button_home(Object *obj, bool value, Error **errp)
{
    s5l8950_press_button(IPOD_TOUCH_BUTTON_HOME, value);
}

/* The shared models' A6 behavior, on this machine only: every other board keeps what it was qualified with. */
static GlobalProperty a6_device_defaults[] = {
    { "exynos4210.uart", "a6-serial", "on" },
    { TYPE_S5L8930_CDMA, "a6", "on" },
    { "s5l8920.vxd", "a6", "on" },
    { TYPE_EXYNOS4210_EHCI, "skip-idle-periodic-qh", "on" },
};

static void a6_class_init(ObjectClass *klass, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);
    compat_props_add(mc->compat_props, a6_device_defaults, G_N_ELEMENTS(a6_device_defaults));
    gles_host_class_init(klass);
    mc->desc = "iPhone 5 (N42AP, S5L8950/A6)";
    mc->init = a6_init;
    mc->reset = a6_machine_reset;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("apple-swift");
    /* QEMU counts the storage IOP alongside the two application cores. */
    /* Two application CPUs, the ISP IOP, and the optional AE2 audio CPU.
     * MTTCG reserves one translation context per advertised maximum CPU. */
    mc->default_cpus = 4;
    mc->max_cpus = 4;
    mc->minimum_page_bits = 10;
    mc->default_ram_size = A6_RAM_SIZE;
    object_class_property_add_str(klass, "kboot", a6_get_kboot, a6_set_kboot);
    object_class_property_add_str(klass, "nand", a6_get_nand, a6_set_nand);
    object_class_property_add_bool(klass, "ppn", a6_get_ppn, a6_set_ppn);
    object_class_property_add_bool(klass, "camera", a6_get_camera,
                                   a6_set_camera);
    object_class_property_add_bool(klass, "display-probe", a6_get_display_probe,
                                   a6_set_display_probe);
    object_class_property_add_bool(klass, "ae2-probe", a6_get_ae2_probe,
                                   a6_set_ae2_probe);
    object_class_property_add_bool(klass, "pmu-adc-probe", a6_get_pmu_adc_probe,
                                   a6_set_pmu_adc_probe);
    object_class_property_add_bool(klass, "tristar-probe", a6_get_tristar_probe,
                                   a6_set_tristar_probe);
    object_class_property_add_bool(
        klass, "usb-host-probe", a6_get_usb_host_probe, a6_set_usb_host_probe);
    object_class_property_add_bool(klass, "hsic-ready-probe",
                                   a6_get_hsic_ready_probe,
                                   a6_set_hsic_ready_probe);
    object_class_property_add_bool(klass, "baseband", a6_get_baseband,
                                   a6_set_baseband);
    object_class_property_add_bool(klass, "audio-ports-probe",
                                   a6_get_audio_ports_probe,
                                   a6_set_audio_ports_probe);
    object_class_property_add_bool(klass, "backlight-probe",
                                   a6_get_backlight_probe,
                                   a6_set_backlight_probe);
    object_class_property_add_bool(klass, "battery-probe", a6_get_battery_probe,
                                   a6_set_battery_probe);
    object_class_property_add_str(klass, "timer-status", a6_get_timer_status,
                                  NULL);
    object_class_property_add_bool(klass, "button-home", a6_get_button_home,
                                   a6_set_button_home);
    object_class_property_add_str(klass, "restore-image", a6_get_restore_image,
                                  a6_set_restore_image);
}
static void a6_set_agent_request(Object *obj, const char *value, Error **errp)
{
    int error = ipod_agent_submit(S5L8950_MACHINE(obj)->agent, value);
    if (error) {
        error_setg(errp, "%s", ipod_agent_submit_error(error));
    }
}
static void a6_cancel_agent_request(Object *obj, const char *value,
                                    Error **errp)
{
    ipod_agent_cancel(S5L8950_MACHINE(obj)->agent, value);
}
static char *a6_get_agent_result(Object *obj, Error **errp)
{
    return ipod_agent_take_result(S5L8950_MACHINE(obj)->agent);
}
static char *a6_get_agent_status(Object *obj, Error **errp)
{
    return g_strdup(ipod_agent_status(S5L8950_MACHINE(obj)->agent,
                                      qemu_clock_get_ms(QEMU_CLOCK_REALTIME)));
}
static bool a6_get_quiet_boot(Object *obj, Error **errp)
{
    return S5L8950_MACHINE(obj)->quiet_boot;
}

static void a6_set_quiet_boot(Object *obj, bool value, Error **errp)
{
    S5L8950_MACHINE(obj)->quiet_boot = value;
}

static void a6_instance_init(Object *obj)
{
    S5L8950State *s = S5L8950_MACHINE(obj);
    object_property_add_bool(obj, "quiet-boot", a6_get_quiet_boot,
                             a6_set_quiet_boot);
    /* These switches originally gated hardware bring-up. The normal device
     * now includes that hardware; keep the properties for compatibility with
     * older launch configurations and targeted development. */
    s->display_probe = true;
    s->ae2_probe = true;
    s->pmu_adc_probe = true;
    s->tristar_probe = true;
    s->usb_host_probe = true;
    s->audio_ports_probe = true;
    s->backlight_probe = true;
    s->battery_probe = true;
    s->hsic_ready_probe = true;
    guest_pkg_init(&S5L8950_MACHINE(obj)->pkg, obj);
    guest_pb_init(&S5L8950_MACHINE(obj)->pb, obj, "iphone5");
    S5L8950_MACHINE(obj)->agent = ipod_agent_new();
    ipod_agent_publish(S5L8950_MACHINE(obj)->agent);
    object_property_add_str(obj, "agent-request", NULL, a6_set_agent_request);
    object_property_add_str(obj, "agent-cancel", NULL, a6_cancel_agent_request);
    object_property_add_str(obj, "agent-result", a6_get_agent_result, NULL);
    object_property_add_str(obj, "agent-status", a6_get_agent_status, NULL);
}
static void a6_instance_finalize(Object *obj)
{
    ipod_agent_publish(NULL);
    ipod_agent_free(S5L8950_MACHINE(obj)->agent);
    timer_free(S5L8950_MACHINE(obj)->ap_off_timer);
}

static const TypeInfo a6_machine = {
    .name = TYPE_S5L8950_MACHINE,
    .parent = TYPE_MACHINE,
    .interfaces = arm_machine_interfaces,
    .instance_size = sizeof(S5L8950State),
    .class_init = a6_class_init,
    .instance_init = a6_instance_init,
    .instance_finalize = a6_instance_finalize,
};
/* D1972 register interface with an opt-in ADC experiment. The original N42
 * driver starts conversions with register 48 bit 5 and consumes the completion
 * at event register 2 bit 5 (80bdc86c..c8de). Its result unpacking at
 * 80bde192..e1c0 defines a 12-bit value across registers 49/4a.
 * The configurable raw input supplies nonthermal channels. Separate virtual
 * calibration-resistor and NTC inputs keep the native thermal correction
 * meaningful; these are fixtures, not measurements of a physical phone. */
typedef struct A6PMU {
    I2CSlave parent_obj;
    uint8_t regs[256], reg;
    unsigned log_count;
    bool address;
    bool adc_probe;
    bool thermal_sensors;
    bool battery_swi;
    bool usb_power;
    uint16_t adc_input;
    uint32_t rtc_latch;
    qemu_irq irq, ap_power;
    qemu_irq gpio[12];
    bool ap_off, ap_waking;
    QEMUTimer *adc_timer;
} A6PMU;
static void a6_pmu_set_ap_power(A6PMU *s, bool on)
{
    if (!s->ap_power || s->ap_off != on) {
        return;
    }
    s->ap_off = !on;
    s->ap_waking = on;
    qemu_set_irq(s->ap_power, on);
}
static void a6_pmu_irq_update(A6PMU *s)
{
    bool pending = false;
    for (unsigned i = 0; i < 9; i++) {
        pending |= s->regs[1 + i] & ~s->regs[0x12 + i];
    }
    /* Original DT: PMU interrupt is GPIO 6, active low. */
    qemu_set_irq(s->irq, !pending);
    if (pending && s->ap_off) {
        a6_pmu_set_ap_power(s, true);
    }
}
static void a6_pmu_reassert(DeviceState *dev)
{
    a6_pmu_irq_update((A6PMU *)dev);
}
static void a6_pmu_cancel_resume(DeviceState *dev)
{
    A6PMU *s = (A6PMU *)dev;
    s->ap_waking = false;
    s->ap_off = false;
}
static bool a6_pmu_take_suspend(DeviceState *dev)
{
    A6PMU *s = (A6PMU *)dev;
    if ((s->regs[0xb0] & 0xd0) != 0x80) {
        return false;
    }
    /* N42 LLB 1000b7f4 consumes this marker with AND2f/OR40. */
    s->regs[0xb0] = (s->regs[0xb0] & 0x2f) | 0x40;
    s->ap_waking = false;
    s->ap_off = false;
    return true;
}
/* Original N42 wake decoder at 80bdd47a/80bdd492 maps event1 bits
 * 1/0 to hold/menu. Preserve the latch while masked, as with ADC events. */
static void a6_pmu_button(DeviceState *dev, bool hold)
{
    A6PMU *s = (A6PMU *)dev;
    s->regs[1] |= hold ? 2 : 1;
    a6_pmu_irq_update(s);
    /* Home/Hold are physical wake inputs even when IRQ delivery is masked. */
    if (s->ap_off) {
        a6_pmu_set_ap_power(s, true);
    }
}
static void a6_pmu_adc_complete(void *opaque)
{
    A6PMU *s = opaque;
    uint16_t raw = s->adc_input & 0xfff;
    unsigned channel = s->regs[0x48] & 0x1f;
    if (s->thermal_sensors) {
        /* N42's calibration resistor and external NTCs are separate analog
         * inputs. Equal counts make the native calibration correction force
         * the external sensors to the reference resistor's temperature. */
        if (channel == 10) {
            raw = 256;
        } else if (channel >= 11 && channel <= 14) {
            raw = 800;
        }
    }
    s->regs[0x49] = raw & 0xf;
    s->regs[0x4a] = raw >> 4;
    s->regs[0x48] &= ~0x20;
    s->regs[2] |= 0x20;
    a6_pmu_irq_update(s);
    qemu_log_mask(LOG_UNIMP, "A6 PMU ADC channel%u raw%u completed\n",
                  s->regs[0x48] & 0x1f, raw);
}
static int a6_pmu_event(I2CSlave *dev, enum i2c_event event)
{
    if (event == I2C_START_SEND)
        ((A6PMU *)dev)->address = true;
    return 0;
}
static int a6_pmu_send(I2CSlave *dev, uint8_t data)
{
    A6PMU *s = (A6PMU *)dev;
    if (s->address) {
        s->reg = data;
        s->address = false;
    } else {
        /* bounded: uint8_t reg addresses the 256-byte register file. */
        uint8_t reg = s->reg++, previous = s->regs[reg];
        trace_a6_pmu_register(
            true, reg, data,
            current_cpu ? CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : 0);
        /* bounded: uint8_t reg addresses the 256-byte register file. */
        s->regs[reg] = data;
        if (reg >= 0x68 && reg <= 0x73) {
            trace_a6_pmu_gpio(reg - 0x68, previous, data);
            qemu_set_irq(s->gpio[reg - 0x68],
                         (data & 0x60) == 0x60 || (data & 2));
        }
        if (reg == 0x1b && (data & 3)) {
            /* Original D1972: hibernate uses B0=80 then 1B|=2;
             * standby (80bdf914) uses 1B|=1 after flash shutdown.
             * Both rail commands are consumed, not persistent bits. */
            s->regs[reg] &= ~3;
            if (data & 1) {
                /* Native standby is the final shutdown operation. Hibernate
                 * uses bit 1 and retains the independently powered devices. */
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            } else {
                a6_pmu_set_ap_power(s, false);
            }
        }
        if (reg == 0x99 && (data & 0x29) == 0x29) {
            /* Original D1972 halt with VBUS writes B0=90, then uses
             * restart 99|=29 to enter iBoot's off-but-charging state.
             * As on the A4 PMU, this is a completed guest shutdown,
             * distinct from an ordinary restart or retained-RAM sleep. */
            if ((s->regs[0xb0] & 0xd0) == 0x90) {
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            } else {
                qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            }
        }
        if (s->adc_probe) {
            if (reg == 0x48) {
                if ((data & 0x20) && !(previous & 0x20)) {
                    timer_mod(s->adc_timer,
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
                } else if (!(data & 0x20)) {
                    timer_del(s->adc_timer);
                }
            }
        }
        a6_pmu_irq_update(s);
    }
    return 0;
}
/* N42 D1972 getRTC at 80bddfa0 reads64..67 twice until equal.
 * PMURTC adds its persisted offset at A5..A8 (80be56e4/80be57b4).
 * Reuse the older Dialog model's QEMU RTC clock, with a coherent LE read. */
static uint32_t a6_pmu_rtc_clock(void)
{
    struct tm tm;
    qemu_get_timedate(&tm, 0);
    return (uint32_t)mktimegm(&tm);
}
static uint8_t a6_pmu_recv(I2CSlave *dev)
{
    A6PMU *s = (A6PMU *)dev;
    /* bounded: uint8_t reg addresses the 256-byte register file. */
    uint8_t reg = s->reg++, value = s->regs[reg];
    /* D1972 STATUS A/B (original N42 11A465): power source reads
     * eight bytes from 0x0a at 80bdf460. 80be1f3a tests A bit3 for
     * VBUS, then 80be1f4e checks A bit6 for a selected/valid source;
     * 80be415c uses B bits1/2 for the charging phase.
     * This fixed virtual supply complements the full HDQ battery. */
    if (reg == 0x0a) {
        value = (value & ~0x48) | (s->usb_power ? 0x48 : 0);
    } else if (reg == 0x0b) {
        value = (value & ~0x06) | (s->usb_power ? 0x02 : 0);
    }
    if (s->battery_swi && reg == 0x0c) {
        /* Original N42 GPIO getter80b0b5ec reads pin5 in bank0C.
         * It releases the wire by setting control6D mode60 (input).
         * The HDQ line has a pull-up; output mode follows data bit1.
         * Preserve other GPIO inputs and let a driven reset go low. */
        uint8_t control = s->regs[0x6d];
        bool high = (control & 0x60) == 0x60 || (control & 2);
        value = (value & ~0x20) | (high ? 0x20 : 0);
    }
    if (reg == 0x64) {
        s->rtc_latch = a6_pmu_rtc_clock();
    }
    if (reg >= 0x64 && reg <= 0x67) {
        value = s->rtc_latch >> (8 * (reg - 0x64));
    }
    if (reg >= 1 && reg <= 9) {
        s->regs[reg] = 0;
        a6_pmu_irq_update(s);
    }
    trace_a6_pmu_register(
        false, reg, value,
        current_cpu ? CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : 0);
    return value;
}
static void a6_pmu_init(Object *obj)
{
    A6PMU *s = (A6PMU *)obj;
    qdev_init_gpio_out(DEVICE(obj), &s->irq, 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->ap_power, "ap-power", 1);
    qdev_init_gpio_out_named(DEVICE(obj), s->gpio, "gpio", 12);
    s->adc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, a6_pmu_adc_complete, s);
}
static void a6_pmu_reset(DeviceState *dev)
{
    A6PMU *s = (A6PMU *)dev;
    if (s->ap_waking) {
        s->ap_waking = false;
        s->ap_off = false;
        a6_pmu_irq_update(s);
        return;
    }
    timer_del(s->adc_timer);
    s->ap_off = false;
    s->rtc_latch = 0;
    memset(s->regs, 0, sizeof(s->regs));
    if (s->battery_swi) {
        s->regs[0x6d] = 0x60; /* released battery single-wire input */
    }
    memset(s->regs + 0x12, 0xff, 9);
    s->reg = 0;
    s->address = false;
    a6_pmu_irq_update(s);
}
static void a6_pmu_finalize(Object *obj)
{
    timer_free(((A6PMU *)obj)->adc_timer);
}
static const Property a6_pmu_properties[] = {
    DEFINE_PROP_BOOL("adc-probe", A6PMU, adc_probe, false),
    DEFINE_PROP_BOOL("thermal-sensors", A6PMU, thermal_sensors, true),
    DEFINE_PROP_BOOL("battery-swi", A6PMU, battery_swi, false),
    DEFINE_PROP_BOOL("usb-power", A6PMU, usb_power, true),
    DEFINE_PROP_UINT16("adc-input", A6PMU, adc_input, 0x800),
};
static void a6_pmu_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    sc->event = a6_pmu_event;
    sc->send = a6_pmu_send;
    sc->recv = a6_pmu_recv;
    device_class_set_props(DEVICE_CLASS(klass), a6_pmu_properties);
    device_class_set_legacy_reset(DEVICE_CLASS(klass), a6_pmu_reset);
}
static const TypeInfo a6_pmu_type = {
    .name = "a6.d1972-registers",
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(A6PMU),
    .class_init = a6_pmu_class_init,
    .instance_init = a6_pmu_init,
    .instance_finalize = a6_pmu_finalize,
};

/* Initial no-accessory TriStar register qualification fixture. The original
 * N42 13A344 AppleTHS7383::start reads register 0x0f as one byte and publishes
 * it as TriStar REV; the byte is not an acceptance test. Revision zero names
 * this virtual fixture, not a measured physical CBTL1608 revision. Accessory
 * detection, interrupt status and electrical mux behavior remain unqualified. */
typedef struct A6TriStar {
    I2CSlave parent_obj;
    uint8_t regs[256], reg;
    unsigned log_count;
    bool address;
    qemu_irq irq;
    bool attached_notified;
} A6TriStar;

static int a6_tristar_event(I2CSlave *dev, enum i2c_event event)
{
    if (event == I2C_START_SEND) {
        ((A6TriStar *)dev)->address = true;
    }
    return 0;
}

static int a6_tristar_send(I2CSlave *dev, uint8_t value)
{
    A6TriStar *s = (A6TriStar *)dev;
    if (s->address) {
        s->reg = value;
        s->address = false;
    } else {
        if (s->log_count < 128) {
            s->log_count++;
            qemu_log_mask(LOG_UNIMP, "A6 TriStar write reg=%02x value=%02x\n",
                          s->reg, value);
        }
        if (s->reg != 0x0f) {
            s->regs[s->reg] = value;
        }
        s->reg++;
    }
    return 0;
}

static uint8_t a6_tristar_recv(I2CSlave *dev)
{
    A6TriStar *s = (A6TriStar *)dev;
    uint8_t value = s->reg == 0x0f ? 0 : s->regs[s->reg];
    if (s->reg == 0x0c) {
        value = 0x83;
        if (!s->attached_notified) {
            /* THS7383 interrupt status0D bit2: connection changed.
             * Deliver after the initial status query, so the driver's
             * startup read/ack cannot lose the attachment notification.
             * Bit3 also announces the already-populated digital-ID mailbox;
             * otherwise iOS7 clears its cached ID on bit2 and waits forever. */
            s->attached_notified = true;
            s->regs[0x0d] |= 0x0c;
            qemu_set_irq(s->irq, 1);
        }
    } else if (s->reg == 0x0d) {
        s->regs[0x0d] = 0;
        qemu_set_irq(s->irq, 0);
    }
    if (s->log_count < 128) {
        s->log_count++;
        qemu_log_mask(LOG_UNIMP, "A6 TriStar read reg=%02x value=%02x\n",
                      s->reg, value);
    }
    s->reg++;
    return value;
}

static void a6_tristar_init(Object *obj)
{
    A6TriStar *s = (A6TriStar *)obj;
    qdev_init_gpio_out(DEVICE(obj), &s->irq, 1);
}

static void a6_tristar_reset(DeviceState *dev)
{
    A6TriStar *s = (A6TriStar *)dev;
    memset(s->regs, 0, sizeof(s->regs));
    /* AppleTHS7383's original getSystemID reads the initial three-byte
     * mailbox at 0x20: message 0x74, followed by ID bits [9:2] and [1:0].
     * This fixture's virtual ID is zero; physical board strap values and
     * later accessory mailbox messages are not yet qualified. */
    s->regs[0x20] = 0x74;
    /* Original THS7383 getAccessoryID reads 39..3F, requires a 75
     * response at3F and reverses39..3E into the six-byte identity.
     * Standard Lightning USB cable ID 10 0C 00 00 00 00, as observed
     * in https://nyansatan.github.io/lightning/ . CRC is consumed by
     * the controller before exposing this receive window. */
    s->regs[0x3f] = 0x75;
    s->regs[0x3e] = 0x10;
    s->regs[0x3d] = 0x0c;
    s->attached_notified = false;
    qemu_set_irq(s->irq, 0);
    s->log_count = 0;
    s->reg = 0;
    s->address = false;
}

static void a6_tristar_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    sc->event = a6_tristar_event;
    sc->send = a6_tristar_send;
    sc->recv = a6_tristar_recv;
    device_class_set_legacy_reset(DEVICE_CLASS(klass), a6_tristar_reset);
}

static const TypeInfo a6_tristar_type = {
    .name = "a6.tristar-registers",
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(A6TriStar),
    .class_init = a6_tristar_class_init,
    .instance_init = a6_tristar_init,
};

/* Diagnostic LM3534 transport. Run45 native BackBoard kernel stack waits
 * indefinitely for IOFunctionParent2B000000 (DT lm3534 phandle). Retest the
 * previously inconclusive endpoint now that UI touch is qualified. Register
 * storage is not a complete analog chip model; keep gated until wake passes. */
typedef struct A6Backlight {
    I2CSlave parent_obj;
    uint8_t regs[256], reg;
    bool address;
    unsigned log_count;
} A6Backlight;

static int a6_backlight_event(I2CSlave *dev, enum i2c_event event)
{
    if (event == I2C_START_SEND) {
        ((A6Backlight *)dev)->address = true;
    }
    return 0;
}

static int a6_backlight_send(I2CSlave *dev, uint8_t value)
{
    A6Backlight *s = (A6Backlight *)dev;
    if (s->address) {
        s->reg = value;
        s->address = false;
    } else {
        if (s->log_count++ < 128) {
            qemu_log_mask(LOG_UNIMP, "A6 backlight probe write %02x=%02x\n",
                          s->reg, value);
        }
        /* bounded: uint8_t reg wraps within the 256-byte register file. */
        s->regs[s->reg++] = value;
    }
    return 0;
}

static uint8_t a6_backlight_recv(I2CSlave *dev)
{
    A6Backlight *s = (A6Backlight *)dev;
    /* bounded: uint8_t reg addresses the 256-byte register file. */
    uint8_t value = s->regs[s->reg];
    if (s->log_count++ < 128) {
        qemu_log_mask(LOG_UNIMP, "A6 backlight probe read %02x=%02x\n", s->reg,
                      value);
    }
    s->reg++;
    return value;
}

static void a6_backlight_reset(DeviceState *dev)
{
    A6Backlight *s = (A6Backlight *)dev;
    memset(s->regs, 0, sizeof(s->regs));
    s->reg = 0;
    s->address = false;
    s->log_count = 0;
}

static void a6_backlight_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    sc->event = a6_backlight_event;
    sc->send = a6_backlight_send;
    sc->recv = a6_backlight_recv;
    device_class_set_legacy_reset(DEVICE_CLASS(klass), a6_backlight_reset);
}

static const TypeInfo a6_backlight_type = {
    .name = "a6.backlight-registers",
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(A6Backlight),
    .class_init = a6_backlight_class_init,
};

static void a6_i2c_init(Object *obj)
{
    A6I2C *s = (A6I2C *)obj;
    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
    memory_region_init_io(&s->mmio, obj, &a6_i2c_ops, s, "a6.i2c", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}
static const TypeInfo a6_i2c_type = {
    .name = "a6.i2c",
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(A6I2C),
    .instance_init = a6_i2c_init,
};
static void a6_register_types(void)
{
    type_register_static(&a6_pmu_type);
    type_register_static(&a6_tristar_type);
    type_register_static(&a6_backlight_type);
    type_register_static(&a6_i2c_type);
    type_register_static(&a6_machine);
}
type_init(a6_register_types)
