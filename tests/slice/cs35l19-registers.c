/* Production Cirrus I2C MAP framing and the native N42 identity transaction.
 * SLICE hw/arm/cs35l19.c range struct CS35L19State { | static int cs35l19_event
 * SLICE hw/arm/cs35l19.c fn cs35l19_event cs35l19_recv cs35l19_send cs35l19_reset
 * CFLAGS -Wall -Wextra -Werror
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef struct {
    int unused;
} I2CSlave;
typedef I2CSlave DeviceState;
typedef struct CS35L19State CS35L19State;
enum i2c_event { I2C_START_SEND, I2C_START_RECV, I2C_FINISH };
#define CS35L19(p) ((CS35L19State *)(p))
#define qemu_log(...) ((void)0)
#include "slice.h"
static void map(CS35L19State *s, uint8_t address)
{
    cs35l19_event((I2CSlave *)s, I2C_START_SEND);
    assert(!cs35l19_send((I2CSlave *)s, address));
}
int main(void)
{
    CS35L19State s;
    I2CSlave *bus = (I2CSlave *)&s;
    cs35l19_reset((DeviceState *)&s);
    map(&s, 0x81);
    cs35l19_event(bus, I2C_START_RECV);
    assert(cs35l19_recv(bus) == 0x35);
    assert(cs35l19_recv(bus) == 0xa1);
    assert(cs35l19_recv(bus) == 0x90);
    map(&s, 0x90);
    cs35l19_send(bus, 0xd2);
    cs35l19_send(bus, 0x19);
    map(&s, 0x90);
    cs35l19_event(bus, I2C_START_RECV);
    assert(cs35l19_recv(bus) == 0xd2);
    assert(cs35l19_recv(bus) == 0x19);
    map(&s, 0x81);
    cs35l19_send(bus, 0);
    cs35l19_send(bus, 0);
    cs35l19_send(bus, 0);
    map(&s, 0x81);
    assert(cs35l19_recv(bus) == 0x35);
    assert(cs35l19_recv(bus) == 0xa1);
    assert(cs35l19_recv(bus) == 0x90);
    map(&s, 0x20);
    cs35l19_send(bus, 7);
    cs35l19_send(bus, 9);
    cs35l19_event(bus, I2C_FINISH);
    map(&s, 0x20);
    assert(cs35l19_recv(bus) == 9);
    assert(cs35l19_recv(bus) == 9);
    map(&s, 0xff);
    for (unsigned i = 0; i < 256; i++)
        cs35l19_send(bus, i);
    assert(s.address == 127);
    cs35l19_reset((DeviceState *)&s);
    assert(!s.regs[0x20]);
    puts("CS35L19 native ID/MAP/repeated-start/register/boundary checks PASS");
}
