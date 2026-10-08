/* Compile the production scheduler/ABI with virtual-clock and pin observers.
 *
 * SLICE contrib/ios-app/qemu-ios-ui.c range /* --- generic host input sequence | /* --- snapshots
 * CFLAGS -Wall -Werror -Wno-unused-function -Wno-comment -I$ROOT/contrib/ios-app
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "qemu-ios-ui.h"
#include "virtual-input.h"
#define QEMU_CLOCK_VIRTUAL 0
#define INPUT_EVENT_ABS_MAX 32767
#define INPUT_AXIS_X 0
#define INPUT_AXIS_Y 1
#define INPUT_BUTTON_LEFT 0
#define INPUT_EVENT_KIND_BTN 1
typedef struct {int button;bool down;} InputBtnEvent;
typedef struct {int type;union {struct {InputBtnEvent *data;} btn;} u;} InputEvent;
typedef struct {unsigned sequence;} QemuSeqLock;
static void seqlock_write_begin(QemuSeqLock*s) {s->sequence++;}
static void seqlock_write_end(QemuSeqLock*s) {s->sequence++;}
static unsigned seqlock_read_begin(QemuSeqLock*s) {return s->sequence&~1u;}
static bool seqlock_read_retry(QemuSeqLock*s,unsigned start) {return s->sequence!=start;}
#define qatomic_read(p) (*(p))
#define qatomic_set(p,v) (*(p)=(v))
#define g_new(T,n) ((T*)malloc(sizeof(T)*(n)))
#define g_new0(T,n) ((T*)calloc((n),sizeof(T)))
#define g_free free
typedef int IPodTouchButton;
typedef struct {void(*cb)(void*);void *arg;int64_t deadline;bool pending;} QEMUTimer;
static int64_t now;
static bool alive=true, pins[4], touch;
static int edges, syncs, last_x, last_y;
static bool ios_manual_touch;
static bool ios_manual_touch2;
static struct {bool down;} ios_button_holds[4];
static struct {void *con;} ios={(void*)1};
static void (*bh)(void*); static void *bh_arg;
static void *qemu_get_aio_context(void) {return NULL;}
static void aio_bh_schedule_oneshot(void *ctx,void(*cb)(void*),void *arg) {
 assert(!bh);bh=cb;bh_arg=arg;
}
static void flush(void) {void(*cb)(void*)=bh;void *arg=bh_arg;bh=NULL;cb(arg);}
static int64_t qemu_clock_get_ms(int clock) {return now;}
bool qemu_ios_ui_ready(void) {return alive;}
static QEMUTimer *timer_new_ms(int clock,void(*cb)(void*),void *arg) {
 QEMUTimer *t=calloc(1,sizeof(*t));t->cb=cb;t->arg=arg;return t;
}
static void timer_mod(QEMUTimer*t,int64_t d) {t->deadline=d;t->pending=true;}
static void timer_del(QEMUTimer*t) {t->pending=false;}
static void ios_press(IPodTouchButton b,bool down) {pins[b]=down;edges++;}
static void qemu_input_queue_abs(void*c,int axis,int value,int lo,int hi) {
 if(axis==0)last_x=value;else last_y=value;
}
static void qemu_input_queue_btn(void*c,int b,bool down) {touch=down;edges++;}
static void qemu_input_event_sync(void) {syncs++;}
static void qemu_input_event_send_impl(void*c,InputEvent *e) {touch=e->u.btn.data->down;edges++;}
static void qemu_input_event_sync_impl(void) {syncs++;}
#include "slice.h"
static void advance(int64_t value) {
 now=value;
 if(ios_sequence_timer->pending && now>=ios_sequence_timer->deadline) {
  ios_sequence_timer->pending=false;
  ios_sequence_timer->cb(ios_sequence_timer->arg);
 }
}
static bool submit(uint64_t id,const IosInputEvent *e,size_t n) {
 int64_t at[256];int32_t kind[256],val[256],phase[256];double x[256],y[256];
 for(size_t i=0;i<n;i++) {
  at[i]=e[i].at_ms;kind[i]=e[i].kind;val[i]=e[i].value;
  phase[i]=e[i].phase;x[i]=e[i].x;y[i]=e[i].y;
 }
 bool result=qemu_ios_ui_input_sequence(id,n,at,kind,val,phase,x,y);
 memset(at,0,sizeof(at)); /* request owns a copy, not caller arrays */
 return result;
}
int main(void) {
 IosInputEvent e[]={
  {150,0,0,1,0,0},{300,0,0,0,0,0},
  {500,0,1,1,0,0},{4000,0,1,0,0,0},
  {5500,1,0,0,.2,.1},{5580,1,0,1,.8,.1},{5660,1,0,2,.9,.1}};
 assert(submit(11,e,7));assert(qemu_ios_ui_input_sequence_status(11)==0);
 flush();assert(qemu_ios_ui_input_sequence_status(11)==1);
 /* Wall time does not move this clock; a paused VM cannot acquire signals. */
 assert(!pins[0] && ios_sequence_timer->deadline==150);
 advance(149);assert(!pins[0]);advance(150);assert(pins[0]);
 advance(299);assert(pins[0]);advance(300);assert(!pins[0]);
 advance(500);assert(pins[1]);advance(4000);assert(!pins[1]);
 advance(5500);assert(touch);advance(5580);assert(last_x==(int)(.8*32767));
 advance(5660);assert(!touch && syncs==3 && edges==7);
 assert(qemu_ios_ui_input_sequence_status(11)==2);
 /* Own cancellation while paused; wrong owner does not cancel. */
 now=6000;assert(submit(12,e,7));flush();advance(6150);assert(pins[0]);
 qemu_ios_ui_input_sequence_cancel(11);flush();assert(pins[0]);
 qemu_ios_ui_input_sequence_cancel(12);flush();assert(!pins[0]);
 assert(!ios_sequence_timer->pending && qemu_ios_ui_input_sequence_status(12)==3);
 now=7000;assert(submit(13,e,7));flush();advance(12500);assert(touch);
 ios_sequence_cancel_current();assert(!touch && !pins[1]);
 /* Replacement releases only owned buttons; unrelated manual volume stays. */
 now=13000;assert(submit(14,e,7));flush();advance(13150);assert(pins[0]);
 pins[2]=true;ios_button_holds[2].down=true;
 assert(submit(15,e,7));flush();assert(!pins[0] && pins[2]);
 assert(qemu_ios_ui_input_sequence_status(15)==4);
 ios_button_holds[2].down=false;pins[2]=false;
 ios_manual_touch=true;assert(submit(16,e,7));flush();
 assert(qemu_ios_ui_input_sequence_status(16)==4);ios_manual_touch=false;
 qemu_ios_ui_manual_touch2(true);assert(submit(17,e,7));flush();
 assert(qemu_ios_ui_input_sequence_status(17)==4);qemu_ios_ui_manual_touch2(false);
 /* Validate ordering/bounds, no unmatched edges, NaN/unknown kind. */
 IosInputEvent bad[7];memcpy(bad,e,sizeof(e));bad[1].at_ms=-1;
 assert(!submit(20,bad,7));memcpy(bad,e,sizeof(e));bad[6].phase=1;
 assert(!submit(20,bad,7));memcpy(bad,e,sizeof(e));bad[5].x=NAN;
 assert(!submit(20,bad,7));memcpy(bad,e,sizeof(e));bad[0].kind=9;
 assert(!submit(20,bad,7));memcpy(bad,e,sizeof(e));bad[0].value=4;
 assert(!submit(20,bad,7));memcpy(bad,e,sizeof(e));bad[6].at_ms=600001;
 assert(!submit(20,bad,7));assert(!submit(0,e,7));
 assert(!qemu_ios_ui_input_sequence(1,257,NULL,NULL,NULL,NULL,NULL,NULL));
 alive=false;assert(!submit(21,e,7));
 free(ios_sequence_timer);
 puts("PASS: production virtual-input admission, deadlines, ordering, copied arrays, paused cancellation, manual ownership, replacement, malformed events");
}
