#!/usr/bin/env python3
"""Production PKE arithmetic, padding, failure bounds and optional signature forging."""
from pathlib import Path
import shlex
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'hw/arm/ipod_touch_pke.c').read_text()
a=s.index('static bool forge_sigcheck_enabled');b=s.index('static const MemoryRegionOps',a)
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <openssl/bn.h>
typedef uint64_t hwaddr;
typedef struct {uint8_t segments[1024];uint32_t seg_size_reg,segment_size;uint8_t num_started;} IPodTouchPKEState;
#define REG_PKE_START 8
#define REG_PKE_SEG_SIZE 0x14
#define REG_PKE_SWRESET 0x24
#define REG_PKE_SEG_START 0x800
#define LOG_GUEST_ERROR 0
#define qemu_log_mask(...) ((void)0)
static bool ipod_touch_sha1_last_hash(uint8_t *hash) {memset(hash,0xa5,20);return true;}
'''+s[a:b]+r'''
static void start(IPodTouchPKEState *s) {
    ipod_touch_pke_write(s,REG_PKE_SWRESET,1,4);
    for(int i=0;i<5;i++)ipod_touch_pke_write(s,REG_PKE_START,1,4);
}
int main(void) {
    unsetenv("IT_FORGE_SIGCHECK");
    for(unsigned length=128;length<=256;length*=2) {
        IPodTouchPKEState s={0};
        ipod_touch_pke_write(&s,REG_PKE_SEG_SIZE,length==128 ? 64:0,4);
        s.segments[0]=19;s.segments[length]=5;
        start(&s);assert(s.segments[length]==4);
        for(unsigned i=1;i<length;i++)assert(s.segments[length+i]==0);
        s.segments[length]=0;start(&s);
        for(unsigned i=0;i<length;i++)assert(s.segments[length+i]==0);
        memset(s.segments,0,sizeof(s.segments));s.segments[length]=7;
        start(&s);assert(s.segments[length]==7); /* zero modulus preserves input */

        /* A valid SHA1 block must survive forge mode even though the current
         * SHA engine digest differs. Generate a key and sign our own fixture. */
        BN_CTX *ctx=BN_CTX_new();
        BIGNUM *p=BN_new(),*q=BN_new(),*n=BN_new(),*phi=BN_new(),*e=BN_new();
        BIGNUM *plain=BN_new(),*signature=BN_new();
        BIGNUM *d=NULL;
        do {
            assert(BN_generate_prime_ex(p,length*4,0,NULL,NULL,NULL));
            assert(BN_generate_prime_ex(q,length*4,0,NULL,NULL,NULL));
            assert(BN_mul(n,p,q,ctx) && BN_sub_word(p,1) && BN_sub_word(q,1));
            assert(BN_mul(phi,p,q,ctx) && BN_set_word(e,65537));
            d=BN_mod_inverse(NULL,e,phi,ctx);
        } while(!d); /* Regenerate the rare key whose totient is divisible by e. */
        uint8_t block[256],hash[20];memset(hash,0x5a,20);
        assert(build_pkcs1_block(block,length,hash));
        assert(BN_bin2bn(block,length,plain) && BN_mod_exp(signature,plain,d,n,ctx));
        assert(BN_bn2lebinpad(n,s.segments,length)==length);
        assert(BN_bn2lebinpad(signature,s.segments+length,length)==length);
        setenv("IT_FORGE_SIGCHECK","1",1);start(&s);
        for(unsigned i=0;i<length;i++)assert(s.segments[length+i]==block[length-1-i]);
        /* Malformed recovery still uses the explicitly enabled compatibility path. */
        memset(s.segments+length,0,length);s.segments[length]=1;start(&s);
        memset(hash,0xa5,20);assert(build_pkcs1_block(block,length,hash));
        for(unsigned i=0;i<length;i++)assert(s.segments[length+i]==block[length-1-i]);
        unsetenv("IT_FORGE_SIGCHECK");
        BN_free(p);BN_free(q);BN_free(n);BN_free(phi);BN_free(e);BN_free(d);
        BN_free(plain);BN_free(signature);BN_CTX_free(ctx);
    }
    for(unsigned size=0;size<=1024;size++)if(size!=128 && size!=256) {
        IPodTouchPKEState s={0};s.segment_size=size;memset(s.segments,0x5a,sizeof(s.segments));
        start(&s);for(unsigned i=0;i<sizeof(s.segments);i++)assert(s.segments[i]==0x5a);
    }
    puts("PASS: 1024/2048-bit RSA, short/zero results, valid block preservation, explicit forging and malformed sizes");
}
'''
flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','openssl'],text=True))
with tempfile.TemporaryDirectory() as tmp:
    tmp=Path(tmp);(tmp/'check.c').write_text(code)
    subprocess.run(['cc','-fsanitize=address,undefined',str(tmp/'check.c'),'-o',str(tmp/'check'),*flags],check=True)
    subprocess.run([str(tmp/'check')],check=True)
