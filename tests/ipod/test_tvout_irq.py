#!/usr/bin/env python3
"""Masking a pending TV-out source must retire the external interrupt line."""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'hw/arm/ipod_touch_tvout.c').read_text()
header=(root/'include/hw/arm/ipod_touch_tvout.h').read_text()
constants='\n'.join(x for x in header.splitlines() if x.startswith(('#define SDO_', '#define MXR_')))
functions=[]
for name in ('tvout_ack_vblank','tvout_update_irq','tvout_vblank','ipod_touch_tvout_sdo_write','ipod_touch_tvout_mixer1_write','tvout_post_load','ipod_touch_tvout_reset'):
    match=re.search(r'^static [^\n]*\b'+name+r'\([^)]*\)\s*\{.*?^}',source,re.M|re.S)
    assert match,name
    functions.append(match.group())
prelude=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define TVT(...) ((void)0)
#define QEMU_CLOCK_VIRTUAL 0
typedef uint64_t hwaddr;
typedef struct {
    uint32_t sdo_clkcon,sdo_config,sdo_irq,sdo_irq_mask,mixer1_cfg,mixer1_intstat,mixer1_status,mixer2_status,mixer2_cfg;
    int *irq,*irq2;
    bool irq2_pending;
    int vblank_timer;
} IPodTouchTVOutState;
typedef IPodTouchTVOutState DeviceState;
#define IPOD_TOUCH_TVOUT(s) (s)
static void qemu_set_irq(int *irq,int value) { *irq=!!value; }
static void qemu_irq_lower(int *irq) { *irq=0; }
static int64_t qemu_clock_get_ns(int clock) { return 0; }
static void timer_mod(int timer,int64_t deadline) {}
'''
tests=r'''
int main(void) {
    int irq=0,irq2=0;
    IPodTouchTVOutState s={.irq=&irq,.irq2=&irq2};
    tvout_vblank(&s);assert(!irq); /* Stopped clocks do not generate frames. */
    ipod_touch_tvout_sdo_write(&s,SDO_CLKCON,1,4);
    ipod_touch_tvout_mixer1_write(&s,MXR_STATUS,1,4);
    assert(!irq); /* Register writes must not complete their own frame. */
    tvout_vblank(&s);assert(irq && s.sdo_irq==1 && (s.mixer1_status & 4));
    ipod_touch_tvout_sdo_write(&s,SDO_IRQMASK,1,4);
    assert(!irq && s.sdo_irq==1); /* Status survives; delivery is masked. */
    tvout_vblank(&s);assert(!irq);
    ipod_touch_tvout_sdo_write(&s,SDO_IRQMASK,0,4);assert(irq);
    ipod_touch_tvout_sdo_write(&s,SDO_IRQ,2,4);assert(irq && s.sdo_irq==1);
    ipod_touch_tvout_sdo_write(&s,SDO_IRQ,1,4);assert(!irq && !s.sdo_irq);
    ipod_touch_tvout_mixer1_write(&s,MXR_STATUS,1,4);assert(!irq);
    tvout_vblank(&s);assert(irq);
    ipod_touch_tvout_sdo_write(&s,SDO_IRQMASK,~0u,4);assert(!irq);
    ipod_touch_tvout_sdo_write(&s,SDO_IRQ,~0u,4);assert(!s.sdo_irq);
    ipod_touch_tvout_sdo_write(&s,SDO_IRQMASK,0,4);assert(!irq);
    /* Reproduce the guest handler: ACK then queue another frame. There must
     * be no immediate interrupt feedback loop, however often it writes. */
    for (int i=0;i<100;i++) {
        ipod_touch_tvout_mixer1_write(&s,MXR_STATUS,1,4);
        assert(!irq);
    }
    tvout_vblank(&s);assert(irq);
    ipod_touch_tvout_sdo_write(&s,SDO_IRQ,1,4);
    ipod_touch_tvout_sdo_write(&s,SDO_CLKCON,0,4);
    tvout_vblank(&s);assert(!irq);
    s.sdo_irq=1;s.sdo_irq_mask=1;irq=1;
    tvout_post_load(&s,3);assert(!irq);
    s.sdo_irq_mask=0;tvout_post_load(&s,3);assert(irq);
    s.mixer1_status=7;tvout_post_load(&s,2);assert(!s.mixer1_status);
    s.irq2_pending=true;irq2=1;
    ipod_touch_tvout_reset(&s);
    assert(!irq && !irq2 && !s.sdo_irq && !s.mixer1_status);
    tvout_vblank(&s);assert(!irq);
    puts("PASS: paced TV-out frames, clock gating, masking, W1C and restore");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'check.c';c.write_text(prelude+constants+'\n'+'\n'.join(functions)+tests)
    binary=str(Path(tmp)/'check')
    subprocess.run(['clang','-std=c11','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',binary],check=True)
    subprocess.run([binary],check=True)
