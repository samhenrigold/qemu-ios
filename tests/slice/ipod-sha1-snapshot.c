/* SHA published digest follows device ownership, reset and declared VMState.
 *
 * SLICE include/hw/arm/ipod_touch_sha1.h typedef IPodTouchSHA1State
 * SLICE hw/arm/ipod_touch_sha1.c range static const uint32_t sha1_iv | \nstatic bool sha1_trace
 * SLICE hw/arm/ipod_touch_sha1.c fn ipod_touch_sha1_last_hash sha1_publish sha1_clear_irq sha1_reset ipod_touch_sha1_reset
 * SLICE:fields hw/arm/ipod_touch_sha1.c range VMSTATE_UINT8_ARRAY(last_hash | VMSTATE_END_OF_LIST()
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef int SysBusDevice, MemoryRegion, qemu_irq;
typedef void DeviceState;
static void qemu_irq_lower(qemu_irq irq) {}
#define IPOD_TOUCH_SHA1(s) ((IPodTouchSHA1State *)(s))
#include "slice.h"
typedef struct {size_t offset,size;} Field;
#define FIELD(f,t) {offsetof(t,f),sizeof(((t *)0)->f)}
#define VMSTATE_UINT8_ARRAY(f,t,n) FIELD(f,t)
#define VMSTATE_UINT32_ARRAY(f,t,n) FIELD(f,t)
#define VMSTATE_UINT32(f,t) FIELD(f,t)
#define VMSTATE_BOOL(f,t) FIELD(f,t)
static const Field fields[]={
#include "fields.h"
};
static void transfer(IPodTouchSHA1State *to,const IPodTouchSHA1State *from) {
 for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++)
  memcpy((char *)to+fields[i].offset,(const char *)from+fields[i].offset,fields[i].size);
}
int main(void) {
 IPodTouchSHA1State a={0},b={0},snapshot={0},empty={0};
 uint8_t digest[20],expected[20];
 assert(!ipod_touch_sha1_last_hash(NULL,digest));
 assert(!ipod_touch_sha1_last_hash(&a,digest));
 for(unsigned i=0;i<5;i++)a.state[i]=0x01020304+i;
 sha1_publish(&a);assert(ipod_touch_sha1_last_hash(&a,expected));
 assert(expected[0]==1 && expected[1]==2 && expected[2]==3 && expected[3]==4);
 assert(!ipod_touch_sha1_last_hash(&b,digest));
 transfer(&snapshot,&a);
 memset(a.state,0xa5,sizeof(a.state));sha1_publish(&a);
 assert(ipod_touch_sha1_last_hash(&a,digest) && memcmp(digest,expected,20));
 transfer(&a,&snapshot);
 assert(ipod_touch_sha1_last_hash(&a,digest) && !memcmp(digest,expected,20));
 transfer(&b,&snapshot); /* A fresh destination receives the published digest. */
 assert(ipod_touch_sha1_last_hash(&b,digest) && !memcmp(digest,expected,20));
 sha1_reset(&a);assert(!ipod_touch_sha1_last_hash(&a,digest));
 assert(ipod_touch_sha1_last_hash(&b,digest));
 ipod_touch_sha1_reset(&b);assert(!ipod_touch_sha1_last_hash(&b,digest));
 transfer(&b,&snapshot);transfer(&b,&empty);
 assert(!ipod_touch_sha1_last_hash(&b,digest));
 puts("PASS: independent SHA digests, guest/machine reset, rewind, fresh restore and invalid-state restore");
}
