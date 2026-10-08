/* Production AES DMA must not drop unrelated writes at legacy boot addresses,
 * and custom-key requests must encrypt/decrypt a stream split into segments the
 * way 3.1.3's AppleS5L8900XAES feeds them (IRQ 0x27, GO=3), blocks straddling
 * segment boundaries included. With aes-uid=engine, UID and short GID
 * operations run through the engine with stand-in keys.
 *
 * SLICE include/hw/arm/ipod_touch_aes.h define AES_|key_uid|key_gid_standin
 * SLICE include/hw/arm/ipod_touch_aes.h typedef AESKeyType IPodTouchAESState
 * SLICE hw/arm/ipod_touch_aes.c range #define IT_AES_DMA_CHUNK | static const MemoryRegionOps aes_ops
 * PKG openssl
 * CFLAGS -Wno-deprecated-declarations
 */
#include <assert.h>
#define trace_event_get_state_backends(id) 0
#define TRACE_PRINTF(fn, ...) do { if (0) printf(__VA_ARGS__); } while (0)
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
#define g_malloc dma_alloc
static void *dma_alloc(size_t n) {assert(n<=65536);return malloc(n);}
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
#include "slice.h"

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
/* aes-uid=engine: a UID (or short GID) encrypt as the 4.x kernel issues it -- seed at 0x28, result at
   0x20 -- comes out as AES with the stand-in key, where the legacy path left the output untouched. */
static void engine(void) {
 static uint8_t mem[0x1000];
 const uint8_t seed[16]={1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
 for(int t=0;t<2;t++){
  uint8_t ref[16];AES_KEY k;
  AES_set_encrypt_key(t?key_gid_standin:key_uid,128,&k);AES_encrypt(seed,ref,&k);
  memset(mem,0,sizeof(mem));memcpy(mem+0x100,seed,16);flat=mem;
  ipod_touch_aes_set_uid_engine(true);
  IPodTouchAESState s={.keytype=t?AESGID:AESUID,.operation=0xf,.insize=16,.outaddr=0x100,.inaddr=0x200};
  ipod_touch_aes_write(&s,AES_GO,1,4);
  assert(!memcmp(mem+0x200,ref,16)&&!memcmp(mem+0x100,seed,16)&&s.status==15);
  ipod_touch_aes_set_uid_engine(false);flat=NULL;
 }
 puts("PASS: aes-uid=engine UID and short-GID operations match OpenSSL with the stand-in keys");
}

/* Stock iBSS uses a single request larger than the former 16 MiB guard.
   Compare all bytes, including the plain final tail, with independent CBC. */
static void restore_size(void) {
 IPodTouchAESState length={0};
 ipod_touch_aes_write(&length,AES_INSIZE,UINT32_MAX,4);
 assert(length.insize==UINT32_MAX);
 const unsigned n=0x1824000+7, src=0x100, dst=src+n+0x100;
 uint8_t *mem=calloc(1,dst+n), *ref=malloc(n), key[16]={0}, iv[16]={0};
 AES_KEY k;
 for(unsigned i=0;i<n;i++)mem[src+i]=(uint8_t)(i*31+(i>>19));
 AES_set_encrypt_key(key,128,&k);
 AES_cbc_encrypt(mem+src,ref,n&~15u,&k,iv,AES_ENCRYPT);
 memcpy(ref+(n&~15u),mem+src+(n&~15u),n&15u);
 flat=mem;
 IPodTouchAESState s={.operation=0xf,.outaddr=src,.inaddr=dst};
 W(AES_INSIZE,n);W(AES_GO,1);
 assert(s.insize==n && s.status==15 && !memcmp(mem+dst,ref,n));
 memset(&s,0,sizeof(s));s.operation=0xe;s.outaddr=dst;s.inaddr=src;
 W(AES_INSIZE,n);W(AES_GO,1);
 for(unsigned i=0;i<n;i++)assert(mem[src+i]==(uint8_t)(i*31+(i>>19)));
 /* The retained legacy UID convention also uses bounded DMA storage. */
 memset(iv,0,sizeof(iv));AES_set_encrypt_key(key_uid,128,&k);
 AES_cbc_encrypt(mem+src,ref,n&~15u,&k,iv,AES_ENCRYPT);
 memcpy(ref+(n&~15u),mem+src+(n&~15u),n&15u);
 memcpy(mem+dst,ref,n);
 memset(&s,0,sizeof(s));s.keytype=AESUID;s.inaddr=dst;s.outaddr=src;
 W(AES_INSIZE,n);W(AES_GO,1);
 for(unsigned i=0;i<n;i++)assert(mem[src+i]==(uint8_t)(i*31+(i>>19)));
 flat=NULL;free(mem);free(ref);
 puts("PASS: complete restore-sized CBC encrypt/decrypt beyond 16 MiB with partial tail");
}
int main(void) {
 segmented();
 restore_size();
 unsigned addresses[]={0x220100ac,0x0bf08468,0x0fb9bcdc};
 for(unsigned i=0;i<3;i++) {
  check(addresses[i],AESCustom,128,true,true);
  check(addresses[i],AESCustom,16,true,false);
  check(addresses[i],AESCustom,128,false,false);
  check(addresses[i],AESUID,128,true,false);
 }
 check(0x0ff290ac,AESCustom,128,true,false);
 engine();
 puts("PASS: three narrowly preserved boot payloads; unrelated size, source, UID and fourth custom operation decrypt correctly");
}
