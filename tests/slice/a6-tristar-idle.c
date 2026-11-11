/* TriStar's N42 interrupt is active-high (GPIO cfg204), not the active-low
 * idle expected by the shared GPIO reset defaults. With no accessory status,
 * reset must drive it low while the ID mailbox and register traffic work.
 * SLICE hw/arm/s5l8950.c fn a6_tristar_event a6_tristar_send a6_tristar_recv a6_tristar_reset
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef struct {
    uint8_t regs[256], reg;
    unsigned log_count;
    bool address, attached_notified;
    int irq;
} A6TriStar;
typedef A6TriStar I2CSlave, DeviceState;
enum i2c_event { I2C_START_SEND };
#define qemu_log_mask(...) ((void)0)
static unsigned irq_level = 1;
static void qemu_set_irq(int irq, int value)
{
    irq_level = value;
}
#include "slice.h"
int main(void)
{
    A6TriStar s = {0};
    a6_tristar_reset(&s);
    assert(irq_level == 0);
    a6_tristar_event(&s, I2C_START_SEND);
    a6_tristar_send(&s, 0x20);
    assert(a6_tristar_recv(&s) == 0x74 && a6_tristar_recv(&s) == 0 &&
           a6_tristar_recv(&s) == 0);
    a6_tristar_event(&s, I2C_START_SEND);
    a6_tristar_send(&s, 0x0e);
    a6_tristar_send(&s, 0xc1);
    a6_tristar_event(&s, I2C_START_SEND);
    a6_tristar_send(&s, 0x0d);
    assert(a6_tristar_recv(&s) == 0 && irq_level == 0);
    a6_tristar_event(&s, I2C_START_SEND);
    a6_tristar_send(&s, 0x0f);
    a6_tristar_send(&s, 0xff);
    a6_tristar_event(&s, I2C_START_SEND);
    a6_tristar_send(&s, 0x0f);
    assert(a6_tristar_recv(&s) == 0);
    irq_level = 1;
    a6_tristar_reset(&s);
    assert(irq_level == 0 && s.regs[0x0e] == 0);
    puts(
        "PASS: TriStar active-high IRQ idle low, empty status, revision and ID mailbox after reset");
}
