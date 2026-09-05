#!/usr/bin/env python3
"""Exercise actual FMSS register handlers with a deterministic virtual clock."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/ipod_touch_fmss.c').read_text()
header = (root / 'include/hw/arm/ipod_touch_fmss.h').read_text()
functions = []
for name in ('ipod_touch_fmss_read', 'fmss_update_irq', 'fmss_complete',
             'ipod_touch_fmss_write', 'fmss_post_load'):
    match = re.search(r'^static [^\n]*\b' + name + r'\([^)]*\)\s*\{.*?^}', source, re.M | re.S)
    assert match, name
    functions.append(match.group())
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#define QEMU_CLOCK_VIRTUAL 0
#define LOG_UNIMP 0
#define qemu_log_mask(...) ((void)0)
#define IT_SIZE(tag,val,max) ((val) < (max) ? (val) : (max))
typedef uint64_t hwaddr;
typedef struct { int64_t deadline; bool pending; } QEMUTimer;
typedef struct {
    int irq;
    uint32_t reg_cs_ctrl, reg_cs_irq_bit, reg_cs_irq_mask;
    uint32_t reg_cinfo_target_addr, reg_pages_in_addr, reg_cs_buf_addr;
    uint32_t reg_num_pages, reg_page_spare_out_addr, reg_pages_out_addr, reg_csgenrc;
    QEMUTimer *completion_timer;
} IPodTouchFMSSState;
static int level;
static bool fmss_io_failed;
static int64_t now;
static void qemu_set_irq(int irq, int value) { level = !!value; }
static int64_t qemu_clock_get_ns(int clock) { return now; }
static void timer_mod(QEMUTimer *t, int64_t when) { t->deadline=when; t->pending=true; }
static void timer_del(QEMUTimer *t) { t->pending=false; }
static bool fmss_trace_on(void) { return false; }
static void write_chip_info(IPodTouchFMSSState *s) {}
static void read_nand_pages(IPodTouchFMSSState *s) {}
static void write_nand_pages(IPodTouchFMSSState *s) {}
static unsigned fmss_total_blocks(IPodTouchFMSSState *s) { return 2048; }
'''
tests = r'''
static void write_reg(IPodTouchFMSSState *s, unsigned r, unsigned v) {
    ipod_touch_fmss_write(s,r,v,4);
}
static void advance(IPodTouchFMSSState *s) {
    assert(s->completion_timer->pending);
    now=s->completion_timer->deadline;
    s->completion_timer->pending=false;
    fmss_complete(s);
}
int main(void) {
    QEMUTimer timer={0};
    IPodTouchFMSSState s={.completion_timer=&timer,.reg_cs_irq_mask=1};
    write_reg(&s,0xc00,0xffb5); /* iBoot polls completion. */
    assert(!level && !s.reg_cs_irq_bit && timer.pending);
    advance(&s); assert(!level && s.reg_cs_irq_bit==1);
    write_reg(&s,0xc0c,4); assert(s.reg_cs_irq_bit==1); /* W1C independent bits */
    write_reg(&s,0xc0c,1); assert(!s.reg_cs_irq_bit);
    write_reg(&s,0xc00,0xfff5); /* XNU enables the completion IRQ. */
    assert(!level); advance(&s); assert(level);
    write_reg(&s,0xc10,0); assert(!level && s.reg_cs_irq_bit==1);
    assert(ipod_touch_fmss_read(&s,0xc10,4)==0);
    write_reg(&s,0xc10,1); assert(level); /* Unmask an already pending event. */
    write_reg(&s,0xc0c,5); assert(!level && !s.reg_cs_irq_bit);
    write_reg(&s,0xc00,0xfff5); write_reg(&s,0xc00,8);
    assert(!timer.pending && !level); /* Abort cancels deferred completion. */
    write_reg(&s,0xc00,0xfff5); fmss_io_failed=true; advance(&s);
    assert(!level && !s.reg_cs_irq_bit); /* Never report a failed host write as done. */
    fmss_io_failed=false;
    s.reg_cs_irq_bit=1; s.reg_cs_ctrl=0x40; s.reg_cs_irq_mask=1;
    fmss_post_load(&s,2); assert(level);
    s.reg_cs_irq_mask=0; fmss_post_load(&s,2); assert(!level);
    timer.pending=true; fmss_post_load(&s,1);
    assert(level && !timer.pending && s.reg_cs_irq_mask==1);
    puts("PASS: FMSS deferred completion, polling, W1C, masking, abort, failure and restore");
}
'''
constants = '\n'.join(line for line in header.splitlines() if line.startswith('#define FMSS') or line.startswith('#define NAND_PAGES_PER_BLOCK'))
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'check.c'
    c.write_text(prelude + constants + '\n' + '\n'.join(functions) + tests)
    binary = str(Path(tmp) / 'check')
    subprocess.run(['clang', '-std=c11', '-fsanitize=address,undefined', '-g', str(c), '-o', binary], check=True)
    subprocess.run([binary], check=True)
