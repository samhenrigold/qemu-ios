#!/usr/bin/env python3
"""Production AES DMA must not drop unrelated writes at legacy boot addresses,
and custom-key requests must encrypt/decrypt a stream split into segments the
way 3.1.3's AppleS5L8900XAES feeds them (IRQ 0x27, GO=3), blocks straddling
segment boundaries included. With aes-uid=engine, UID and short GID
operations run through the engine with stand-in keys."""
from pathlib import Path
import os, re, shlex, subprocess, sys, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'hw/arm/ipod_touch_aes.c').read_text()
header=(root/'include/hw/arm/ipod_touch_aes.h').read_text()
constants='\n'.join(re.findall(r'^#define (?:AES_|key_uid|key_gid_standin).*$',header,re.M))
state=re.search(r'typedef struct IPodTouchAESState.*?} IPodTouchAESState;',header,re.S)[0]
enum=re.search(r'typedef enum AESKeyType.*?} AESKeyType;',header,re.S)[0]
production=source[source.index('#define IT_AES_DMA_CHUNK'):source.index('static const MemoryRegionOps aes_ops')]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/aes.h>
#include <glib.h>
typedef int SysBusDevice, MemoryRegion;
typedef uint64_t hwaddr;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
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
static unsigned writes, total_reads, total_writes;
static void cpu_physical_memory_read(hwaddr a,void *p,size_t n) {total_reads++;if(flat){memcpy(p,flat+a,n);return;}assert(n<=sizeof(input));memcpy(p,input,n);}
static void cpu_physical_memory_write(hwaddr a,const void *p,size_t n) {total_writes++;if(flat){memcpy(flat+a,p,n);return;}assert(n<=sizeof(output));writes++;memcpy(output,p,n);}
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
/* The S5L8720 ROM sets direction, key size and mode in separate RMWs.
   Readback must retain each field; otherwise its UID derivation decrypts. */
static void rom_keylen(void) {
 for(unsigned encrypt=0;encrypt<2;encrypt++){
  IPodTouchAESState s={.keytype=AESUID,.insize=16,
                       .inaddr=0x2000,.outaddr=0x1000};
  W(AES_KEYLEN,6);
  assert(ipod_touch_aes_read(&s,AES_KEYLEN,4)==6);
  uint32_t op=ipod_touch_aes_read(&s,AES_KEYLEN,4);
  W(AES_KEYLEN,(op&~1u)|encrypt);
  op=ipod_touch_aes_read(&s,AES_KEYLEN,4);
  W(AES_KEYLEN,op&~0x30u);
  op=ipod_touch_aes_read(&s,AES_KEYLEN,4);
  W(AES_KEYLEN,op&~8u);
  assert(ipod_touch_aes_read(&s,AES_KEYLEN,4)==6+encrypt);
  uint8_t wanted[16],iv[16]={0};AES_KEY k;
  for(unsigned i=0;i<16;i++)input[i]=i^3;
  if(encrypt)AES_set_encrypt_key(key_uid,128,&k);
  else AES_set_decrypt_key(key_uid,128,&k);
  AES_cbc_encrypt(input,wanted,16,&k,iv,encrypt?AES_ENCRYPT:AES_DECRYPT);
  ipod_touch_aes_set_uid_engine(true);W(AES_GO,1);
  assert(!memcmp(output,wanted,16));
  ipod_touch_aes_set_uid_engine(false);
 }
 puts("PASS: stock ROM KEYLEN RMW retains direction and UID encrypt/decrypt matches OpenSSL");
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
 rom_keylen();
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
 printf("transactions reads=%u writes=%u\n",total_reads,total_writes);
 IPodTouchAESState trace_probe={0};
 for(unsigned i=0;i<IT_AES_CONTRACT_LIMIT+5;i++)aes_contract_begin(&trace_probe,1,false);
}
'''
with tempfile.TemporaryDirectory(prefix='it-aes-check-') as tmp:
 tmp=Path(tmp);(tmp/'check.c').write_text(code)
 flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','openssl','glib-2.0'],text=True))
 subprocess.run(['cc','-Wno-deprecated-declarations','-fsanitize=address,undefined',str(tmp/'check.c'),*flags,'-o',str(tmp/'check')],check=True)
 if '--contract-trace' in sys.argv:
  disabled_env = dict(os.environ)
  disabled_env.pop('IT_AES_CONTRACT_TRACE', None)
  disabled = subprocess.run([str(tmp/'check')],env=disabled_env,capture_output=True,text=True,check=True)
  enabled = subprocess.run([str(tmp/'check')],env=disabled_env | {'IT_AES_CONTRACT_TRACE':'1'},capture_output=True,text=True,check=True)
  assert enabled.stdout == disabled.stdout
  assert 'AES_CONTRACT' not in disabled.stderr
  records = [line for line in enabled.stderr.splitlines() if line.startswith('AES_CONTRACT')]
  assert len(records) == 65, len(records)
  assert records[-1] == 'AES_CONTRACT truncated limit=64'
  assert sum('truncated' in line for line in records) == 1
  assert any('preserve=1' in line for line in records)
  assert any('written=0' in line for line in records)
  assert any('keysha=' in line for line in records)
  assert any('insha=' in line and 'outsha=' in line for line in records)
  print(disabled.stdout, end='')
  print('PASS bounded AES trace disabled silence, identical transaction counts/results, fingerprints, preservation observation and single overflow marker')
 else:
  subprocess.run([str(tmp/'check')],check=True)
