#!/usr/bin/env python3
"""Production AES DMA must not drop unrelated writes at legacy boot addresses."""
from pathlib import Path
import re, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'hw/arm/ipod_touch_aes.c').read_text()
header=(root/'include/hw/arm/ipod_touch_aes.h').read_text()
constants='\n'.join(re.findall(r'^#define (?:AES_|key_uid).*$',header,re.M))
state=re.search(r'typedef struct IPodTouchAESState.*?} IPodTouchAESState;',header,re.S)[0]
enum=re.search(r'typedef enum AESKeyType.*?} AESKeyType;',header,re.S)[0]
production=source[source.index('#define IT_AES_MAX_XFER'):source.index('static const MemoryRegionOps aes_ops')]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/aes.h>
typedef int SysBusDevice, MemoryRegion;
typedef uint64_t hwaddr;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define IT_SIZE(name,value,max) MIN(value,max)
#define g_malloc malloc
#define g_free free
#define error_report(...) fprintf(stderr,__VA_ARGS__)
static uint8_t input[256], output[256];
static unsigned writes;
static void cpu_physical_memory_read(hwaddr a,void *p,size_t n) {assert(n<=sizeof(input));memcpy(p,input,n);}
static void cpu_physical_memory_write(hwaddr a,const void *p,size_t n) {assert(n<=sizeof(output));writes++;memcpy(output,p,n);}
'''+constants+'\n'+enum+'\n'+state+'\n'+production+r'''
static void check(unsigned address,unsigned type,unsigned length,bool inplace,bool preserve) {
 IPodTouchAESState s={.keytype=type,.insize=length,.inaddr=inplace?address:0x1000,.outaddr=address};
 uint8_t key[16]={0}, iv[16]={0}, plain[256];AES_KEY enc;
 for(unsigned i=0;i<length;i++)plain[i]=i+1;
 if(type==AESUID)memcpy(key,key_uid,16);
 assert(!AES_set_encrypt_key(key,128,&enc));
 AES_cbc_encrypt(plain,input,length,&enc,iv,AES_ENCRYPT);
 writes=0;memset(output,0xcc,sizeof(output));
 ipod_touch_aes_write(&s,AES_GO,1,4);
 assert(writes==!preserve && s.outsize==length && s.status==15);
 if(!preserve)assert(!memcmp(output,plain,length));
}
int main(void) {
 unsigned addresses[]={0x220100ac,0x0bf08468,0x0fb9bcdc};
 for(unsigned i=0;i<3;i++) {
  check(addresses[i],AESCustom,128,true,true);
  check(addresses[i],AESCustom,16,true,false);
  check(addresses[i],AESCustom,128,false,false);
  check(addresses[i],AESUID,128,true,false);
 }
 check(0x0ff290ac,AESCustom,128,true,false);
 puts("PASS: three narrowly preserved boot payloads; unrelated size, source, UID and fourth custom operation decrypt correctly");
}
'''
with tempfile.TemporaryDirectory(prefix='it-aes-check-') as tmp:
 tmp=Path(tmp);(tmp/'check.c').write_text(code)
 flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','openssl'],text=True))
 subprocess.run(['cc','-Wno-deprecated-declarations','-fsanitize=address,undefined',str(tmp/'check.c'),*flags,'-o',str(tmp/'check')],check=True)
 subprocess.run([str(tmp/'check')],check=True)
