/* Native THS7383 receive window and connection event.
 * SLICE hw/arm/s5l8950.c fn a6_tristar_recv a6_tristar_reset
 * CFLAGS -Wall -Werror
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef struct A6TriStar {
    uint8_t regs[256], reg;
    unsigned log_count;
    bool address;
    int irq;
    bool attached_notified;
} A6TriStar;
typedef A6TriStar I2CSlave, DeviceState;
static unsigned edges;
static int level;
static void qemu_set_irq(int irq, int on)
{
    edges += level != on;
    level = on;
}
#define qemu_log_mask(...) ((void)0)
#include "slice.h"
int main(void)
{
    A6TriStar s = {0};
    a6_tristar_reset(&s);
    s.reg = 0x0d;
    assert(a6_tristar_recv(&s) == 0 && !level);
    s.reg = 0x0c;
    assert(a6_tristar_recv(&s) == 0x83 && level);
    unsigned before = edges;
    s.reg = 0x0c;
    assert(a6_tristar_recv(&s) == 0x83 && edges == before);
    s.reg = 0x0d;
    assert(a6_tristar_recv(&s) == 0x0c && !level);
    s.reg = 0x0d;
    assert(a6_tristar_recv(&s) == 0 && !level);
    s.reg = 0x0c;
    assert(a6_tristar_recv(&s) == 0x83 && !level);
    /* Original 6/7 getAccessoryID: seven bytes, response at end,
     * six-byte identity returned in reverse register order. */
    uint8_t rx[7], id[6], expected[6] = {0x10, 0x0c, 0, 0, 0, 0};
    s.reg = 0x39;
    for (unsigned i = 0; i < 7; i++)
        rx[i] = a6_tristar_recv(&s);
    assert(rx[6] == 0x75);
    for (unsigned i = 0; i < 6; i++)
        id[i] = rx[5 - i];
    assert(!memcmp(id, expected, sizeof(id)));
    a6_tristar_reset(&s);
    assert(!level && !s.attached_notified);
    s.reg = 0x0c;
    assert(a6_tristar_recv(&s) == 0x83 && level);
    puts(
        "PASS: TriStar native RX byte order, USB cable identity, attach event and read acknowledgement");
}
