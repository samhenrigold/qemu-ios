#!/usr/bin/env python3
"""Panel replies are request-driven, bounded, and safe across partial resets."""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'hw/arm/ipod_touch_mipi_dsi.c').read_text()
header=(root/'include/hw/arm/ipod_touch_mipi_dsi.h').read_text()
constants='\n'.join(re.findall(r'^#define (?:REG_|DSIM_|rDSIM_).*$',header,re.M))
state=re.search(r'typedef struct IPodTouchMIPIDSIState\s*\{.*?} IPodTouchMIPIDSIState;',header,re.S).group()
funcs=[]
for name in ('dsi_panel_read','ipod_touch_mipi_dsi_read','ipod_touch_mipi_dsi_write','ipod_touch_mipi_dsi_reset','dsi_post_load'):
    funcs.append(re.search(r'^static [^\n]*\b'+name+r'\([^)]*\)\s*\{.*?^}',source,re.M|re.S).group())
pre=r'''
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
typedef int SysBusDevice, MemoryRegion, qemu_irq;
typedef uint64_t hwaddr;
#define IPOD_TOUCH_MIPI_DSI(s) ((IPodTouchMIPIDSIState*)(s))
#define LOG_UNIMP 1
#define qemu_log_mask(...) ((void)0)
static bool dsi_trace(void) { return false; }
static bool dsi_direct_iboot(void) { return true; }
'''
tests=r'''
#define RD(a) ipod_touch_mipi_dsi_read(&s,a,4)
#define WR(a,v) ipod_touch_mipi_dsi_write(&s,a,v,4)
int main(void) {
    IPodTouchMIPIDSIState s={0};
    assert(!RD(REG_RXFIFO) && !RD(REG_INTSRC));
    WR(REG_PKTHDR,0x2905); assert(!s.rx_count);
    WR(REG_PKTHDR,0xb114); assert(s.rx_count==2);
    assert(RD(REG_INTSRC)==rDSIM_INTSRC_RxDatDone);
    assert(RD(REG_RXFIFO)==0x31a);
    WR(REG_PKTHDR,0xb114); /* unread payload stays ahead of the new reply */
    assert(RD(REG_RXFIFO)==0xa1d13c);
    assert(RD(REG_RXFIFO)==0x31a && RD(REG_RXFIFO)==0xa1d13c);
    assert(!RD(REG_RXFIFO));
    WR(REG_INTSRC,rDSIM_INTSRC_RxDatDone);assert(!RD(REG_INTSRC));
    for(int i=0;i<20;i++) WR(REG_PKTHDR,0xb114);
    assert(s.rx_count==16);
    for(int i=0;i<8;i++) assert(RD(REG_RXFIFO)==0x31a && RD(REG_RXFIFO)==0xa1d13c);
    WR(REG_PKTHDR,0xb114);assert(RD(REG_RXFIFO)==0x31a);
    WR(4,1);assert(!RD(REG_RXFIFO) && !RD(REG_INTSRC));
    WR(REG_PKTHDR,0xb114);assert(RD(REG_RXFIFO)==0x31a);
    ipod_touch_mipi_dsi_reset(&s);assert(!s.rx_count && !s.intsrc);
    WR(REG_PKTHDR,0xb114);assert(RD(REG_RXFIFO)==0x31a);
    assert(!dsi_post_load(&s,2));assert(RD(REG_RXFIFO)==0xa1d13c);
    s.return_panel_id=true;assert(!dsi_post_load(&s,1));assert(RD(REG_RXFIFO)==0xa1d13c);
    s.rx_count=17;assert(dsi_post_load(&s,2)==-EINVAL);
    puts("PASS: DSI request/reply FIFO, wrap/full handling, interrupt ACK, reset and restore");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'check.c';c.write_text(pre+constants+'\n'+state+'\ntypedef IPodTouchMIPIDSIState DeviceState;\n'+'\n'.join(funcs)+tests)
    exe=str(Path(tmp)/'check')
    subprocess.run(['clang','-std=gnu11','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',exe],check=True)
    subprocess.run([exe],check=True)
