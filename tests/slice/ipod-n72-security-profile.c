/* Run actual production fuse transformation; secure is not security-domain.
 *
 * SLICE include/hw/arm/ipod_touch_chipid.h range typedef enum { | void ipod_touch_chipid_set_n72_profile(
 * SLICE hw/arm/ipod_touch_chipid.c fn ipod_touch_chipid_set_n72_profile
 */
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#define g_assert_not_reached() assert(0)
typedef struct { int unused; } SysBusDevice;
typedef struct { int unused; } MemoryRegion;
#include "slice.h"
int main(void) {
 for(unsigned domain=0;domain<4;domain++) {
  for(unsigned oscillator=0;oscillator<2;oscillator++) {
   for(unsigned info_secure=0;info_secure<2;info_secure++) {
    for(unsigned prod_input=0;prod_input<2;prod_input++) {
    IPodTouchChipIDState original={.word1=0xabcd0000|(prod_input<<5),.word2=0x87200000|(domain<<2)|oscillator|(info_secure<<1),.word3=123,.word4=456};
    for(unsigned profile=0;profile<3;profile++) {
     IPodTouchChipIDState s=original;
     ipod_touch_chipid_set_n72_profile(&s,profile);
     unsigned production=(s.word1>>5)&1;
     unsigned secure=((s.word2>>1)&1)|production;
     assert((secure|(production<<1))==(profile==0?((info_secure|prod_input)|(prod_input<<1)):profile==1?1:0));
     if(profile==0) assert(s.word1==original.word1&&s.word2==original.word2);
     assert((s.word2&~2u)==(original.word2&~2u));
     assert((s.word1&~32u)==(original.word1&~32u));
     assert(s.word3==123&&s.word4==456);
     IPodTouchChipIDState once=s;
     ipod_touch_chipid_set_n72_profile(&s,profile);
     assert(s.word1==once.word1&&s.word2==once.word2);
    }
    }
   }
  }
 }
 puts("PASS actual N72 CPFM03/01/00 transformation: SDOM/oscillator/identity preserved");
 return 0;
}
