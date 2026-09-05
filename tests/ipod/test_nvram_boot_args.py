#!/usr/bin/env python3
"""Rewrite boot arguments without damaging CHRP headers or adjacent partitions."""
from pathlib import Path
import shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'hw/arm/ipod_touch_nor_spi.c').read_text()
s=s[s.index('#include <zlib.h>'):s.index('\n}',s.index('static void nor_set_boot_args'))+2]
header=r'''
#include <glib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define error_report(...) ((void)0)
static uint16_t lduw_le_p(const void*p){uint16_t x;memcpy(&x,p,2);return GUINT16_FROM_LE(x);}
static uint32_t ldl_le_p(const void*p){uint32_t x;memcpy(&x,p,4);return GUINT32_FROM_LE(x);}
static void stl_le_p(void*p,uint32_t x){x=GUINT32_TO_LE(x);memcpy(p,&x,4);}
typedef struct {uint8_t*nor_data;const char*boot_args;} IPodTouchNORSPIState;
'''
check=r'''
static void hdr(uint8_t*p,int sig,int units,const char*name){
 p[0]=sig;p[2]=units&255;p[3]=units>>8;memcpy(p+4,name,strlen(name));p[1]=nvram_checksum(p);
}
int main(void){
 uint8_t bank[8192]={0},original[8192];
 hdr(bank,0x5a,2,"nvram");hdr(bank+32,0x70,128,"common");
 hdr(bank+2080,0xa1,129,"APL,OSXPanic");
 memcpy(bank+48,"a=b\0boot-args=old\0z=q\0",sizeof("a=b\0boot-args=old\0z=q\0"));
 memset(bank+2096,0xab,2048);
 stl_le_p(bank+16,adler32(1,bank+20,8172));memcpy(original,bank,8192);
 IPodTouchNORSPIState s={bank,"-v serial=1"};nor_set_boot_args(&s,sizeof(bank));
 assert(!memcmp(bank+48,"a=b\0z=q\0boot-args=-v serial=1\0\0",30));
 assert(!memcmp(bank+2080,original+2080,8192-2080));
 assert(!memcmp(bank+32,original+32,16));
 assert(ldl_le_p(bank+16)==adler32(1,bank+20,8172));
 uint8_t saved[8192];memcpy(saved,bank,8192);nor_set_boot_args(&s,sizeof(bank));assert(!memcmp(bank,saved,8192));
 char oversized[3000];memset(oversized,'x',2999);oversized[2999]=0;s.boot_args=oversized;
 nor_set_boot_args(&s,sizeof(bank));assert(!memcmp(bank,saved,8192));
 s.boot_args="-v";bank[16]^=1;memcpy(saved,bank,8192);nor_set_boot_args(&s,sizeof(bank));assert(!memcmp(bank,saved,8192));
 memcpy(bank,original,8192);memset(bank+48,'x',2032);stl_le_p(bank+16,adler32(1,bank+20,8172));memcpy(saved,bank,8192);
 nor_set_boot_args(&s,sizeof(bank));assert(!memcmp(bank,saved,8192));
}
'''
flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0','zlib'],text=True))
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'check.c';p.write_text(header+s+check);exe=Path(d)/'check'
 subprocess.run(['clang','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(p),'-o',str(exe),*flags],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: CHRP partition boundaries, bank checksum, idempotence, oversized/malformed rejection')
