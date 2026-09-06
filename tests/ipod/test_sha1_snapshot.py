#!/usr/bin/env python3
"""SHA published digest follows device ownership, reset and declared VMState."""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'hw/arm/ipod_touch_sha1.c').read_text()
header=(root/'include/hw/arm/ipod_touch_sha1.h').read_text()
state=re.search(r'typedef struct IPodTouchSHA1State.*?} IPodTouchSHA1State;',header,re.S)[0]
iv=re.search(r'static const uint32_t sha1_iv.*?};',source,re.S)[0]
functions=[]
for name in ['ipod_touch_sha1_last_hash','sha1_publish','sha1_clear_irq','sha1_reset','ipod_touch_sha1_reset']:
 functions.append(re.search(r'^(?:static )?(?:bool|void) '+name+r'\([^)]*\)\s*\{.*?^}',source,re.M|re.S)[0])
fields=source[source.index('        VMSTATE_UINT8_ARRAY(last_hash'):source.index('        VMSTATE_END_OF_LIST()',source.index('static const VMStateDescription'))]
code=r'''
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
'''+state+'\n'+iv+'\n'+'\n'.join(functions)+r'''
typedef struct {size_t offset,size;} Field;
#define FIELD(f,t) {offsetof(t,f),sizeof(((t *)0)->f)}
#define VMSTATE_UINT8_ARRAY(f,t,n) FIELD(f,t)
#define VMSTATE_UINT32_ARRAY(f,t,n) FIELD(f,t)
#define VMSTATE_UINT32(f,t) FIELD(f,t)
#define VMSTATE_BOOL(f,t) FIELD(f,t)
static const Field fields[]={
'''+fields+r'''};
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
'''
with tempfile.TemporaryDirectory(prefix='it-sha-state-') as tmp:
 tmp=Path(tmp);(tmp/'check.c').write_text(code)
 subprocess.run(['cc','-fsanitize=address,undefined',str(tmp/'check.c'),'-o',str(tmp/'check')],check=True)
 subprocess.run([str(tmp/'check')],check=True)
