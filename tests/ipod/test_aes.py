#!/usr/bin/env python3
"""Production AES DMA must not drop unrelated writes at legacy boot addresses,
and custom-key requests must encrypt/decrypt a stream split into segments the
way 3.1.3's AppleS5L8900XAES feeds them (IRQ 0x27, GO=3), blocks straddling
segment boundaries included."""
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
typedef int qemu_irq;
static int irq_level;
static void qemu_set_irq(qemu_irq i,int level) {irq_level=level;}
static uint8_t input[256], output[256];
static uint8_t *flat;        /* segmented checks: guest memory, addressed directly */
static unsigned writes;
static void cpu_physical_memory_read(hwaddr a,void *p,size_t n) {if(flat){memcpy(p,flat+a,n);return;}assert(n<=sizeof(input));memcpy(p,input,n);}
static void cpu_physical_memory_write(hwaddr a,const void *p,size_t n) {if(flat){memcpy(flat+a,p,n);return;}assert(n<=sizeof(output));writes++;memcpy(output,p,n);}
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
#define W(o,v) ipod_touch_aes_write(&s,(o),(v),4)
/* One request the way the kernel's multi-segment path issues it. */
static void stream(bool enc, const uint8_t *key, const uint8_t *iv, unsigned total,
                   const unsigned *inseg, const unsigned *outseg, unsigned in_base, unsigned out_base) {
 IPodTouchAESState s={0};
 unsigned in_i=0,out_i=0,in_off=in_base,out_off=out_base;
 s.status=0xf;                                   /* left over from a polled request */
 W(AES_UNKREG0,1);W(AES_UNKREG0,0);W(AES_CONTROL,1);
 assert(!irq_level);                             /* the reset cleared it */
 W(AES_IRQEN,7);assert(!irq_level);
 for(int i=0;i<4;i++)W(AES_KEY_REG+0x10+4*i,((const uint32_t*)key)[i]);
 W(AES_TYPE,0);W(AES_KEYLEN,enc?0xf:0xe);
 for(int i=0;i<4;i++)W(AES_IV_REG+4*i,((const uint32_t*)iv)[i]);
 W(AES_INSIZE,total);W(AES_SIZE3,total);
 W(AES_OUTADDR,in_off);W(AES_AUXSIZE,inseg[0]);in_off+=inseg[in_i++];
 W(AES_INADDR,out_off);W(AES_OUTSIZE,outseg[0]);out_off+=outseg[out_i++];
 W(AES_GO,1);
 for(int guard=0;;guard++){
  assert(guard<100 && irq_level);
  unsigned st=ipod_touch_aes_read(&s,AES_STATUS,4),ack=st&7;
  if(st&AES_ST_DONE){W(AES_STATUS,ack);assert(!irq_level);break;}
  if(st&AES_ST_NEED_IN){assert(inseg[in_i]);W(AES_OUTADDR,in_off);W(AES_AUXSIZE,inseg[in_i]);in_off+=inseg[in_i++];}
  if(st&AES_ST_NEED_OUT){assert(outseg[out_i]);W(AES_INADDR,out_off);W(AES_OUTSIZE,outseg[out_i]);out_off+=outseg[out_i++];}
  W(AES_STATUS,ack);assert(!irq_level);W(AES_GO,3);
 }
}
static void segmented(void) {
 enum{N=8192+48};
 static uint8_t mem[0x10000];
 uint8_t key[16],iv[16],ref[N],iv2[16];AES_KEY k;
 const unsigned in1[]={4093,3,4096-3,N-8192+6,0},out1[]={100,4000,4000,N-8100,0};
 const unsigned in2[]={N,0},out2[]={17,15,N-32,0};
 for(int i=0;i<16;i++){key[i]=i*7+1;iv[i]=i*3+5;}
 for(int i=0;i<N;i++)mem[0x100+i]=(uint8_t)(i*31+7);
 flat=mem;
 stream(true,key,iv,N,in1,out1,0x100,0x4100);
 memcpy(iv2,iv,16);AES_set_encrypt_key(key,128,&k);AES_cbc_encrypt(mem+0x100,ref,N,&k,iv2,AES_ENCRYPT);
 assert(!memcmp(mem+0x4100,ref,N));
 stream(false,key,iv,N,in2,out2,0x4100,0x8100);
 assert(!memcmp(mem+0x8100,mem+0x100,N));
 flat=NULL;
 puts("PASS: segmented custom-key encrypt/decrypt matches OpenSSL across straddling blocks");
}
int main(void) {
 segmented();
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
