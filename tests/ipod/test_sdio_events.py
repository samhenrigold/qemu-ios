#!/usr/bin/env python3
"""Decode production Broadcom events at the offsets used by both guest drivers."""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'hw/arm/ipod_touch_sdio.c').read_text()
header=(root/'include/hw/arm/ipod_touch_sdio.h').read_text()
constants='\n'.join(re.findall(r'^#define (?:BDC_|BCMETH_|ETHER_TYPE_BRCM|WL_EVENT_MSG_LEN|SDPCM_(?:EVENT|DATA)_CHANNEL).*$',header,re.M))
code=r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define trace_sdio(...) ((void)0)
typedef struct { unsigned bdc_hdrlen; } IPodTouchSDIOState;
static uint8_t bytes[128];static unsigned length,channel;
static void stw_be_p(uint8_t *p,uint16_t v) {p[0]=v>>8;p[1]=v;}
static void stl_be_p(uint8_t *p,uint32_t v) {p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static void g_strlcpy(char *p,const char *s,unsigned n) {snprintf(p,n,"%s",s);}
static void sdpcm_send(IPodTouchSDIOState *s,unsigned c,uint8_t *p,unsigned n) {
 assert(n<=sizeof(bytes));memcpy(bytes,p,n);length=n;channel=c;
}
''' + constants + '\n'
for name in ('sdio_bdc_hdrlen','sdpcm_send_event'):
    match=re.search(r'^static [^\n]*\b'+name+r'\([^;]*?\n\{.*?^}',source,re.M|re.S)
    assert match,name
    code+=match.group()+'\n'
code+=r'''
static unsigned be16(uint8_t *p) {return p[0]<<8|p[1];}
static uint32_t be32(uint8_t *p) {return (uint32_t)p[0]<<24|p[1]<<16|p[2]<<8|p[3];}
int main(void) {
 for(unsigned hdr=0;hdr<=6;hdr+=2) {
  if(hdr==2)continue;
  IPodTouchSDIOState s={hdr};sdpcm_send_event(&s,0x12345678,0x87654321,0xabcd);
  unsigned bdc=hdr?hdr:6;
  assert(length==bdc+70 && channel==(bdc==4?1:2));
  assert(bytes[0]==0x20);for(unsigned i=1;i<bdc;i++)assert(!bytes[i]);
  uint8_t *eth=bytes+bdc;
  assert(!memcmp(eth,"\x00\x23\x32\x6e\xaa\x10",6));
  assert(be16(eth+12)==0x886c && be16(eth+14)==0x8001);
  assert(be16(eth+16)==52);
  assert(!memcmp(eth+19,"\x00\x10\x18",3));assert(be16(eth+22)==1);
  uint8_t *msg=eth+24;
  assert(be16(msg)==1 && be16(msg+2)==0xabcd);
  assert(be32(msg+4)==0x12345678 && be32(msg+8)==0x87654321);
  assert(!be32(msg+12)&&!be32(msg+16)&&!be32(msg+20));
  assert(!memcmp(msg+24,eth,6)&&!strcmp((char*)msg+30,"en0"));
 }
 puts("PASS: 2.1.1/3.1.3 event channels, BDC padding, OUI, lengths and endian fields");
}
'''
with tempfile.TemporaryDirectory() as d:
    path=Path(d)/'check.c';path.write_text(code)
    subprocess.run(['clang','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(path),'-o',d+'/check'],check=True)
    subprocess.run([d+'/check'],check=True)
