/* The production SPI decoder against native driver packets and boundaries.
 * SLICE hw/arm/cs42l65.c define CS42L65_REGS
 * SLICE hw/arm/cs42l65.c range struct CS42L65State { | static uint32_t cs42l65_transfer
 * SLICE hw/arm/cs42l65.c fn cs42l65_transfer cs42l65_reset
 * CFLAGS -Wall -Wextra -Werror
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef struct {
    int unused;
} SSIPeripheral;
typedef SSIPeripheral DeviceState;
typedef struct CS42L65State CS42L65State;
#define CS42L65(p) ((CS42L65State *)(p))
#define qemu_log_mask(...) ((void)0)
#define g_assert_not_reached() assert(0)
#include "slice.h"

static uint8_t byte(CS42L65State *s, unsigned v)
{
    return cs42l65_transfer((SSIPeripheral *)s, v);
}
static void header(CS42L65State *s, unsigned op, unsigned reg, bool increment,
                   unsigned length)
{
    assert(length >= 1 && length <= 256);
    byte(s, op);
    byte(s, reg >> 8);
    byte(s, (reg & 127) | (increment ? 128 : 0));
    byte(s, length - 1);
}
int main(void)
{
    CS42L65State s;
    cs42l65_reset((DeviceState *)&s);
    /* Exact first transaction observed from the original N42 driver. */
    header(&s, 0x6d, 1, true, 3);
    assert(byte(&s, 255) == 0x42);
    assert(byte(&s, 255) == 0xa6);
    assert(byte(&s, 255) == 0x50);
    header(&s, 0x6c, 0x9901, true, 3);
    /* The native serializer sends address >> 8, not a packed 15-bit index.
     * Check the decoded address independently of the matching read packet. */
    assert(s.address == 0x9901);
    byte(&s, 0xa5);
    byte(&s, 0x34);
    byte(&s, 0x56);
    assert(s.regs[0x9901] == 0xa5 && s.regs[0x9902] == 0x34);
    header(&s, 0x6d, 0x9901, true, 3);
    assert(byte(&s, 255) == 0xa5);
    assert(byte(&s, 255) == 0x34);
    assert(byte(&s, 255) == 0x56);
    /* Non-incrementing packets stay on one register. */
    header(&s, 0x6c, 0x20, false, 3);
    byte(&s, 1);
    byte(&s, 2);
    byte(&s, 3);
    header(&s, 0x6d, 0x20, false, 2);
    assert(byte(&s, 0) == 3);
    assert(byte(&s, 0) == 3);
    /* Identity remains read-only. */
    header(&s, 0x6c, 1, true, 3);
    byte(&s, 0);
    byte(&s, 0);
    byte(&s, 0);
    header(&s, 0x6d, 1, true, 3);
    assert(byte(&s, 0) == 0x42);
    assert(byte(&s, 0) == 0xa6);
    assert(byte(&s, 0) == 0x50);
    /* Maximum packet length, address wrap, then a new packet. */
    header(&s, 0x6c, 0xff7f, true, 256);
    for (unsigned i = 0; i < 256; i++)
        byte(&s, i);
    assert(!s.remaining && !s.header_pos);
    assert(s.address == 0x007f);
    assert(s.regs[0xff7f] == 0 && s.regs[0xffff] == 128);
    assert(s.regs[0] == 129 && s.regs[0x7e] == 255);
    byte(&s, 0xff);
    header(&s, 0x6d, 1, true, 3);
    assert(byte(&s, 0) == 0x42);
    assert(byte(&s, 0) == 0xa6);
    assert(byte(&s, 0) == 0x50);
    cs42l65_reset((DeviceState *)&s);
    assert(s.regs[0x20] == 0);
    puts("CS42L65 native packet/ID/register/boundary checks PASS");
}
