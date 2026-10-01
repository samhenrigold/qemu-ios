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
for name in ('pmu_update_backlight','pmu_update_irq','pmu_latch_event','pmu_adc_complete',
             'pcf50633_adc_for_level','pcf50633_level_for_adc','pmu_charge_active',
             'pmu_apply_battery_adc','pcf50633_update_battery','pcf50633_set_battery_adc',
             'pcf50633_set_battery_level','pcf50633_set_battery_drain','pcf50633_set_charging_mode',
             'pcf50633_set_usb_cable','pcf50633_set_exton1','pmu_adc_command','pmu_bcd','pmu_bcd_rtc_read','pcf50633_recv','pcf50633_guest_shutdown_confirmed',
             'pcf50633_guest_shutdown','pcf50633_send','pcf50633_reset','pcf50633_post_load','pcf50633_init'):
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
#include <math.h>
#include <errno.h>
typedef struct {int unused;} I2CSlave;
typedef struct {bool pending;int64_t deadline;} QEMUTimer;
typedef int *qemu_irq;
typedef void Object;
#define DEVICE(p) (p)
static int init_irq;
static QEMUTimer init_timer;
static void qdev_init_gpio_out(void *object, qemu_irq *irq, int count) { *irq=&init_irq; }
static QEMUTimer *timer_new_ns(int clock, void (*callback)(void *), void *opaque) { return &init_timer; }
#define MAX(a,b) ((a)>(b)?(a):(b))
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
static int brightness=-1;
static void lcd_changebrightness(uint8_t value) { brightness=value; }
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
    Pcf50633State initial={0};pcf50633_init(&initial);pcf50633_reset(&initial);
    /* The dock ID has its own open-circuit input, independent of USB/battery. */
    assert(initial.adc_values[3]==1023 && initial.adc_values[4]==850);
    wr(&initial,0x40,0x23);assert(!init_timer.pending);
    wr(&initial,0x40,0x13);advance(&initial);
    assert(rd(&initial,0x41)==3 && rd(&initial,0x42)==255);
    pcf50633_set_usb_cable(&initial,true);pcf50633_reset(&initial);
    assert(initial.adc_values[3]==1023);
    /* usb-status-reg/-bits at their property defaults: the D1759's power-source status 0x04 bit 3 */
    int irq=0;QEMUTimer timer={0};Pcf50633State s={.irq=&irq,.adc_timer=&timer,.usb_status_reg=0x04,.usb_status_bits=0x08,.shutdown_reg=PMU_SHUTDOWN_REG};
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
    wr(&s,0x40,0x14);s.regs[0x64]=0x31;s.regs[PMU_STANDBY_CMD]=0x90;s.regs[PMU_SHUTDOWN_REG]=0x11;pcf50633_reset(&s);
    assert(s.regs[PMU_SHUTDOWN_REG]==0x10);
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
    /* Fractional drain uses virtual time and retains fractions across ADC polls. */
    pcf50633_set_battery_level(&s,60);
    pcf50633_set_usb_cable(&s,true);
    pcf50633_set_charging_mode(&s,2);
    pcf50633_set_battery_drain(&s,1);
    now+=30000000000LL;pcf50633_update_battery(&s);
    assert(fabs(s.drain_level-59.5)<1e-9);
    now+=30000000000LL;pcf50633_update_battery(&s);
    assert(fabs(s.drain_level-59)<1e-9 && s.adc_values[4]==pcf50633_adc_for_level(59));
    pcf50633_update_battery(&s);assert(s.drain_level==59); /* Paused clock. */
    pcf50633_set_charging_mode(&s,0);
    now+=60000000000LL;pcf50633_update_battery(&s);assert(s.drain_level==59);
    pcf50633_set_usb_cable(&s,false);
    pcf50633_set_battery_drain(&s,0.5);
    now+=120000000000LL;pcf50633_update_battery(&s);assert(s.drain_level==58);
    pcf50633_set_battery_drain(&s,0);
    now+=3600000000000LL;pcf50633_update_battery(&s);assert(s.drain_level==58);
    now-=100;pcf50633_update_battery(&s);assert(s.drain_level==58);
    pcf50633_set_battery_drain(&s,100);
    now+=60000000000LL;pcf50633_update_battery(&s);
    assert(!s.drain_level && s.adc_values[4]==pcf50633_adc_for_level(0));
    pcf50633_set_battery_level(&s,100);pcf50633_set_usb_cable(&s,true);
    now+=60000000000LL;pcf50633_update_battery(&s);assert(s.drain_level==100);
    s.drain_rate=NAN;assert(pcf50633_post_load(&s,4)==-EINVAL);
    s.drain_rate=0;s.drain_level=101;assert(pcf50633_post_load(&s,4)==-EINVAL);
    assert(!pcf50633_post_load(&s,3) && s.drain_rate==0 && s.drain_level==100);
    s.drain_rate=0.25;s.drain_level=55.125;
    uint64_t saved_rate=s.drain_rate_bits,saved_level=s.drain_level_bits;
    s.drain_rate=s.drain_level=0;s.drain_rate_bits=saved_rate;s.drain_level_bits=saved_level;
    assert(!pcf50633_post_load(&s,4) && s.drain_rate==0.25 && s.drain_level==55.125);
    /* Backlight, D1759 defaults: the 0x30 level while 0x10 bit 6 is on. */
    Pcf50633State d={.irq=&irq,.adc_timer=&timer,.backlight_enable_reg=PMU_LDO_ENABLE,
                     .backlight_enable_bit=PMU_LDO_BACKLIGHT,.backlight_level_reg=PMU_DSBL1};
    pcf50633_reset(&d);assert(d.regs[PMU_LDO_ENABLE]&PMU_LDO_BACKLIGHT);
    wr(&d,PMU_DSBL1,0x40);assert(brightness==0x40);
    wr(&d,PMU_LDO_ENABLE,0xa0);assert(brightness==0);
    wr(&d,PMU_LDO_ENABLE,0xe0);assert(brightness==0x40);
    /* The 1G's PCF50633 (1.x): LEDENA 0x29 bit 0, LEDOUT not rendered. 4B1's lock: LEDOUT 1, LEDDIM 1, LEDENA 0. */
    Pcf50633State p={.irq=&irq,.adc_timer=&timer,.backlight_enable_reg=0x29,.backlight_enable_bit=1};
    pcf50633_reset(&p);
    wr(&p,0x28,0x05);wr(&p,0x29,0x01);assert(brightness==255);
    wr(&p,0x28,0x01);wr(&p,0x2b,0x01);assert(brightness==255);
    wr(&p,0x29,0x00);assert(brightness==0);
    wr(&p,0x30,0x40);wr(&p,0x10,0xff);assert(brightness==0); /* the D1759's registers mean nothing here */
    wr(&p,0x29,0x01);assert(brightness==255);
    /* PCF50635: five independent status/mask pairs, including the two
     * registers which the D1759 treats as power-source status. */
    Pcf50633State n={.irq=&irq,.adc_timer=&timer,.rtc_bcd=true,
                     .shutdown_reg=0x0c,.usb_status_reg=0x4b,.usb_status_bits=3};
    pcf50633_reset(&n);
    for(unsigned i=0;i<5;i++) {
        assert(n.regs[2+i]==0 && n.regs[7+i]==0xff);
        pmu_latch_event(&n,2+i,0x40);assert(!irq);
        wr(&n,7+i,0xbf);assert(irq);
        assert(rd(&n,2+i)==0x40 && !irq);
        assert(rd(&n,2+i)==0);
        wr(&n,7+i,0xff);
    }
    pcf50633_set_exton1(&n,true);assert(!irq);
    pcf50633_set_exton1(&n,true);assert(n.regs[3]==4);
    pcf50633_set_exton1(&n,false);assert(n.regs[3]==12);
    wr(&n,8,0xfb);assert(irq);assert(rd(&n,3)==12 && !irq);
    pcf50633_set_usb_cable(&n,true);assert(rd(&n,0x4b)==3);
    assert(rd(&n,2)==4 && !irq);
    pcf50633_set_usb_cable(&n,false);assert(rd(&n,0x4b)==0 && rd(&n,2)==8);
    assert(pcf50633_post_load(&n,4)==-EINVAL);
    pmu_latch_event(&n,6,1);wr(&n,11,0xfe);assert(irq);
    irq=0;assert(pcf50633_post_load(&n,5)==0 && irq);
    n.regs[12]=1;pcf50633_reset(&n);assert(!irq && n.regs[12]==0);
    for(unsigned i=0;i<5;i++) assert(n.regs[2+i]==0 && n.regs[7+i]==0xff);
    puts("PASS: PCF50635 five-bank IRQs, EXTON1 edges, cable events and snapshot rejection; backlight enable/level per register map; D1759 ADC settling/conversion, ten-bit results, masks, cable events, reset, fractional drain and migration");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'check.c';c.write_text(prelude+constants+'\n'+re.search(r'static const uint16_t battery_curve\[\]\[2\] = \{.*?\n};',source,re.S).group()+'\n'+state+'\ntypedef Pcf50633State DeviceState;\n'+'\n'.join(functions)+tests)
    binary=str(Path(tmp)/'check')
    subprocess.run(['clang','-std=gnu11','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',binary],check=True)
    subprocess.run([binary],check=True)
