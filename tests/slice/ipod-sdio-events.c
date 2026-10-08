/* Decode production Broadcom events at the offsets used by both guest drivers.
 *
 * SLICE include/hw/arm/ipod_touch_sdio.h define BDC_|BCMETH_|ETHER_TYPE_BRCM|WL_EVENT_MSG_LEN|SDPCM_(EVENT|DATA)_CHANNEL
 * SLICE hw/arm/ipod_touch_sdio.c fn sdio_bdc_hdrlen sdpcm_send_event
 */
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define trace_sdio(...) ((void)0)
typedef struct { unsigned bdc_hdrlen; struct { uint8_t mac[6]; } chip; } IPodTouchSDIOState;
static uint8_t bytes[128];static unsigned length,channel;
static void stw_be_p(uint8_t *p,uint16_t v) {p[0]=v>>8;p[1]=v;}
static void stl_be_p(uint8_t *p,uint32_t v) {p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static void g_strlcpy(char *p,const char *s,unsigned n) {snprintf(p,n,"%s",s);}
static void sdpcm_send(IPodTouchSDIOState *s,unsigned c,uint8_t *p,unsigned n) {
 assert(n<=sizeof(bytes));memcpy(bytes,p,n);length=n;channel=c;
}
#include "slice.h"
static unsigned be16(uint8_t *p) {return p[0]<<8|p[1];}
static uint32_t be32(uint8_t *p) {return (uint32_t)p[0]<<24|p[1]<<16|p[2]<<8|p[3];}
int main(void) {
 for(unsigned hdr=0;hdr<=6;hdr+=2) {
  if(hdr==2)continue;
  IPodTouchSDIOState s={hdr, {{0x02,0x9f,0xef,0x8e,0x4a,0xf8}}};sdpcm_send_event(&s,0x12345678,0x87654321,0xabcd);
  unsigned bdc=hdr?hdr:6;
  assert(length==bdc+76 && channel==(bdc==4?1:2));
  assert(bytes[0]==0x20);for(unsigned i=1;i<bdc;i++)assert(!bytes[i]);
  uint8_t *eth=bytes+bdc;
  assert(!memcmp(eth,"\x02\x9f\xef\x8e\x4a\xf8",6));
  assert(be16(eth+12)==0x886c && be16(eth+14)==0x8001);
  assert(be16(eth+16)==58);
  assert(!memcmp(eth+19,"\x00\x10\x18",3));assert(be16(eth+22)==1);
  uint8_t *msg=eth+24;
  assert(be16(msg)==1 && be16(msg+2)==0xabcd);
  assert(be32(msg+4)==0x12345678 && be32(msg+8)==0x87654321);
  assert(!be32(msg+12)&&!be32(msg+16)&&!be32(msg+20));
  assert(!memcmp(msg+24,eth,6)&&!strcmp((char*)msg+30,"en0"));
  for(unsigned i=46;i<WL_EVENT_MSG_LEN;i++)assert(!msg[i]); /* the 7.x tail */
 }
 puts("PASS: 2.1.1/3.1.3 event channels, BDC padding, OUI, lengths and endian fields");
}
