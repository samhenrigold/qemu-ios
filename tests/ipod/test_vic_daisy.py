#!/usr/bin/env python3
"""Check real PL192 daisy-chain acknowledgment and nested interrupt state."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/intc/pl192.c').read_text()
header = (root / 'include/hw/intc/pl192.h').read_text()
constants = '\n'.join(x for x in header.splitlines() if x.startswith('#define PL192_') and '"' not in x)
state = re.search(r'struct PL192State \{.*?\n};', header, re.S).group()
functions = []
for name in ('pl192_raise', 'pl192_lower', 'pl192_priority_sorter', 'pl192_update',
             'pl192_mask_priority', 'pl192_unmask_priority', 'pl192_irq_ack',
             'pl192_irq_fin', 'pl192_irq_handler'):
    match = re.search(r'^static (?:inline )?[^\n]*\b' + name + r'\([^)]*\)\s*\{.*?^}', source, re.M | re.S)
    assert match, name
    functions.append(match.group())
prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int SysBusDevice;
typedef int MemoryRegion;
typedef int *qemu_irq;
typedef struct PL192State PL192State;
#define IT_IDX(tag,index,size) ((index)<(size)?(index):(size)-1)
#define hw_error(...) assert(0)
static void qemu_irq_raise(qemu_irq q) { *q=1; }
static void qemu_irq_lower(qemu_irq q) { *q=0; }
static void pl192_update(PL192State *);
'''
tests = r'''
static void init(PL192State *s) {
    memset(s,0,sizeof(*s));
    s->intenable=~0u; s->sw_priority_mask=0xffff;
    s->current=s->current_highest=PL192_NO_IRQ;
    s->irq_stack[0]=PL192_NO_IRQ;
    s->priority=s->priority_stack[0]=PL192_PRIO_LEVELS;
    s->daisy_priority=15;
    for (int i=0;i<32;i++) { s->vect_priority[i]=15; s->vect_addr[i]=0x100+i; }
}
int main(void) {
    int irq=0,fiq=0;
    PL192State parent,child;
    init(&parent);init(&child); parent.irq=&irq;parent.fiq=&fiq;child.daisy=&parent;
    child.vect_priority[22]=7;
    pl192_irq_handler(&child,22,1);assert(irq);
    assert(pl192_irq_ack(&parent)==0x116);
    assert(child.current==22 && child.priority==7 && child.stack_i==1);
    assert(parent.current==PL192_DAISY_IRQ && parent.stack_i==1);
    pl192_irq_handler(&child,22,0);
    pl192_irq_fin(&parent);
    assert(child.current==PL192_NO_IRQ && child.priority==16 && child.stack_i==0);
    assert(parent.current==PL192_NO_IRQ && parent.stack_i==0 && !irq);
    /* A higher-priority parent interrupt nests over an active child. */
    parent.vect_priority[4]=2;
    pl192_irq_handler(&child,22,1);pl192_irq_ack(&parent);
    pl192_irq_handler(&parent,4,1);assert(pl192_irq_ack(&parent)==0x104);
    assert(parent.stack_i==2 && child.stack_i==1);
    pl192_irq_handler(&parent,4,0);pl192_irq_fin(&parent);
    assert(parent.current==PL192_DAISY_IRQ && child.current==22);
    pl192_irq_handler(&child,22,0);pl192_irq_fin(&parent);
    assert(!irq && !parent.stack_i && !child.stack_i);
    /* Pending child vectors must be selected again when priority is released;
     * the parent's cached daisy address must not keep the completed vector. */
    child.vect_priority[23]=9;
    pl192_irq_handler(&child,22,1);pl192_irq_ack(&parent);
    pl192_irq_handler(&child,23,1);pl192_irq_handler(&child,22,0);
    pl192_irq_fin(&parent);
    assert(irq && pl192_irq_ack(&parent)==0x117);
    assert(child.current==23 && child.priority==9);
    pl192_irq_handler(&child,23,0);pl192_irq_fin(&parent);
    assert(!irq && !parent.stack_i && !child.stack_i);
    pl192_irq_ack(&parent); /* Spurious ACK must not push NO_IRQ. */
    assert(!parent.stack_i && !child.stack_i);
    puts("PASS: daisy IRQ acknowledges the child vector and restores nested priorities");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'check.c'; c.write_text(prelude+constants+'\n'+state+'\n'+'\n'.join(functions)+tests)
    binary=str(Path(tmp)/'check')
    subprocess.run(['clang','-std=c11','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',binary],check=True)
    subprocess.run([binary],check=True)
