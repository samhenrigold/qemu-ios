/* The modem's open-drain nRESET is released through input mode.
 * SLICE hw/arm/s5l8930_gpio.c define GPIO_CFG_DATA|GPIO_CFG_MODE|GPIO_MODE_OUT|GPIO_MODE_OUT_ALT
 * SLICE hw/arm/s5l8930_gpio.c fn pin_input pin_mode s5l8930_gpio_pad_level s5l8930_gpio_update_pad
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef struct S5L8930GPIOState {
    uint32_t cfg[232], input[8];
    int *pad[232];
} S5L8930GPIOState;
static void qemu_set_irq(int *irq, int value)
{
    *irq = value;
}
#include "slice.h"
int main(void)
{
    S5L8930GPIOState s = {0};
    const unsigned pin = 204;
    int level = -1;
    s.pad[pin] = &level;
    s.input[pin / 32] |= 1u << (pin % 32);
    s.cfg[pin] = 0x202; /* output low overrides pull-up */
    s5l8930_gpio_update_pad(&s, pin);
    assert(!level);
    s.cfg[pin] = 0x201; /* native nRESET release: input, pull-up high */
    s5l8930_gpio_update_pad(&s, pin);
    assert(level);
    s.input[pin / 32] &= ~(1u << (pin % 32));
    s5l8930_gpio_update_pad(&s, pin);
    assert(!level); /* external driver holds a released pin low */
    s.cfg[pin] = 0x203;
    s5l8930_gpio_update_pad(&s, pin);
    assert(level); /* output high overrides external input readback */
    s.cfg[pin] = 0x20e;
    s5l8930_gpio_update_pad(&s, pin);
    assert(!level);
    s.cfg[pin] = 0x20f;
    s5l8930_gpio_update_pad(&s, pin);
    assert(level);
    s.cfg[pin] = 0x20c; /* interrupt input follows the external line */
    s5l8930_gpio_update_pad(&s, pin);
    assert(!level);
    s.input[pin / 32] |= 1u << (pin % 32);
    s5l8930_gpio_update_pad(&s, pin);
    assert(level);
    puts(
        "PASS: physical GPIO output, open-drain release, pull-up/external input and interrupt-input levels");
}
