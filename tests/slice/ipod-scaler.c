/* Run the actual scaler MMIO and conversion code under ASan/UBSan.
 *
 * SLICE hw/arm/ipod_touch_scaler.c range typedef struct { | static const MemoryRegionOps
 * PKG glib-2.0
 */
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef int SysBusDevice;
typedef int DeviceState;
#define ROUND_UP(n,d) (((n)+(d)-1)/(d)*(d))
typedef int MemoryRegion;
typedef uint64_t hwaddr;
typedef int *qemu_irq;
static uint8_t ram[0x100000];
#define BASE 0x0f600000
static void cpu_physical_memory_read(uint64_t a,void *p,size_t n)
{ assert(a>=BASE && a+n<=BASE+sizeof(ram));memcpy(p,ram+(a-BASE),n); }
static void cpu_physical_memory_write(uint64_t a,const void *p,size_t n)
{ assert(a>=BASE && a+n<=BASE+sizeof(ram));memcpy(ram+(a-BASE),p,n); }
static void stw_le_p(void *p,unsigned v) { uint8_t *b=p;b[0]=v;b[1]=v>>8; }
static void qemu_set_irq(qemu_irq irq,int level) { *irq=level; }
#define error_report(...) ((void)0)
typedef struct { bool pending; int64_t deadline; } QEMUTimer;
enum { QEMU_CLOCK_VIRTUAL };
static int64_t now;
static int64_t qemu_clock_get_ns(int c) { (void)c; return now; }
static void timer_mod(QEMUTimer *t, int64_t at) { t->pending = true; t->deadline = at; }
static void timer_del(QEMUTimer *t) { t->pending = false; }
static bool timer_pending(QEMUTimer *t) { return t->pending; }
#include "slice.h"
static QEMUTimer done;
/* The clock reaches the pending end, and the timer fires. */
static void run_to_end(IPodScalerState *s)
{
    assert(done.pending && done.deadline > now);
    now = done.deadline;
    done.pending = false;
    scaler_complete(s);
}
int main(void)
{
    int irq=0;IPodScalerState s={.irq=&irq,.done=&done};
    uint32_t *r=s.regs;
    /* A transfer stays busy, its done interrupt unraised, until its end: 5 ns a destination pixel, 20 us at least.
     * Done at the start write, an asynchronous transfer's completion freed its request under the kext's submit path. */
    r[2]=1;r[0x40/4]=(640<<16)|960;
    scaler_write(&s,4,1,4);
    assert(!irq && (r[1]&1) && !r[3] && done.deadline==now+640*960*5);
    run_to_end(&s);
    assert(irq && !(r[1]&1) && r[3]==1);
    scaler_write(&s,12,1,4);assert(!irq);
    r[0x40/4]=(2<<16)|2;scaler_write(&s,4,1,4);
    assert(!irq && done.deadline==now+20000);
    scaler_write(&s,4,1,4);   /* started again before its end: the first ends at once */
    assert(irq && r[3]==1 && (r[1]&1) && done.pending);
    scaler_write(&s,4,2,4);   /* reset: nothing pending */
    assert(!irq && !done.pending && !r[1] && !r[2] && !r[3]);
    r[1]=0x200;r[4]=0;r[5]=BASE;r[6]=BASE+256;r[7]=(6<<16)|6;
    r[9]=(4<<16)|2;r[12]=4;r[13]=BASE+512;r[15]=(6<<16)|6;r[16]=r[9];
    unsigned matrix[]={596,0,817,596,0xf38,0xe60,596,1033,0};
    memcpy(r+0x220/4,matrix,sizeof(matrix));
    memset(ram,0xa5,sizeof(ram));
    uint8_t luma[]={16,235,81,81};memcpy(ram,luma,4);memcpy(ram+6,luma,4);
    uint8_t uv[]={128,128,90,240};memcpy(ram+256,uv,4);
    scaler_write(&s,4,0x201,4);
    run_to_end(&s);
    assert(!irq && r[1]==0x200 && scaler_read(&s,12,4)==1);
    uint8_t expected[]={0,0,255,255,0,248,0,248};
    assert(!memcmp(ram+512,expected,8) && !memcmp(ram+524,expected,8));
    for(int i=520;i<524;i++) assert(ram[i]==0xa5);
    scaler_write(&s,8,1,4);assert(irq);
    scaler_write(&s,12,1,4);assert(!irq && !r[3]);
    r[12]=6;r[15]=(6<<16)|6;
    scaler_write(&s,4,0x201,4);assert(!irq);
    run_to_end(&s);assert(irq);
    assert(!memcmp(ram+512,"\0\0\0\xff\xff\xff\xff\xff\0\0\xfe\xff",12));
    r[13]=0x10000000;assert(!scaler_convert(&s));
    r[13]=BASE+512;r[9]|=1;assert(!scaler_convert(&s));
    r[9]&=~1u;r[16]++;assert(!scaler_convert(&s));
    scaler_write(&s,4,2,4);assert(!irq);
    for(unsigned i=0;i<G_N_ELEMENTS(s.regs);i++) assert(!s.regs[i]);
    puts("PASS: NV12 matrix conversion, RGB565/BGRA, padding, DMA bounds, IRQ mask/ack and reset, completion after the transfer's time");
}
