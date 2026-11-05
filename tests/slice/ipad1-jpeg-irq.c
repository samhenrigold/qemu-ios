/* The JPEG block's interrupt: up when an operation finishes, down when the driver reads the status.
 *
 * AppleJPEGDriver's handler reads 0x20 and acknowledges nothing (it only ever writes 0 to 0x1c, while setting up). With
 * the line held until the next software reset, the handler ran about a million times with the workloop starved, and an
 * app's snapshot froze an A4 device for seconds, logging "handle the unwanted JPEG interrupts" (LightTouchMac issue
 * 123). The status stays readable: only the line falls.
 *
 * SLICE hw/arm/s5l8920_jpeg.c define R_INT_ACK|R_INT_STATUS
 * SLICE hw/arm/s5l8920_jpeg.c fn jpeg_update_irq jpeg_complete jpeg_read
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef uint64_t hwaddr;
typedef int qemu_irq;
typedef struct S5L8920JPEGState {
    uint32_t regs[0x100];
    uint32_t pending;
    qemu_irq irq;
} S5L8920JPEGState;
#define qemu_irq_raise(i) ((i) = 1)
#define qemu_irq_lower(i) ((i) = 0)
#include "slice.h"

int main(void) {
    S5L8920JPEGState s = {.pending = 0x40};
    jpeg_complete(&s);
    assert(s.irq == 1);
    assert(jpeg_read(&s, R_INT_STATUS, 4) == 0x40);
    assert(s.irq == 0);
    assert(jpeg_read(&s, R_INT_STATUS, 4) == 0x40);     /* the status stays */
    jpeg_update_irq(&s);
    assert(s.irq == 0);                                 /* and doesn't interrupt again */
    s.regs[R_INT_STATUS / 4] = 0;
    jpeg_complete(&s);
    assert(s.irq == 1);                                 /* the next operation does */
    s.regs[R_INT_STATUS / 4] = 0;
    jpeg_update_irq(&s);
    assert(s.irq == 0);                                 /* a reset takes it down unread */
    puts("PASS: reading the JPEG block's status drops its interrupt line");
}
