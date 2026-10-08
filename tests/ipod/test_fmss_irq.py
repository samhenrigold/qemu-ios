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
             'ipod_touch_fmss_write', 'fmss_pre_load', 'fmss_invalid_page',
             'fmss_invalid_marker', 'fmss_post_load', 'ipod_touch_fmss_reset'):
    match = re.search(r'^static [^\n]*\b' + name + r'\([^)]*\)\s*\{.*?^}', source, re.M | re.S)
    assert match, name
    functions.append(match.group())
prelude = r'''
#include <assert.h>
#define trace_event_get_state_backends(id) 0
#define TRACE_PRINTF(fn, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <glib.h>
#define NAND_BYTES_PER_PAGE 4096
#define NAND_BYTES_PER_SPARE 64
#define error_report(...) ((void)0)
#define IPOD_TOUCH_FMSS(p) ((IPodTouchFMSSState *)(p))
typedef void DeviceState;
#define QEMU_CLOCK_VIRTUAL 0
#define fmss_script_trace(...) ((void)0)
#define LOG_UNIMP 0
#define qemu_log_mask(...) ((void)0)
#define IT_SIZE(tag,val,max) ((val) < (max) ? (val) : (max))
typedef uint64_t hwaddr;
typedef struct { int64_t deadline; bool pending; } QEMUTimer;
typedef struct {
    int irq;
    uint32_t reg_cs_ctrl, reg_cs_irq_bit, reg_cs_irq_mask, reg_cs_script;
    uint32_t reg_cinfo_target_addr, reg_pages_in_addr, reg_cs_buf_addr;
    uint32_t reg_num_pages, reg_page_spare_out_addr, reg_pages_out_addr, reg_csgenrc, reg_script_param_d38, reg_script_param_d34, reg_script_param_d48, reg_script_param_d4c, reg_chunks_per_page, reg_script_csgenr15, reg_script_scratch_d7c, reg_script_scratch_d3c;
    GTree *phys_pages, *erased_blocks;
    GHashTable *overlay_pages;
    bool overlay_indexed;
    uint8_t *page_buffer, *page_spare_buffer;
    QEMUTimer *completion_timer;
} IPodTouchFMSSState;
static int level;
static gint key_compare(gconstpointer a, gconstpointer b) {
    return ((uintptr_t)a > (uintptr_t)b) - ((uintptr_t)a < (uintptr_t)b);
}
static bool fmss_physical(void) { return false; }
static void qemu_irq_lower(int irq) { level=0; }
static bool fmss_io_failed;
static int64_t now;
static void qemu_set_irq(int irq, int value) { level = !!value; }
static int64_t qemu_clock_get_ns(int clock) { return now; }
static void timer_mod(QEMUTimer *t, int64_t when) { t->deadline=when; t->pending=true; }
static void timer_del(QEMUTimer *t) { t->pending=false; }
static bool fmss_trace_on(void) { return false; }
static int scripts_run;
static void fmss_run_script(IPodTouchFMSSState *s) { scripts_run++; }
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
    uint8_t page[NAND_BYTES_PER_PAGE], spare[NAND_BYTES_PER_SPARE];
    IPodTouchFMSSState s={.completion_timer=&timer,.reg_cs_irq_mask=1,
        .page_buffer=page,.page_spare_buffer=spare};
    /* Sequencer-owned CSGENR15 is observable in the stock driver dump.
     * Seed the state as a script would, then exercise actual CPU read/reset. */
    s.reg_script_csgenr15=0x12345678u;
    assert(ipod_touch_fmss_read(&s,FMSS_SCRIPT_CSGENR15,4)==0x12345678u);
    s.phys_pages=g_tree_new(key_compare);
    s.erased_blocks=g_tree_new(key_compare);
    write_reg(&s,FMSS_NUM_PAGES,3);
    assert(s.reg_num_pages==3);
    write_reg(&s,FMSS_SCRIPT_PARAM_D4C,0x20011000u);
    assert(ipod_touch_fmss_read(&s,FMSS_SCRIPT_PARAM_D4C,4)==0x20011000u);
    write_reg(&s,FMSS_CHUNKS_PER_PAGE,2);
    assert(ipod_touch_fmss_read(&s,FMSS_CHUNKS_PER_PAGE,4)==2);
    ipod_touch_fmss_reset((DeviceState *)&s);
    assert(ipod_touch_fmss_read(&s,FMSS_SCRIPT_CSGENR15,4)==0);
    assert(ipod_touch_fmss_read(&s,FMSS_CHUNKS_PER_PAGE,4)==0);
    assert(ipod_touch_fmss_read(&s,FMSS_SCRIPT_PARAM_D4C,4)==0);
    write_reg(&s,FMSS_SCRIPT_PARAM_D4C,0xffffffffu);
    assert(ipod_touch_fmss_read(&s,FMSS_SCRIPT_PARAM_D4C,4)==0xffffffffu);
    write_reg(&s,FMSS_CHUNKS_PER_PAGE,0xffffffffu);
    assert(ipod_touch_fmss_read(&s,FMSS_CHUNKS_PER_PAGE,4)==0xffffffffu);
    s.reg_script_csgenr15=0xabcdef12u;
    fmss_pre_load(&s);
    assert(s.reg_script_csgenr15==0); /* Absent in older v4-v8 streams. */
    assert(s.reg_chunks_per_page==0); /* v4-v6 streams omit this parameter. */
    assert(s.reg_script_param_d4c==0); /* v4/v5 streams omit this parameter. */
    s.reg_script_param_d4c=0x20011000u; /* v6 restored field */
    s.reg_chunks_per_page=2; /* v7 restored field */
    fmss_post_load(&s,7);
    assert(s.reg_script_param_d4c==0x20011000u && s.reg_chunks_per_page==2);
    write_reg(&s,0xc04,0x1000); assert(s.reg_cs_script==0x1000 && !scripts_run);
    write_reg(&s,0xc00,0xffb5); /* iBoot polls completion. */
    assert(!level && !s.reg_cs_irq_bit && timer.pending && scripts_run==1);
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
    fmss_post_load(&s,4); assert(level);
    s.reg_cs_irq_mask=0; fmss_post_load(&s,4); assert(!level);
    /* Only certified v4 states can load; no legacy default reconstruction. */
    timer.pending=true; fmss_post_load(&s,4);
    assert(!level && timer.pending && s.reg_cs_irq_mask==0);
    g_tree_destroy(s.phys_pages); g_tree_destroy(s.erased_blocks);
    puts("PASS: FMSS D54/D28/D4C readback/reset/load, deferred completion, polling, W1C, masking, abort, failure and restore");
}
'''
constants = '\n'.join(line for line in header.splitlines() if line.startswith('#define FMSS') or line.startswith('#define NAND_PAGES_PER_BLOCK'))
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'check.c'
    c.write_text(prelude + constants + '\n' + '\n'.join(functions) + tests)
    binary = str(Path(tmp) / 'check')
    glib_flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True).split()
    subprocess.run(['clang', *glib_flags, '-std=c11', '-fsanitize=address,undefined', '-g', str(c), '-o', binary], check=True)
    subprocess.run([binary], check=True)
