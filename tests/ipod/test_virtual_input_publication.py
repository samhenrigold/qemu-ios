#!/usr/bin/env python3
"""Actual QEMU publication/getter against real QEMU atomic/seqlock primitives."""
from pathlib import Path
import re, subprocess, tempfile, shlex
root=Path(__file__).resolve().parents[2]
s=(root/'contrib/ios-app/qemu-ios-ui.c').read_text()
def function(name):
 m=re.search(r'^(?:static )?[^\n]*\b'+name+r'\([^)]*\)\s*\{.*?^}',s,re.M|re.S)
 assert m,name
 return m.group()
source='''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <glib.h>
#include "qemu/typedefs.h"
#define qemu_build_not_reached() __builtin_trap()
#define coroutine_fn
#define qemu_build_assert(test) _Static_assert(test, "atomic-size")
#include "qemu/seqlock.h"
#include "qemu-ios-ui.h"
#include <pthread.h>
static uint64_t ios_sequence_id;
static int ios_sequence_status;
static QemuSeqLock ios_sequence_publication;
static bool done;
'''+function('ios_sequence_publish')+'\n'+function('qemu_ios_ui_input_sequence_status')+r'''
static void *reader(void *arg) {
 while (!qatomic_read(&done)) {
  uint64_t id=qatomic_read(&ios_sequence_id);
  int status=qemu_ios_ui_input_sequence_status(id);
  assert(status==0 || status==(int)(id%3+1));
 }
 return NULL;
}
int main(void) {
 pthread_t threads[4];
 for(int i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,reader,NULL));
 for(uint64_t id=1;id<=500000;id++)ios_sequence_publish(id,id%3+1);
 qatomic_set(&done,true);
 for(int i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
 assert(qemu_ios_ui_input_sequence_status(500000)==500000%3+1);
 assert(qemu_ios_ui_input_sequence_status(1)==0);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(source)
 flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','glib-2.0'],text=True))
 subprocess.run(['clang','-O2','-Wall','-Werror','-fsanitize=address,undefined',
  '-I',str(root/'include'),'-I',str(root/'contrib/ios-app'),
  *flags,str(p),'-o',d+'/test'],check=True)
 subprocess.run([d+'/test'],check=True)
print('PASS: 500000 actual ID/status publications with four concurrent readers, real QEMU seqlock/atomic primitives')
