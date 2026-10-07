/* Synthetic DRAM, actual non-copyrighted N72 7E18 command words.
 * Does not contain raw guest RAM or Apple firmware bytes. */
#include "hw/arm/mbx_fill.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define BASE 0x08000000u
#define SIZE 0x00200000u
static uint8_t ram[SIZE];
static unsigned writes, max_span;
static uint32_t mapped_base = BASE;
static uint8_t *span(void *ctx, uint32_t pa, uint32_t n) {
    (void)ctx;if(n>max_span)max_span=n;
    return pa>=mapped_base && (uint64_t)pa+n<=(uint64_t)mapped_base+SIZE ? ram+(pa-mapped_base):NULL;
}
static void store(void *ctx,uint32_t pa,uint32_t v) {uint8_t *p=span(ctx,pa,4);assert(p);writes++;for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static uint32_t load(uint32_t pa) {uint8_t *p=span(NULL,pa,4);assert(p);return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static MBXFillState m;
static MBXFillBus bus={.host_ram=span,.write32=store,.mmu_enabled=true};
static uint32_t packet[16]={0xa0060500,0x00823000,0x94060500,0,0x30000000,0x60800200,0x8000f0f0,0xff000000,0,0x014001e0,0x70000000,0x70000000,0x70000000,0x70000000,0x70000000,0x70000000};
static void setup(void) {
    mapped_base=BASE;bus.mmu_enabled=true;
    mbx_fill_reset(&m);memset(ram,0x5a,sizeof ram);
    memset(ram+0x1000,0,4096);
    assert(mbx_fill_write(&m,&bus,0x1008,0x08001000,NULL)==MBX_FILL_IGNORED);
    for(unsigned i=0;i<150;i++)store(NULL,0x08001000+(0x23+i)*4,0x08040000+i*8192);
    writes=0;max_span=0;
}
static void copy_packet(uint32_t off,const uint32_t *p,unsigned n) {
    for(unsigned i=0;i<n;i++)assert(mbx_fill_write(&m,&bus,off+i*4,p[i],NULL)==MBX_FILL_IGNORED);
    assert(writes==0);
}
static MBXFillResult submit(const char **why) {return mbx_fill_write(&m,&bus,0xa00000,0xf0000000,why);}
int main(void) {
    setup();copy_packet(0xa00000,packet,16);assert(submit(NULL)==MBX_FILL_DONE);
    assert(writes==153600 && max_span<=4096 && m.pending_count==0);
    for(unsigned page=0;page<150;page++) {
        uint32_t pa=0x08040000+page*8192;
        for(unsigned i=0;i<4096;i+=4)assert(load(pa+i)==0xff000000);
        assert(load(pa+4096)==0x5a5a5a5a); /* Discontiguous gap preserved. */
    }
    setup();copy_packet(0xa00040,packet,16);
    MBXFillState saved=m; memset(&m,0,sizeof m);m=saved; /* In-flight leaf state roundtrip. */
    assert(submit(NULL)==MBX_FILL_DONE && writes==153600);
    setup();copy_packet(0xa00000,packet,15);const char *why=NULL;
    assert(submit(&why)==MBX_FILL_REJECTED && why && !writes);
    setup();uint32_t p[16];memcpy(p,packet,sizeof p);p[6]=0x8000cccc;
    copy_packet(0xa00000,p,16);assert(submit(&why)==MBX_FILL_REJECTED && !writes);
    setup();copy_packet(0xa00000,packet,16);store(NULL,0x08001000+(0x23+149)*4,0);writes=0;
    assert(submit(&why)==MBX_FILL_REJECTED && !writes); /* Late hole atomic. */
    setup();copy_packet(0xa00000,packet,16);m.roots[2]=0x3b000000;
    assert(submit(&why)==MBX_FILL_REJECTED && !writes); /* MMIO root rejected. */
    setup();copy_packet(0xa00000,packet,16);store(NULL,0x08001000+0x23*4,0x3b000000);writes=0;
    assert(submit(&why)==MBX_FILL_REJECTED && !writes); /* MMIO target rejected. */
    setup();copy_packet(0xa00000,packet,16);copy_packet(0xa00040,packet,16);
    assert(submit(&why)==MBX_FILL_REJECTED && !writes); /* Multi-command unsupported. */
    setup();copy_packet(0xa0fff0,packet,4);assert(submit(&why)==MBX_FILL_REJECTED && !writes);
    setup();copy_packet(0xa00000,packet,16);mbx_fill_reset(&m);
    assert(submit(&why)==MBX_FILL_REJECTED && !writes);
    setup();copy_packet(0xa00000,packet,16);
    store(NULL,0x08001000+0x23*4,0x08001000);writes=0;
    assert(submit(&why)==MBX_FILL_DONE && writes==153600);
    /* The first target destroyed the PTEs; the frozen remaining translations
     * still write every later original target, with no retranslation fault. */
    for(unsigned page=1;page<150;page++)
        assert(load(0x08040000+page*8192)==0xff000000);
    setup();memcpy(p,packet,sizeof p);p[7]=0xffffffff;
    copy_packet(0xa00000,p,16);assert(submit(&why)==MBX_FILL_REJECTED && !writes);
    setup();memcpy(p,packet,sizeof p);p[9]=0x014001df;
    copy_packet(0xa00000,p,16);assert(submit(&why)==MBX_FILL_REJECTED && !writes);
    setup();bus.mmu_enabled=false;copy_packet(0xa00000,packet,16);
    assert(submit(&why)==MBX_FILL_REJECTED && !writes);
    assert(load(0x08040000)==0x5a5a5a5a); /* Disabled MMU never follows saved roots. */
    setup();bus.mmu_enabled=false;mapped_base=0x00800000;
    memset(m.roots,0xff,sizeof m.roots);copy_packet(0xa00000,packet,16);
    assert(submit(NULL)==MBX_FILL_DONE && writes==153600);
    for(unsigned i=0;i<150*4096;i+=4)assert(load(packet[1]+i)==0xff000000);
    puts("PASS: actual N72 fill encoding on synthetic discontiguous pages; 153600 exact writes; <=4096-byte spans; in-flight leaf state; reset; malformed/unsupported/batch/MMIO/late PTE rejection without writes; frozen alias translations; unmeasured color/geometry refusal");
}
