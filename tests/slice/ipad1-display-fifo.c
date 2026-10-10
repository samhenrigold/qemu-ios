/* The A4 display pipe's parameter FIFO (pipe_fifo_write): a word with bit 31 set is a transaction header only between
 * packets. Inside a packet it is data, as the video layer's polyphase taps are: 6.1.6's first movie swap carries
 * 0xf0808000 among them, and read as a header it named the swap 0x8000 and dropped the rest of its packets, so the
 * swap AppleDisplayPipe waited for never completed.
 *
 * SLICE:types hw/arm/s5l8930_display.c define PIPE_WORDS
 * SLICE:types hw/arm/s5l8930_display.c range typedef struct { | /* A UI layer
 * SLICE hw/arm/s5l8930_display.c fn pipe_fifo_write pipe_fifo_write_word
 * PKG glib-2.0
 */
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define S5L8930_DISP_PIPE0_SIZE 0x8000
typedef void *qemu_irq;
#include "types.h"
static void pipe_fifo_write_word(DisplayPipe *p, uint32_t val);
#include "slice.h"

int main(void)
{
    static DisplayPipe p;

    /* A 6.x swap: header (bit 29, word count, ID), then packets of (count << 16 | register) and their words. */
    pipe_fifo_write(&p, 0xa00565e9);
    pipe_fifo_write(&p, 2 << 16 | 0x3120);
    pipe_fifo_write(&p, 0xf0808000);            /* a negative tap: data */
    pipe_fifo_write(&p, 0x00ff0010);
    pipe_fifo_write(&p, 1 << 16 | 0x1038);
    pipe_fifo_write(&p, 0x600);
    assert(p.swap_id == 0x65e9);
    assert(p.regs[0x3120 / 4] == 0xf0808000 && p.regs[0x3124 / 4] == 0x00ff0010);
    assert(p.regs[0x1038 / 4] == 0x600);
    /* Between packets, bit 31 starts the next transaction. */
    pipe_fifo_write(&p, 0xa00265f1);
    assert(p.swap_id == 0x65f1 && p.pkt_left == 0);
    printf("ok\n");
    return 0;
}
