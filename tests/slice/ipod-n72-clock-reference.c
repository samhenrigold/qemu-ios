/* Exercise production N72 clock derivation; no duplicated frequency function.
 *
 * SLICE include/hw/arm/ipod_touch_chipid.h typedef IPodTouchChipIDState
 * SLICE include/hw/arm/ipod_touch_clock.h typedef IPodTouchClockState
 * SLICE hw/arm/ipod_touch_clock.c fn s5l8720_pll_locks ipod_touch_clock_update
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
typedef struct { int unused; } SysBusDevice;
typedef struct { int unused; } MemoryRegion;
typedef struct { uint64_t hz; } Clock;
static void clock_update_hz(Clock *c, uint64_t hz) { c->hz = hz; }
#include "slice.h"
static uint64_t frequency(IPodTouchClockState *s) {
 ipod_touch_clock_update(s); return s->pclk->hz;
}
int main(void) {
 Clock c={0}; IPodTouchChipIDState chip={.word2=0x87200004};
 IPodTouchClockState s={.s5l8720=true,.chipid=&chip,.pclk=&c};
 assert(frequency(&s)==12000000);
 s.config1=0x4200; assert(frequency(&s)==3000000);
 chip.word2|=1; assert(frequency(&s)==6000000);
 s.config0=0x1000; s.pll0con=0x03008500; s.pllmode=1;
 assert(frequency(&s)==266000000);
 for(unsigned shift=0;shift<8;shift++) {
  s.pll0con=0x03008500|shift;
  assert(frequency(&s)==266000000/(1U<<shift));
 }
 s.pll0con=0x03008500; chip.word2&=~1;
 assert(frequency(&s)==133000000);
 s.pllmode=0x11; assert(frequency(&s)==299250000);
 chip.word2|=1; assert(frequency(&s)==299250000);
 s.config0=0x2000; s.pll1con=0x06005101; s.pllmode=3;
 assert(frequency(&s)==20250000);
 s.config0=0x3000; s.pll2con=0x02004002; s.pllmode=7;
 assert(frequency(&s)==24000000);
 s.pllmode=3; assert(frequency(&s)==0);
 s.pllmode=7; s.pll2con=0x00004002; assert(frequency(&s)==0);
 s.config0=0; s.pllmode=0; assert(frequency(&s)==6000000);
 s.chipid=0; assert(frequency(&s)==0);
 s.chipid=&chip; s.s5l8720=false; assert(frequency(&s)==0);
 puts("PASS actual N72 reference/bypass/SDIV/invalid-selection derivation");
 return 0;
}
