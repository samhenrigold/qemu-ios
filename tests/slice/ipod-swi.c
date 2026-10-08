/* Run the actual core-voltage command handlers with sanitizer bounds checks.
 *
 * SLICE hw/arm/ipod_touch_swi.c range static uint64_t swi_read( | static const MemoryRegionOps
 */
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
typedef uint64_t hwaddr;
typedef struct { uint32_t regs[0x1000/4]; } IPodSWIState;
#include "slice.h"

int main(void)
{
    IPodSWIState s={0};
    /* Both channels are reused by voltage-up/down transitions. */
    for(unsigned n=0;n<100;n++) {
        unsigned ctl=(n&1)?0x1c:0x14, command=0x880+(n&127);
        swi_write(&s,ctl+4,command,4);swi_write(&s,ctl,3,4);
        assert(swi_read(&s,ctl,4)==2 && swi_read(&s,ctl+4,4)==command);
    }
    swi_write(&s,0x18,0x5200,4);swi_write(&s,0x14,1,4);
    assert(swi_read(&s,0x14,4)==0 && swi_read(&s,0x18,4)==0x5200);
    swi_write(&s,0,0x303,4);swi_write(&s,0x24,42,4);
    assert(swi_read(&s,0,4)==0x303 && swi_read(&s,0x24,4)==42);
    swi_write(&s,0xffc,123,4);assert(swi_read(&s,0xffc,4)==123);
    puts("PASS: both SWI channels complete repeated voltage commands; configuration preserved");
}
