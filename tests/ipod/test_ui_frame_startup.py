#!/usr/bin/env python3
"""Frame polling is safe before attach, including concurrent first readers."""
from pathlib import Path
import re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'contrib/ios-app/qemu-ios-ui.c').read_text()
functions = []
for name in ('ios_init_frame_lock', 'qemu_ios_ui_attach', 'qemu_ios_ui_frame', 'qemu_ios_ui_frame_size', 'qemu_ios_ui_copy_frame'):
    match = re.search(r'^(?:static )?[^\n]*\b' + name + r'\([^)]*\)\s*\{.*?^}', source, re.M | re.S)
    assert match, name
    functions.append(match.group())
prelude = r'''
#include <assert.h>
#include <glib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { pthread_mutex_t lock; bool initialized; } QemuMutex;
static void qemu_mutex_init(QemuMutex *m) { assert(!m->initialized); assert(!pthread_mutex_init(&m->lock, NULL)); m->initialized=true; }
static void qemu_mutex_lock(QemuMutex *m) { assert(m->initialized); assert(!pthread_mutex_lock(&m->lock)); }
static void qemu_mutex_unlock(QemuMutex *m) { assert(!pthread_mutex_unlock(&m->lock)); }
typedef void (*qemu_ios_frame_cb)(void *);
static struct {
 QemuMutex frame_lock;
 void *buf[3]; int published, width, height; uint64_t serial;
 qemu_ios_frame_cb cb; void *cb_opaque;
} ios;
'''
tests = r'''
static void *poll(void *unused) {
 for (int i=0;i<10000;i++) {
  const void *pixels=(void *)1; int w=-1,h=-1; uint64_t serial=0;
  assert(!qemu_ios_ui_frame(&pixels,&w,&h,&serial));
  assert(pixels==(void *)1 && w==-1 && h==-1 && serial==0);
  assert(!qemu_ios_ui_copy_frame((void *)&w, sizeof(w), &w, &h));
  qemu_ios_ui_frame_size(&w,&h); assert(w==0 && h==0);
 }
 return NULL;
}
int main(void) {
 pthread_t threads[8];
 for (int i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,poll,NULL));
 qemu_ios_ui_attach(NULL,NULL);
 for (int i=0;i<8;i++) assert(!pthread_join(threads[i],NULL));
 int pixel=42; ios.buf[0]=&pixel; ios.published=0; ios.width=1; ios.height=1; ios.serial=1;
 const void *p=NULL; int w=0,h=0; uint64_t serial=0;
 assert(qemu_ios_ui_frame(&p,&w,&h,&serial));
 assert(p==&pixel && w==1 && h==1 && serial==1);
 qemu_ios_ui_attach(NULL,NULL); /* no reinitialization or frame loss */
 assert(!qemu_ios_ui_frame(&p,&w,&h,&serial));
 int copy=0;
 assert(!qemu_ios_ui_copy_frame(&copy,3,&w,&h)); assert(copy==0);
 assert(qemu_ios_ui_copy_frame(&copy,sizeof(copy),&w,&h));
 pixel=99; assert(copy==42 && w==1 && h==1);
 assert(qemu_ios_ui_copy_frame(&copy,sizeof(copy),&w,&h)); assert(copy==99);
 puts("PASS: concurrent frame polls before attach, empty outputs and retained publication");
}
'''
flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True))
with tempfile.TemporaryDirectory() as tmp:
    path=Path(tmp)/'frame.c'; binary=Path(tmp)/'frame'
    path.write_text(prelude+'\n'.join(functions)+tests)
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-pthread',str(path),'-o',str(binary),*flags],check=True)
    subprocess.run([str(binary)],check=True)
