/* The A4 boards' L3G4200D FIFO_SRC as AppleAP3GDL reads it: with WTM 2 (FIFO_CTRL 0x42) the driver pops only when FSS is
 * above the watermark, so the flag rises past it. Up at the watermark, INT2 stayed asserted with nothing to pop, and
 * the driver read FIFO_SRC thousands of times a second whenever CoreMotion ran the gyro.
 *
 * SLICE hw/arm/s5l8930_i2c.c define L3G_FIFO_CTRL
 * SLICE hw/arm/s5l8930_i2c.c fn l3g_fifo_src
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
typedef struct {
    uint8_t regs[0x40];
    uint8_t level;
    bool ovrn;
} S5L8930L3GState;
#include "slice.h"

int main(void)
{
    S5L8930L3GState s = { .regs = { [L3G_FIFO_CTRL] = 0x42 } };

    assert(l3g_fifo_src(&s) == 0x20);        /* empty */
    s.level = 2;
    assert(l3g_fifo_src(&s) == 2);           /* at the watermark: FSS only, as the driver has nothing to pop */
    s.level = 3;
    assert(l3g_fifo_src(&s) == (0x80 | 3));  /* past it */
    s.level = 40;
    s.ovrn = true;
    assert(l3g_fifo_src(&s) == (0x80 | 0x40 | 0x1f));
    puts("PASS: L3G4200D watermark rises past WTM, not at it");
    return 0;
}
