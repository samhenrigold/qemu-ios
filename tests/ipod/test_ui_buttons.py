#!/usr/bin/env python3
"""Host button events batched by a busy emulator still have a guest-time hold."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[2]
s = (root/'contrib/ios-app/qemu-ios-ui.c').read_text()
s = s[s.index('struct ios_button {'):s.index('void qemu_ios_ui_button(')]
header = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define QEMU_CLOCK_VIRTUAL 0
#define MAX(a,b) ((a)>(b)?(a):(b))
#define g_free free
#define QEMU_IOS_BUTTON_HOME 0
#define QEMU_IOS_BUTTON_POWER 1
#define QEMU_IOS_BUTTON_VOLUME_UP 2
#define QEMU_IOS_BUTTON_VOLUME_DOWN 3
typedef enum {IPOD_TOUCH_BUTTON_HOME,IPOD_TOUCH_BUTTON_POWER,IPOD_TOUCH_BUTTON_VOLUP,IPOD_TOUCH_BUTTON_VOLDOWN} IPodTouchButton;
typedef struct { void(*cb)(void*);void *arg;int64_t deadline;bool pending; } QEMUTimer;
static int64_t now;
static bool pins[4];
static int64_t qemu_clock_get_ms(int clock) { return now; }
static void ipod_touch_press_button(IPodTouchButton b,bool down) {pins[b]=down;}
static QEMUTimer *timer_new_ms(int c,void(*cb)(void*),void *arg) {
 QEMUTimer *t=calloc(1,sizeof(*t));t->cb=cb;t->arg=arg;return t;
}
static void timer_del(QEMUTimer*t) { t->pending=false; }
static void timer_mod(QEMUTimer*t,int64_t d) {t->deadline=d;t->pending=true;}
'''
check = r'''
static void event(int button,bool down) {
 struct ios_button *b=calloc(1,sizeof(*b));b->button=button;b->down=down;ios_button_bh(b);
}
int main(void) {
 for(int b=0;b<4;b++) {
  now=1000;event(b,true);event(b,false);
  QEMUTimer*t=ios_button_holds[b].release;
  assert(pins[b] && t->pending && t->deadline==1100);
  now=1100;t->cb(t->arg);assert(!pins[b]);
  now=2000;event(b,true);now=2500;event(b,false);
  assert(pins[b] && t->deadline==2500);t->cb(t->arg);assert(!pins[b]);
  now=3000;event(b,true);event(b,false);now=3050;event(b,true);
  assert(!t->pending && pins[b]);event(b,false);assert(t->deadline==3150);
  t->cb(t->arg);assert(!pins[b]);free(t);
 }
 event(-1,true);event(4,true);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'check.c';p.write_text(header+s+check)
 exe=Path(d)/'check'
 subprocess.run(['clang','-fsanitize=address,undefined',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: batched button events, long holds, repeated presses, invalid buttons')
