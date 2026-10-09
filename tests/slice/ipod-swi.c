/* Run the actual core-voltage command handlers with sanitizer bounds checks, and the A4 backlight level
 * decoded from the words AppleSamsungSWI sends (n90 11D257, with AppleARMBacklight's own log of each level).
 *
 * SLICE hw/arm/ipod_touch_swi.c range static int swi_backlight_level( | static const MemoryRegionOps
 */
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
typedef uint64_t hwaddr;
#include <stdbool.h>
typedef struct { uint32_t regs[0x1000/4]; bool backlight; uint8_t iset; uint32_t backlight_word; } IPodSWIState;
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

    /* iBoot's word on the asynchronous channel: the kernel logs 0x641 for it. */
    IPodSWIState b={.backlight=true,.iset=3};
    assert(swi_backlight_level(&b)==-1);
    swi_write(&b,0x20,0x3cc1,4);swi_write(&b,0x1c,3,4);
    assert(swi_backlight_level(&b)==0x641);
    /* The enable level goes on the synchronous channel ("set level to 0x7b3"), and a core-voltage
     * (vsel, command 6) word after it leaves the level alone. */
    swi_write(&b,0x18,0x3fb3,4);swi_write(&b,0x14,3,4);
    swi_write(&b,0x18,0x6155,4);swi_write(&b,0x14,3,4);
    assert(swi_backlight_level(&b)==0x7b3);
    /* A ramp step (0x7a6), the disable level (0x1c8) and 4.x/5.x's top (0x7ff). */
    swi_write(&b,0x20,0x3fa6,4);swi_write(&b,0x1c,3,4);assert(swi_backlight_level(&b)==0x7a6);
    swi_write(&b,0x18,0x33c8,4);swi_write(&b,0x14,3,4);assert(swi_backlight_level(&b)==0x1c8);
    swi_write(&b,0x20,0x3fff,4);swi_write(&b,0x1c,3,4);assert(swi_backlight_level(&b)==0x7ff);
    /* The K48's command-iset is 1: 5.1.1 ramps 0x1fed..0x1fff; an iset-3 word is not its backlight. */
    IPodSWIState k={.backlight=true,.iset=1};
    swi_write(&k,0x20,0x1fff,4);swi_write(&k,0x1c,3,4);assert(swi_backlight_level(&k)==0x7ff);
    swi_write(&k,0x20,0x3fa6,4);swi_write(&k,0x1c,3,4);assert(swi_backlight_level(&k)==0x7ff);
    puts("PASS: both SWI channels complete repeated voltage commands; configuration preserved; "
         "the backlight level is the last current-set word on either channel");
}
