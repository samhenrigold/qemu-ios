#!/usr/bin/env python3
"""Production UART register transitions preserve DMA/timeout consistency."""
from pathlib import Path
import re,subprocess,tempfile
s=(Path(__file__).resolve().parents[2] / 'hw/char/exynos4210_uart.c').read_text()
def fn(name):
 start=s.index('static ',s.index(name)-35); a=s.index('{',s.index(name)); n=1;b=a+1
 while n:
  n+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[start:b]
names=['exynos4210_uart_update_dmabusy','exynos4210_uart_timeout_int','exynos4210_uart_rx_timeout_set','exynos4210_uart_write']
fns=[fn(x) for x in names]
defs='\n'.join(x for x in s.splitlines() if x.startswith('#define ') and '\\' not in x)
traces='\n'.join('#define '+x+'(...) ((void)0)' for x in set(re.findall(r'trace_\w+', '\n'.join(fns))))
code='''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
typedef uint64_t hwaddr;
typedef struct {unsigned size,sp,rp;} FIFO;
typedef struct {uint32_t reg[32]; FIFO rx,tx; unsigned channel; bool s5l8720_irq,rx_since_timeout; int dmairq,rxdmareq,rxdmalast,chr; int *fifo_timeout_timer; uint32_t wordtime;} Exynos4210UartState;
static unsigned levels[4],pulses;
#define QEMU_CLOCK_VIRTUAL 0
static int64_t qemu_clock_get_ns(int c){return 100;}
static void timer_del(int *t){*t=0;}
static void timer_mod(int *t,int64_t deadline){*t=1;}
static void qemu_set_irq(int irq,int level){levels[irq]=level;}
static void qemu_irq_raise(int irq){levels[irq]=1;}
static void qemu_irq_lower(int irq){levels[irq]=0;}
static void qemu_irq_pulse(int irq){pulses++;}
static unsigned fifo_elements_number(FIFO *f){return(f->sp+f->size-f->rp)%f->size;}
static void fifo_reset(FIFO *f){f->sp=f->rp=0;}
#define bt_record(...) ((void)0)
#define qemu_chr_fe_backend_connected(...) 0
#define qemu_chr_fe_write_all(...) ((void)0)
#define exynos4210_uart_update_parameters(...) ((void)0)
static void exynos4210_uart_update_irq(Exynos4210UartState *s);
'''+defs+'\n'+traces+'\n'+fns[0]+'''
static void exynos4210_uart_update_irq(Exynos4210UartState *s){exynos4210_uart_update_dmabusy(s);}
'''+fns[1]+'\n'+fns[2]+'\n'+fns[3]+'''
int main(void){
 int timer=1;
 Exynos4210UartState s={.rx={256,7,0},.tx={256,0,0},.channel=1,.s5l8720_irq=true,.rx_since_timeout=true,.dmairq=0,.rxdmareq=1,.rxdmalast=2,.fifo_timeout_timer=&timer};
 s.reg[I_(UFCON)]=1;s.reg[I_(UCON)]=0x115c80;s.reg[I_(UTRSTAT)]=7;
 exynos4210_uart_write(&s,UCON,0x115c8f,4);
 assert(levels[1]==1);
 exynos4210_uart_update_dmabusy(&s);
 exynos4210_uart_write(&s,UCON,0x115c80,4);
 assert(levels[1]==0 && timer==0);
 s.reg[I_(UCON)]=0x115c8f; timer=1;
 exynos4210_uart_write(&s,UFCON,3,4);
 assert(!fifo_elements_number(&s.rx) && !levels[1]);
 assert(!(s.reg[I_(UTRSTAT)] & (UTRSTAT_Rx_BUFFER_DATA_READY|UTRSTAT_Rx_TIMEOUT)));
 assert(!s.rx_since_timeout && !timer);
 exynos4210_uart_timeout_int(&s);
 assert(!pulses);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'check.c';p.write_text(code)
 subprocess.run(['clang','-fsanitize=address,undefined',str(p),'-o',d+'/check'],check=True)
 subprocess.run([d+'/check'],check=True)

print("PASS: UART DMA mode transitions and FIFO reset cancel stale packet completion")
