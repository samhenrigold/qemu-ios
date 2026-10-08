/* Exercise production PMGR with a deterministic virtual clock, no firmware.
 *
 * A watchdog armed once must expire without further register writes. Disabling or feeding it must cancel the
 * previous deadline. Event timers and PLL/gate polling share the block and must keep their existing behavior.
 *
 * SLICE hw/arm/s5l8930_pmgr.c define .*
 * SLICE hw/arm/s5l8930_pmgr.c range typedef struct S5L8930EventTimer | /*\n * Reset values
 * SLICE hw/arm/s5l8930_pmgr.c fn pmgr_modeled pmgr_ticks evt_remaining evt_arm evt_expire evt_write_state wdog_count wdog_expire wdog_check s5l8930_pmgr_read s5l8930_pmgr_write
 */
#include <assert.h>
#define trace_event_get_state_backends(id) 0
#define TRACE_PRINTF(fn, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
typedef uint64_t hwaddr;
typedef struct { int unused; } SysBusDevice;
typedef struct { int unused; } MemoryRegion;
typedef struct { bool pending; int64_t deadline; } QEMUTimer;
typedef int *qemu_irq;
typedef struct S5L8930PMGRState S5L8930PMGRState;
#define S5L8930_PMGR_SIZE 0x10000
#define QEMU_CLOCK_VIRTUAL 0
#define NANOSECONDS_PER_SECOND 1000000000ULL
#define LOG_UNIMP 0
#define LOG_GUEST_ERROR 0
#define SHUTDOWN_CAUSE_GUEST_RESET 2
#define MIN(a,b) ((a)<(b)?(a):(b))
static int64_t now;
static int resets;
static uint64_t muldiv64(uint64_t a,uint64_t b,uint64_t c) {return (__uint128_t)a*b/c;}
static int64_t qemu_clock_get_ns(int clock) {return now;}
static void timer_mod(QEMUTimer *t,int64_t at) {t->pending=true;t->deadline=at;}
static void timer_del(QEMUTimer *t) {t->pending=false;}
static bool timer_pending(QEMUTimer *t) {return t->pending;}
static void qemu_irq_raise(qemu_irq irq) {*irq=1;}
static void qemu_irq_lower(qemu_irq irq) {*irq=0;}
static void qemu_system_reset_request(int cause) {assert(cause==2);resets++;}
static void qemu_log_mask(int mask,const char *fmt,...) {}
#include "slice.h"
static void wr(S5L8930PMGRState *s,hwaddr off,uint32_t v) {s5l8930_pmgr_write(s,off,v,4);}
static uint32_t rd(S5L8930PMGRState *s,hwaddr off) {return s5l8930_pmgr_read(s,off,4);}
static void expire(S5L8930PMGRState *s) {assert(s->wdog_timer->pending);now=s->wdog_timer->deadline;s->wdog_timer->pending=false;wdog_expire(s);}
int main(void) {
 QEMUTimer timer={0},event={0};int irq=0;
 S5L8930PMGRState s={.wdog_timer=&timer};s.evt[0].timer=&event;s.evt[0].irq=&irq;
 wr(&s,PMGR_WDOG_RST,24000);wr(&s,PMGR_WDOG_TMR,0);wr(&s,PMGR_WDOG_CTL,4);
 assert(timer.pending&&timer.deadline==1000001);now=500000;assert(rd(&s,PMGR_WDOG_TMR)==12000);assert(!resets);
 wr(&s,PMGR_WDOG_TMR,0);assert(timer.deadline==1500001);expire(&s);assert(resets==1);
 wr(&s,PMGR_WDOG_CTL,0);assert(!timer.pending);uint32_t frozen=rd(&s,PMGR_WDOG_TMR);now+=5000000;assert(rd(&s,PMGR_WDOG_TMR)==frozen);
 wr(&s,PMGR_WDOG_TMR,0);wr(&s,PMGR_WDOG_CTL,4);assert(timer.pending);wr(&s,PMGR_WDOG_RST,48000);int64_t at=timer.deadline;assert(at==now+2000001);
 now+=500000;wr(&s,PMGR_WDOG_CTL,0);assert(!timer.pending);assert(rd(&s,PMGR_WDOG_TMR)==12000);
 wr(&s,PMGR_WDOG_CTL,4);assert(timer.deadline==at);expire(&s);assert(resets==2);
 wr(&s,PMGR_WDOG_CTL,0);wr(&s,PMGR_WDOG_RST,0);wr(&s,PMGR_WDOG_TMR,0);wr(&s,PMGR_WDOG_CTL,4);assert(resets==3&&!timer.pending);
 wr(&s,PMGR_WDOG_CTL,0);now=0;wr(&s,PMGR_EVT_COUNT(0),24000);wr(&s,PMGR_EVT_STATE(0),3);assert(event.pending&&event.deadline==1000001);now=500000;assert(rd(&s,PMGR_EVT_COUNT(0))==12000);evt_expire(&s.evt[0]);assert(irq);wr(&s,PMGR_EVT_STATE(0),3);assert(!irq);
 wr(&s,GATE_START,0x80000005);assert(rd(&s,GATE_START)==0x55);wr(&s,0x00,PLL_CON0_ENABLE|PLL_CON0_UPDATE);assert((rd(&s,0)&(PLL_CON0_LOCKED|PLL_CON0_UPDATE))==PLL_CON0_LOCKED);wr(&s,0,0);assert(!(rd(&s,0)&PLL_CON0_LOCKED));
 puts("PASS PMGR watchdog expiry/feed/disable, immediate reset, event timer and clock polls");
}
