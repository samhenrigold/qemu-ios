#!/usr/bin/env python3
"""A NAKed absent device must not poison subsequent PMU transactions."""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'hw/i2c/ipod_touch_i2c.c').read_text()
header=(root/'include/hw/i2c/ipod_touch_i2c.h').read_text()
constants='\n'.join(re.findall(r'^#define (?:I2C|IIC|S5L|SR_MODE|ST_MODE|MR_MODE|MT_MODE).*$',header,re.M))
state=re.search(r'typedef struct IPodTouchI2CState \{.*?} IPodTouchI2CState;',header,re.S).group()
funcs=[]
for name in ('s5l8900_i2c_start_addr','s5l8900_i2c_set_ack','s5l8900_i2c_update','s5l8900_i2c_receive','s5l8900_i2c_send','ipod_touch_i2c_read','ipod_touch_i2c_write'):
    funcs.append(re.search(r'^static [^\n]*\b'+name+r'\([^)]*\)\s*\{.*?^}',source,re.M|re.S).group())
pre=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
typedef int SysBusDevice, MemoryRegion;
typedef struct {const char *name;int address;int writes;int last;} I2CBus;
typedef int *qemu_irq;
typedef uint64_t hwaddr;
#define BUS(s) (s)
static bool i2c_trace(void) { return false; }
static bool i2c_nak_enabled(void) { return true; }
static void qemu_irq_raise(int *irq) { *irq=1; }
static void qemu_irq_lower(int *irq) { *irq=0; }
static int i2c_recv(I2CBus *bus) { return 0; }
static int i2c_send(I2CBus *bus,int value) {
    if(bus->address==0x73) { bus->writes++;bus->last=value; }
    return 0;
}
static void i2c_end_transfer(I2CBus *bus) { bus->address=-1; }
static int i2c_start_transfer(I2CBus *bus,int address,int read) { bus->address=address;return address!=0x73; }
'''
claim='static bool i2c_addr_is_claimed(IPodTouchI2CState *s,uint8_t addr) { return addr==0x73; }\n'
tests=r'''
#define WR(a,v) ipod_touch_i2c_write(&s,a,v,4)
int main(void) {
    int irq=0;I2CBus bus={.name="i2c0",.address=-1};
    IPodTouchI2CState s={.bus=&bus,.irq=&irq};
    WR(I2CSTAT,0xd0); WR(I2CDS,0x52);WR(I2CSTAT,0xf0);
    assert(s.active && s.cur_addr==0x29 && (s.status&1));
    WR(I2CSTAT,0xd0);assert(!s.active);
    WR(I2CDS,0xe6);assert(s.data==0xe6 && bus.writes==0);
    WR(I2CSTAT,0xf0);assert(s.active && s.cur_addr==0x73 && !(s.status&1));
    WR(I2CDS,0x40);assert(bus.writes==1 && bus.last==0x40);
    WR(I2CDS,0x14);assert(bus.writes==2 && bus.last==0x14);
    WR(I2CSTAT,0xd0);assert(!s.active);
    /* Repeated START keeps the address even when the data register is an index. */
    WR(I2CDS,0xe6);WR(I2CSTAT,0xf0);WR(I2CDS,0x41);
    assert(s5l8900_i2c_start_addr(&s)==0x73);
    puts("PASS: absent-device NAK followed by PMU address/data and repeated START");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'check.c';c.write_text(pre+constants+'\n'+state+'\n'+claim+'\n'.join(funcs)+tests)
    exe=str(Path(tmp)/'check')
    subprocess.run(['clang','-std=gnu11','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',exe],check=True)
    subprocess.run([exe],check=True)
