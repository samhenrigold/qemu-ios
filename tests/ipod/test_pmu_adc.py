#!/usr/bin/env python3
"""D1759 conversion sequencing, event masks and read-to-clear interrupt levels."""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'hw/arm/ipod_touch_pcf50633_pmu.c').read_text()
header=(root/'include/hw/arm/ipod_touch_pcf50633_pmu.h').read_text()
constants='\n'.join(re.findall(r'^#define PMU_.*$',header,re.M))
state=re.search(r'typedef struct Pcf50633State \{.*?\n} Pcf50633State;',header,re.S).group()
functions=[]
for name in ('pmu_update_irq','pmu_latch_event','pmu_adc_complete','pmu_adc_command',
             'pcf50633_adc_for_level','pcf50633_level_for_adc','pmu_charge_active',
             'pcf50633_set_battery_adc','pcf50633_set_charging_mode',
             'pcf50633_set_usb_cable','pcf50633_recv','pcf50633_guest_shutdown_confirmed',
             'pcf50633_guest_shutdown','pcf50633_send','pcf50633_reset','pcf50633_post_load'):
    match=re.search(r'^(?:static )?[^\n]*\b'+name+r'\([^)]*\)\s*\{.*?^}',source,re.M|re.S)
    assert match,name
    functions.append(match.group())
prelude=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
typedef struct {int unused;} I2CSlave;
typedef struct {bool pending;int64_t deadline;} QEMUTimer;
typedef int *qemu_irq;
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define PCF50633(s) ((Pcf50633State *)(s))
#define QEMU_CLOCK_VIRTUAL 0
#define qatomic_read(p) (*(p))
#define qatomic_set(p,v) (*(p)=(v))
#define SHUTDOWN_CAUSE_GUEST_SHUTDOWN 1
static bool guest_shutdown_confirmed;
static int64_t now;
static void qemu_set_irq(int *irq,int value) { *irq=!!value; }
static int64_t qemu_clock_get_ns(int clock) { return now; }
static void timer_del(QEMUTimer *t) { t->pending=false; }
static void timer_mod(QEMUTimer *t,int64_t at) { t->pending=true;t->deadline=at; }
static bool pmu_trace(void) { return false; }
static void pmu_trace_access(const char *s,uint8_t r,uint8_t v) {}
static void lcd_changebrightness(uint8_t value) {}
static void qemu_system_shutdown_request(int cause) {}
'''
tests=r'''
static void wr(Pcf50633State *s,uint8_t reg,uint8_t value) {
    s->addressing=true;pcf50633_send((I2CSlave*)s,reg);pcf50633_send((I2CSlave*)s,value);
}
static unsigned rd(Pcf50633State *s,uint8_t reg) {
    s->curreg=reg;return pcf50633_recv((I2CSlave*)s);
}
static void advance(Pcf50633State *s) {
    assert(s->adc_timer->pending);now=s->adc_timer->deadline;
    s->adc_timer->pending=false;pmu_adc_complete(s);
}
int main(void) {
    int irq=0;QEMUTimer timer={0};Pcf50633State s={.irq=&irq,.adc_timer=&timer};
    pcf50633_reset(&s);
    wr(&s,0x40,0x23);assert(!timer.pending && !irq && !s.regs[2]);
    const unsigned counts[]={0,1,3,4,850,1022,1023};
    for (unsigned i=0;i<sizeof(counts)/sizeof(*counts);i++) {
        s.adc_values[4]=counts[i];wr(&s,0x40,0x14);
        assert(timer.pending && !irq);
        s.adc_values[4]=0; /* conversion samples its selected input */
        advance(&s);assert(s.regs[2]==0x20 && !irq);
        assert(rd(&s,0x41)==(counts[i]&3));
        assert(pcf50633_recv((I2CSlave*)&s)==(counts[i]>>2));
        assert(!(rd(&s,0x40)&0x10));
        wr(&s,0x08,0xdf);assert(irq); /* unmask an already completed conversion */
        assert(rd(&s,1)==0 && irq);
        assert(pcf50633_recv((I2CSlave*)&s)==0x20 && !irq);
        assert(pcf50633_recv((I2CSlave*)&s)==0);
        wr(&s,0x08,0xff);
    }
    wr(&s,0x40,0x14);wr(&s,0x40,0);assert(!timer.pending);
    pmu_latch_event(&s,1,8);pmu_latch_event(&s,3,0x40);
    assert(!irq);wr(&s,7,0xf7);assert(irq);
    assert(rd(&s,3)==0x40 && irq);assert(rd(&s,1)==8 && !irq);
    pcf50633_set_usb_cable(&s,true);assert(rd(&s,4)&8);assert(irq);
    assert(rd(&s,1)==8 && !irq);
    assert(rd(&s,5)&6);
    wr(&s,0x0a,8);assert(!(rd(&s,5)&6));
    wr(&s,0x0a,0);assert(rd(&s,5)&6);
    pcf50633_set_usb_cable(&s,true);assert(!irq); /* no repeated edge */
    wr(&s,4,0xff); /* software cannot override the live cable input */
    pcf50633_set_usb_cable(&s,false);assert(!(rd(&s,4)&8) && irq);
    assert(!(rd(&s,5)&6));
    wr(&s,0x4b,0x55);wr(&s,0x57,0xaa); /* these are not ADC status registers */
    assert(rd(&s,0x4b)==0x55 && rd(&s,0x57)==0xaa);
    irq=0;pcf50633_post_load(&s,2);assert(irq);
    wr(&s,0x40,0x14);s.regs[0x64]=0x31;s.regs[PMU_STANDBY_CMD]=0x90;pcf50633_reset(&s);
    assert(s.regs[PMU_STANDBY_CMD]==0);
    assert(!irq && !timer.pending && s.regs[0x64]==0x31);
    assert(s.regs[7]==0xff && s.regs[8]==0xff && s.regs[9]==0xff);
    unsigned last=0;
    for(unsigned i=0;i<=100;i++) {
        unsigned count=pcf50633_adc_for_level(i);
        assert(count>=last && count<=1023);last=count;
    }
    assert(pcf50633_adc_for_level(20)==660);
    assert(pcf50633_adc_for_level(60)==726);
    assert(pcf50633_adc_for_level(100)==870);
    assert(pcf50633_level_for_adc(660)==20);
    assert(pcf50633_level_for_adc(726)==60);
    assert(pcf50633_level_for_adc(870)==100);
    pcf50633_set_battery_adc(&s,850);pcf50633_set_usb_cable(&s,true);
    assert(rd(&s,5)&6);
    wr(&s,9,0xfb);pcf50633_set_charging_mode(&s,2);
    assert(!(rd(&s,5)&6) && irq);assert(rd(&s,3)==4);
    pcf50633_set_charging_mode(&s,0);assert(rd(&s,5)&6);rd(&s,3);
    pcf50633_set_battery_adc(&s,870);assert(!(rd(&s,5)&6) && irq);rd(&s,3);
    pcf50633_set_charging_mode(&s,1);assert(rd(&s,5)&6);rd(&s,3);
    pcf50633_set_usb_cable(&s,false);assert(!(rd(&s,5)&6));
    puts("PASS: D1759 ADC settling/conversion, ten-bit results, masks, cable events and reset");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'check.c';c.write_text(prelude+constants+'\n'+re.search(r'static const uint16_t battery_curve\[\]\[2\] = \{.*?\n};',source,re.S).group()+'\n'+state+'\ntypedef Pcf50633State DeviceState;\n'+'\n'.join(functions)+tests)
    binary=str(Path(tmp)/'check')
    subprocess.run(['clang','-std=gnu11','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',binary],check=True)
    subprocess.run([binary],check=True)
