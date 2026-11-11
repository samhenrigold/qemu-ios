/* Exercise the production PMU transport: hibernate must retain the VM, while
 * the native final standby command must exit even without a host request.
 * SLICE:types hw/arm/s5l8950.c typedef A6PMU
 * SLICE hw/arm/s5l8950.c fn a6_pmu_event a6_pmu_send
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef struct {
    int unused;
} I2CSlave;
typedef struct {
    int unused;
} QEMUTimer;
typedef int *qemu_irq;
enum i2c_event { I2C_START_SEND };
#define QEMU_CLOCK_VIRTUAL 0
#define SHUTDOWN_CAUSE_GUEST_SHUTDOWN 1
#define SHUTDOWN_CAUSE_GUEST_RESET 2
#define trace_a6_pmu_register(...) ((void)0)
#define trace_a6_pmu_gpio(...) ((void)0)
static unsigned shutdowns, hibernates;
#include "types.h"
static void a6_pmu_set_ap_power(A6PMU *s, bool on)
{
    assert(!on);
    hibernates++;
}
static void a6_pmu_irq_update(A6PMU *s)
{
}
static void timer_mod(QEMUTimer *t, int64_t when)
{
}
static void timer_del(QEMUTimer *t)
{
}
static void qemu_set_irq(qemu_irq irq, int value)
{
}
static void qemu_system_reset_request(int cause)
{
    assert(false);
}
static int64_t qemu_clock_get_ns(int clock)
{
    return 0;
}
static void qemu_system_shutdown_request(int cause)
{
    assert(cause == SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    shutdowns++;
}
#include "slice.h"
static void write_reg(A6PMU *s, uint8_t reg, uint8_t value)
{
    a6_pmu_event((I2CSlave *)s, I2C_START_SEND);
    a6_pmu_send((I2CSlave *)s, reg);
    a6_pmu_send((I2CSlave *)s, value);
}
int main(void)
{
    A6PMU s = {0};
    write_reg(&s, 0xb0, 0x80);
    write_reg(&s, 0x1b, 2);
    assert(hibernates == 1 && shutdowns == 0);
    assert(s.regs[0xb0] == 0x80 && s.regs[0x1b] == 0);
    write_reg(&s, 0x1b, 0);
    write_reg(&s, 0x48, 0);
    assert(hibernates == 1 && shutdowns == 0);
    write_reg(&s, 0xb0, 0);
    write_reg(&s, 0x1b, 1);
    assert(shutdowns == 1 && hibernates == 1);
    assert(s.regs[0x1b] == 0);
    puts("A6 native standby exits; hibernate retains the VM");
}
