/* Host button events batched by a busy emulator still have a guest-time hold.
 *
 * Every machine's press hook (iPod touch, iPad, 1G, S5L8920) sees the press and the release.
 *
 * SLICE contrib/ios-app/qemu-ios-ui.c range struct ios_button { | void qemu_ios_ui_button(
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#define QEMU_CLOCK_VIRTUAL 0
#define MAX(a,b) ((a)>(b)?(a):(b))
#define g_free free
#define QEMU_IOS_BUTTON_HOME 0
#define QEMU_IOS_BUTTON_POWER 1
#define QEMU_IOS_BUTTON_VOLUME_UP 2
#define QEMU_IOS_BUTTON_VOLUME_DOWN 3
typedef enum {IPOD_TOUCH_BUTTON_HOME,IPOD_TOUCH_BUTTON_POWER,IPOD_TOUCH_BUTTON_VOLUP,IPOD_TOUCH_BUTTON_VOLDOWN} IPodTouchButton;
typedef struct { void(*cb)(void*);void *arg;int64_t deadline;bool pending; } QEMUTimer;
static void ios_sequence_cancel_current(void) {}
static int64_t now;
static bool pins[4];
static int64_t qemu_clock_get_ms(int clock) { return now; }
static bool ipad_pins[4];
static void ipod_touch_press_button(IPodTouchButton b,bool down) {pins[b]=down;}
static void ipad1_press_button(IPodTouchButton b,bool down) {ipad_pins[b]=down;}
static char g1_pins[8];
static void ipod_touch_1g_press_button(IPodTouchButton b,bool down) {g1_pins[b]=down;}
static bool s5l8920_pins[4];
static void s5l8920_press_button(IPodTouchButton b,bool down) {s5l8920_pins[b]=down;}
static QEMUTimer *timer_new_ms(int c,void(*cb)(void*),void *arg) {
 QEMUTimer *t=calloc(1,sizeof(*t));t->cb=cb;t->arg=arg;return t;
}
static void timer_del(QEMUTimer*t) { t->pending=false; }
static void timer_mod(QEMUTimer*t,int64_t d) {t->deadline=d;t->pending=true;}
#include "slice.h"
static void event(int button,bool down) {
 struct ios_button *b=calloc(1,sizeof(*b));b->button=button;b->down=down;ios_button_bh(b);
}
int main(void) {
 for(int b=0;b<4;b++) {
  now=1000;event(b,true);event(b,false);
  QEMUTimer*t=ios_button_holds[b].release;
  assert(pins[b] && ipad_pins[b] && g1_pins[b] && s5l8920_pins[b] && t->pending && t->deadline==1100);
  now=1100;t->cb(t->arg);assert(!pins[b] && !ipad_pins[b] && !g1_pins[b] && !s5l8920_pins[b]);
  now=2000;event(b,true);now=2500;event(b,false);
  assert(pins[b] && t->deadline==2500);t->cb(t->arg);assert(!pins[b]);
  now=3000;event(b,true);event(b,false);now=3050;event(b,true);
  assert(!t->pending && pins[b]);event(b,false);assert(t->deadline==3150);
  t->cb(t->arg);assert(!pins[b]);free(t);
 }
 event(-1,true);event(4,true);
 puts("PASS: batched button events, long holds, repeated presses, invalid buttons");
}
