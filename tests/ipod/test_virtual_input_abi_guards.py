#!/usr/bin/env python3
"""Malformed ordinary host input cannot steal sequence ownership or reach a BH."""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
def function(path,name):
 s=(root/path).read_text()
 m=re.search(r'^void '+name+r'\([^)]*\)\s*\{.*?^}',s,re.M|re.S)
 assert m,name
 return m.group()
functions='\n'.join(function(p,n) for p,n in [
 ('contrib/ios-app/qemu-ios-ui.c','qemu_ios_ui_touch'),
 ('contrib/ios-app/qemu-ios-ui.c','qemu_ios_ui_button'),
 ('contrib/macos-app/qemu-macos-extras.c','qemu_ios_ui_touch2')])
source=r'''
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include "qemu-ios-ui.h"
#define g_new0(T,n) ((T*)calloc((n),sizeof(T)))
struct ios_touch {double nx,ny;bool down;};
struct ios_button {int button;bool down;};
struct mtt_touch {int phase;double nx,ny;};
static bool ready=true;static int submitted;
bool qemu_ios_ui_ready(void) {return ready;}
static void *qemu_get_aio_context(void) {return NULL;}
static void ios_touch_bh(void*p) {free(p);}
static void ios_button_bh(void*p) {free(p);}
static void mtt_bh(void*p) {free(p);}
static void aio_bh_schedule_oneshot(void *ctx,void(*cb)(void*),void*arg) {submitted++;cb(arg);}
'''+functions+r'''
int main(void) {
 for(int phase=-1;phase<=3;phase++)if(phase<0 || phase>2) {
  qemu_ios_ui_touch(0,phase,.5,.5);qemu_ios_ui_touch2(phase,.5,.5);
 }
 double bad[]={NAN,INFINITY,-INFINITY,-.01,1.01};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
  qemu_ios_ui_touch(0,0,bad[i],.5);qemu_ios_ui_touch(0,0,.5,bad[i]);
  qemu_ios_ui_touch2(0,bad[i],.5);qemu_ios_ui_touch2(0,.5,bad[i]);
 }
 qemu_ios_ui_button(-1,true);qemu_ios_ui_button(4,false);
 qemu_ios_ui_touch(1,0,.5,.5);assert(!submitted);
 ready=false;qemu_ios_ui_touch(0,0,.5,.5);qemu_ios_ui_touch2(0,.5,.5);qemu_ios_ui_button(0,true);
 assert(!submitted);ready=true;
 for(int p=0;p<3;p++){qemu_ios_ui_touch(0,p,0,1);qemu_ios_ui_touch2(p,1,0);}
 for(int b=0;b<4;b++){qemu_ios_ui_button(b,true);qemu_ios_ui_button(b,false);}
 assert(submitted==14);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(source)
 subprocess.run(['clang','-Wall','-Werror','-fsanitize=address,undefined','-I',str(root/'contrib/ios-app'),str(p),'-o',d+'/test'],check=True)
 subprocess.run([d+'/test'],check=True)
print('PASS: actual ordinary host touch/button/touch2 ABI rejects bad phase, slot, bounds, NaN/Inf, button and lifecycle before scheduling')
