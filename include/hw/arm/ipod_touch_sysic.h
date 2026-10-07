#ifndef IPOD_TOUCH_SYSIC_H
#define IPOD_TOUCH_SYSIC_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"

#define TYPE_IPOD_TOUCH_SYSIC                "ipodtouch.sysic"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchSYSICState, IPOD_TOUCH_SYSIC)

#define POWER_ID 0x44
#define POWER_ONCTRL 0xC
#define POWER_OFFCTRL 0x10
#define POWER_SETSTATE 0x8
#define POWER_STATE 0x14 // seems to be toggled by writing a 1 to the right device ID - cleared to 0 when the device has started.

#define POWER_ID_ADM 0x10

// the GPIO IC is part of the system controller
#define GPIO_INTLEVEL 0x80
#define GPIO_INTSTAT  0xA0
#define GPIO_INTEN    0xC0
#define GPIO_INTTYPE  0xE0

#define GPIO_NUMINTGROUPS 7
#define GPIO_NUMINTGROUPS_2 5

typedef struct IPodTouchSYSICState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    bool direct_boot; /* Startup board compatibility policy. */
    uint32_t epoch;   /* POWER_ID[31:24] to synthesise on a direct boot: the staged iBoot's
                       * own security epoch (it_iboot_find_epoch), which the LLB would have latched */
    bool s5l8900;     /* "s5l8900" property: +0xC powers down, +0x10 up, STATE is the on-mask */
    qemu_irq gpio_irqs[GPIO_NUMINTGROUPS];
    uint32_t power_id;
    uint32_t power_state;

    // GPIO
    uint32_t gpio_int_level[GPIO_NUMINTGROUPS];
    uint32_t gpio_int_status[GPIO_NUMINTGROUPS];
    uint32_t gpio_int_enabled[GPIO_NUMINTGROUPS];
    uint32_t gpio_int_type[GPIO_NUMINTGROUPS];
    /* Asserted logical level requests from devices such as the nested PMU IC. */
    uint32_t gpio_level_pending[GPIO_NUMINTGROUPS];
    /*
     * Pad-driven sources (ipod_touch_sysic_set_pad): their input levels, and
     * which bits have one. Those interrupt by the S5L8900's rule: INTTYPE bit 1 =
     * level, 0 = edge; INTLEVEL bit = the polarity (1 high); a level source
     * asserts while the pad matches, an edge source when it starts to.
     */
    uint32_t gpio_pad_level[GPIO_NUMINTGROUPS];
    uint32_t gpio_pad_driven[GPIO_NUMINTGROUPS];
} IPodTouchSYSICState;

/* A pad behind GPIO-IC line `irq` is now at `level`. */
void ipod_touch_sysic_set_pad(IPodTouchSYSICState *s, unsigned irq, bool level);

/* Latch an edge request and update the masked group output. */
void ipod_touch_sysic_request_edge(IPodTouchSYSICState *s, unsigned group, unsigned bit);

#endif