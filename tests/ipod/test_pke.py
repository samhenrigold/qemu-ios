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
#include <errno.h>
#include <openssl/bn.h>
typedef uint64_t hwaddr;
typedef struct {uint8_t segments[2048],modulus[256];uint32_t seg_size_reg,segment_size,modulus_size,key_len,seg_id,seg_sign;} IPodTouchPKEState;
#define REG_PKE_START 8
#define REG_PKE_SEG_SIZE 0x14
#define REG_PKE_SWRESET 0x24
#define REG_PKE_SEG_START 0x800
#define LOG_GUEST_ERROR 0
#define qemu_log_mask(...) ((void)0)
static bool ipod_touch_sha1_last_hash(uint8_t *hash) {memset(hash,0xa5,20);return true;}
'''+s[a:b]+r'''
static void op(IPodTouchPKEState *s, unsigned a, unsigned b, unsigned dest, bool one, bool load) {
    ipod_touch_pke_write(s,0xc,a<<24|b<<16|dest,4);
    ipod_touch_pke_write(s,REG_PKE_SEG_SIZE,(s->seg_size_reg & ~2u)|(one ? 2:0),4);
    ipod_touch_pke_write(s,REG_PKE_START,load ? 9:1,4);
    assert(ipod_touch_pke_read(s,REG_PKE_START,4)==0);
}
static void exponentiate(IPodTouchPKEState *s,unsigned exponent, BIGNUM *mod) {
    unsigned size=s->segment_size,rsq=size==64 ? 29:size==128 ? 14:6;
    unsigned bits=size*8+(size/64)*16;
    BN_CTX *ctx=BN_CTX_new();BIGNUM *r=BN_new();assert(BN_set_bit(r,bits));
    assert(BN_mod_sqr(r,r,mod,ctx));
    assert(BN_bn2lebinpad(r,s->segments+rsq*size,size)==size);
    op(s,1,rsq,3,false,true);op(s,rsq,rsq,2,true,false);
    int bit=31;while(bit && !(exponent & (1u<<bit)))bit--;
    for(;bit>=0;bit--) {
        op(s,2,2,1,false,false);
        if(exponent & (1u<<bit))op(s,1,3,2,false,false);
        else memcpy(s->segments+2*size,s->segments+size,size);
    }
    op(s,2,2,1,true,false);
    BN_free(r);BN_CTX_free(ctx);
}
int main(void) {
    unsetenv("IT_FORGE_SIGCHECK");
    for(unsigned length=64;length<=256;length*=2) {
        IPodTouchPKEState s={0};
        ipod_touch_pke_write(&s,REG_PKE_SEG_SIZE,length==64 ? 129:length==128 ? 65:1,4);
        ipod_touch_pke_write(&s,0,length==64 ? 0x78:length==128 ? 0x79:0x7b,4);
        assert(s.segment_size==length && ipod_touch_pke_read(&s,0,4)==s.key_len);
        BN_CTX *ctx=BN_CTX_new();
        BIGNUM *p=BN_new(),*q=BN_new(),*n=BN_new(),*phi=BN_new(),*e=BN_new();
        BIGNUM *plain=BN_new(),*signature=BN_new(),*d=NULL;
        do {
            assert(BN_generate_prime_ex(p,length*4,0,NULL,NULL,NULL));
            assert(BN_generate_prime_ex(q,length*4,0,NULL,NULL,NULL));
            assert(BN_mul(n,p,q,ctx) && BN_sub_word(p,1) && BN_sub_word(q,1));
            assert(BN_mul(phi,p,q,ctx) && BN_set_word(e,65537));
            d=BN_mod_inverse(NULL,e,phi,ctx);
        } while(!d);
        uint8_t block[256],hash[20];memset(hash,0x5a,20);
        assert(build_pkcs1_block(block,length,hash));
        assert(BN_bin2bn(block,length,plain) && BN_mod_exp(signature,plain,d,n,ctx));
        assert(BN_bn2lebinpad(n,s.segments,length)==length);
        assert(BN_bn2lebinpad(signature,s.segments+length,length)==length);
        setenv("IT_FORGE_SIGCHECK","1",1);exponentiate(&s,65537,n);
        for(unsigned i=0;i<length;i++)assert(s.segments[length+i]==block[length-1-i]);
        memset(s.segments+length,0,length);s.segments[length]=1;exponentiate(&s,65537,n);
        memset(hash,0xa5,20);assert(build_pkcs1_block(block,length,hash));
        for(unsigned i=0;i<length;i++)assert(s.segments[length+i]==block[length-1-i]);
        unsetenv("IT_FORGE_SIGCHECK");
        for(unsigned exponent=0;exponent<=17;exponent++) {
            memset(s.segments+length,0,length);s.segments[length]=3;
            exponentiate(&s,exponent,n);
            uint64_t expected=1;for(unsigned i=0;i<exponent;i++)expected*=3;
            for(unsigned i=0;i<length;i++)assert(s.segments[length+i]==(i<8 ? (expected>>(8*i))&255:0));
        }
        memset(s.segments+length,0,length);exponentiate(&s,3,n);
        for(unsigned i=0;i<length;i++)assert(s.segments[length+i]==0);
        /* No preload, invalid segment IDs, and even modulus do not publish. */
        s.modulus_size=0;memset(s.segments+length,0x5a,length);op(&s,1,1,1,false,false);
        for(unsigned i=0;i<length;i++)assert(s.segments[length+i]==0x5a);
        op(&s,255,1,1,false,true);
        for(unsigned i=0;i<length;i++)assert(s.segments[length+i]==0x5a);
        memset(s.segments,0,length);s.segments[0]=2;op(&s,1,1,1,false,true);
        for(unsigned i=0;i<length;i++)assert(s.segments[length+i]==0x5a);
        BN_free(p);BN_free(q);BN_free(n);BN_free(phi);BN_free(e);BN_free(d);
        BN_free(plain);BN_free(signature);BN_CTX_free(ctx);
    }
    IPodTouchPKEState s={0};
    for(unsigned off=0x800;off<0x1000;off+=4) {
        ipod_touch_pke_write(&s,off,off*7,4);
        assert(ipod_touch_pke_read(&s,off,4)==off*7);
    }
    ipod_touch_pke_write(&s,0x1000,0x12345678,4);assert(s.seg_size_reg==0);
    ipod_touch_pke_write(&s,REG_PKE_SEG_SIZE,193,4);assert(s.segment_size==0);
    ipod_touch_pke_write(&s,REG_PKE_START,9,4);
    ipod_touch_pke_write(&s,UINT64_MAX,123,4);assert(ipod_touch_pke_read(&s,UINT64_MAX,4)==0);
    ipod_touch_pke_write(&s,0xfff,0xab,1);assert(ipod_touch_pke_read(&s,0xfff,1)==0xab);
    assert(pke_post_load(&s,2)==0);s.modulus_size=300;assert(pke_post_load(&s,2)==-EINVAL);
    puts("PASS: operand-driven 512/1024/2048-bit RSA, arbitrary exponents, signature compatibility, full SRAM and invalid commands");
}
'''
flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','openssl'],text=True))
with tempfile.TemporaryDirectory() as tmp:
    tmp=Path(tmp);(tmp/'check.c').write_text(code)
    subprocess.run(['cc','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(tmp/'check.c'),'-o',str(tmp/'check'),*flags],check=True)
    subprocess.run([str(tmp/'check')],check=True)
