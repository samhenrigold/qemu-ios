/* Shared GPIO groups must preserve other pending sources when one is ACKed.
 *
 * SLICE include/hw/arm/ipod_touch_sysic.h define GPIO_|POWER_
 * SLICE include/hw/arm/ipod_touch_sysic.h typedef IPodTouchSYSICState
 * SLICE hw/arm/ipod_touch_sysic.c fn sysic_update_gpio_irq sysic_pad_match sysic_pad_eval sysic_gpio_irq_input ipod_touch_sysic_read ipod_touch_sysic_write sysic_post_load
 */
#include <stdint.h>
#define trace_event_get_state_backends(id) 0
#define TRACE_PRINTF(fn, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
typedef int SysBusDevice;
typedef int MemoryRegion;
typedef int *qemu_irq;
typedef uint64_t hwaddr;
static bool sysic_gpio_trace(void) { return false; }
static void qemu_set_irq(int *irq,int value) { *irq=!!value; }
#include "slice.h"

int main(void) {
    int level[GPIO_NUMINTGROUPS]={0};IPodTouchSYSICState s={0};
    for (unsigned i=0;i<GPIO_NUMINTGROUPS;i++) s.gpio_irqs[i]=&level[i];
    s.gpio_int_status[3]=(1u<<1)|(1u<<13); /* PMU and digitizer share group 3. */
    ipod_touch_sysic_write(&s,GPIO_INTEN+12,~0u,4);assert(level[3]);
    ipod_touch_sysic_write(&s,GPIO_INTSTAT+12,1u<<13,4);
    assert(level[3] && s.gpio_int_status[3]==2);
    ipod_touch_sysic_write(&s,GPIO_INTEN+12,0,4);assert(!level[3]);
    assert(ipod_touch_sysic_read(&s,GPIO_INTSTAT+12,4)==2);
    ipod_touch_sysic_write(&s,GPIO_INTEN+12,2,4);assert(level[3]);
    ipod_touch_sysic_write(&s,GPIO_INTSTAT+12,2,4);assert(!level[3]);
    s.gpio_int_status[3]=2;sysic_post_load(&s,1);assert(level[3]);
    s.gpio_int_enabled[3]=0;sysic_post_load(&s,1);assert(!level[3]);
    ipod_touch_sysic_write(&s,GPIO_INTSTAT+28,~0u,4);
    ipod_touch_sysic_write(&s,GPIO_INTEN+28,~0u,4);
    assert(ipod_touch_sysic_read(&s,GPIO_INTSTAT+28,4)==0);
    /* PMU events remain asserted across a GPIO ACK until I2C consumes them. */
    sysic_gpio_irq_input(&s,97,1);assert(!level[3]);
    ipod_touch_sysic_write(&s,GPIO_INTEN+12,2,4);assert(level[3]);
    ipod_touch_sysic_write(&s,GPIO_INTSTAT+12,2,4);assert(level[3]);
    /* The guest masks then ACKs before scheduling its I2C worker. */
    ipod_touch_sysic_write(&s,GPIO_INTEN+12,0,4);assert(!level[3]);
    ipod_touch_sysic_write(&s,GPIO_INTSTAT+12,2,4);
    assert(!s.gpio_int_status[3] && s.gpio_level_pending[3]==2);
    ipod_touch_sysic_write(&s,GPIO_INTEN+12,2,4);assert(level[3]);
    sysic_gpio_irq_input(&s,97,0);assert(!level[3] && !s.gpio_int_status[3]);
    puts("PASS: shared GPIO pending ACK, mask/unmask, restore and group bounds");
}
