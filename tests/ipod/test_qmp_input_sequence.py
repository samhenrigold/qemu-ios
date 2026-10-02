#!/usr/bin/env python3
"""Production generic QMP sequence conversion, ownership and modifier ordering."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'ui/input.c').read_text()
s=s[s.index('/* Generic host automation:'):s.index('static void qemu_input_event_trace(')]
header=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "ui/virtual-input.h"
#define g_new0(T,n) ((T*)calloc((n),sizeof(T)))
#define OBJECT(p) (p)
#define QEMU_CLOCK_VIRTUAL 0
#define INPUT_EVENT_ABS_MAX 32767
#define INPUT_BUTTON_LEFT 0
#define INPUT_EVENT_MASK_KEY 1
#define INPUT_EVENT_MASK_ABS 2
#define INPUT_EVENT_MASK_BTN 4
#define INPUT_SEQUENCE_STATUS_UNKNOWN 0
#define INPUT_SEQUENCE_STATUS_RUNNING 1
#define INPUT_SEQUENCE_STATUS_COMPLETED 2
#define INPUT_SEQUENCE_STATUS_CANCELLED 3
#define INPUT_SEQUENCE_EVENT_KIND_KEY 0
#define INPUT_SEQUENCE_EVENT_KIND_TOUCH 1
#define KEY_VALUE_KIND_QCODE 0
#define INPUT_EVENT_KIND_KEY 0
#define INPUT_EVENT_KIND_ABS 1
#define INPUT_EVENT_KIND_BTN 2
#define INPUT_EVENT_KIND_MTT 3
#define INPUT_EVENT_SLOTS_MAX 8
#define INPUT_MULTI_TOUCH_TYPE_BEGIN 0
#define INPUT_MULTI_TOUCH_TYPE_END 2
#define INPUT_MULTI_TOUCH_TYPE_CANCEL 3
typedef struct {int type,slot;} InputMultiTouchEvent;
typedef int QKeyCode,InputSequenceStatus,QemuConsole;
typedef struct {int status;} InputSequenceInfo;
typedef struct {int type;union {struct {int data;} qcode;} u;} KeyValue;
typedef struct {KeyValue *key;bool down;} InputKeyEvent;
typedef struct {int axis,value;} InputMoveEvent;
typedef struct {int button;bool down;} InputBtnEvent;
typedef struct {int type;union {struct {InputKeyEvent *data;} key;struct {InputMoveEvent *data;} abs;struct {InputBtnEvent *data;} btn;struct {InputMultiTouchEvent *data;} mtt;} u;} InputEvent;
typedef struct {int type;int64_t at_ms;union {struct {int key;bool down;} key;struct {int phase;double x,y;} touch;} u;} InputSequenceEvent;
typedef struct InputSequenceEventList {InputSequenceEvent *value;struct InputSequenceEventList *next;} InputSequenceEventList;
typedef struct {int set;} Error;
typedef struct {void(*cb)(void*);void*arg;int64_t deadline;bool pending;} QEMUTimer;
static int64_t now;
static int console,refs,mask=7,logkeys[256],logcount;
static bool keys[512],touch,paused;
static bool virtual_sequence_emitting;
static bool virtual_manual_keys[IOS_INPUT_MAX_KEYS],virtual_manual_touch;
static bool virtual_manual_mtt[INPUT_EVENT_SLOTS_MAX];
static void qemu_input_sequence_cancel_current(void);
static QemuConsole *qemu_console_lookup_by_device_name(const char*d,int64_t h,Error**e) {return &console;}
static QemuConsole *qemu_console_lookup_by_index(int i) {return &console;}
static void *qemu_input_find_handler(int m,QemuConsole*c) {return (mask&m) ? c : NULL;}
static void error_setg(Error**out,const char*format,...) {static Error err;*out=&err;}
static void object_ref(void*o) {assert(o==&console);refs++;}
static void object_unref(void*o) {assert(o==&console && refs==1);refs--;}
static int64_t qemu_clock_get_ms(int c) {return now;}
static QEMUTimer *timer_new_ms(int c,void(*cb)(void*),void*arg) {QEMUTimer*t=calloc(1,sizeof(*t));t->cb=cb;t->arg=arg;return t;}
static void timer_mod(QEMUTimer*t,int64_t d) {t->deadline=d;t->pending=true;}
static void timer_del(QEMUTimer*t) {t->pending=false;}
static void qemu_register_reset(void(*cb)(void*),void*o) {}
static void qemu_input_event_send_impl(QemuConsole*c,InputEvent*e) {
 if(e->type==INPUT_EVENT_KIND_KEY) {
  int k=e->u.key.data->key->u.qcode.data;bool down=e->u.key.data->down;
  keys[k]=down;logkeys[logcount++]=down?k:-k;
 } else if(e->type==INPUT_EVENT_KIND_BTN) {touch=e->u.btn.data->down;}
}
static void qemu_input_event_send(QemuConsole*c,InputEvent*e) {if(!paused)qemu_input_event_send_impl(c,e);}
static void qemu_input_event_sync_impl(void) {}
static void qemu_input_event_sync(void) {}
'''
checks=r'''
static int status(uint64_t id) {InputSequenceInfo*i=qmp_query_input_sequence(id,NULL);int s=i->status;free(i);return s;}
static void advance(int64_t t) {now=t;if(!paused && virtual_sequence_timer->pending && now>=virtual_sequence_timer->deadline) {virtual_sequence_timer->pending=false;virtual_sequence_timer->cb(NULL);}}
int main(void) {
 InputSequenceEvent e[7]={
  {.type=0,.at_ms=100,.u.key={10,true}},
  {.type=0,.at_ms=100,.u.key={11,true}},
  {.type=0,.at_ms=250,.u.key={11,false}},
  {.type=0,.at_ms=250,.u.key={10,false}},
  {.type=1,.at_ms=300,.u.touch={0,.2,.1}},
  {.type=1,.at_ms=380,.u.touch={1,.8,.1}},
  {.type=1,.at_ms=460,.u.touch={2,.9,.1}}};
 InputSequenceEventList nodes[7];for(int i=0;i<7;i++){nodes[i].value=&e[i];nodes[i].next=i==6?NULL:&nodes[i+1];}
 Error *err=NULL;
 qmp_input_send_sequence(1,NULL,false,0,nodes,&err);assert(!err && refs==1 && status(1)==1);
 advance(99);assert(!keys[10]);advance(100);assert(keys[10] && keys[11]);
 assert(logkeys[0]==10 && logkeys[1]==11);
 paused=true;qmp_input_cancel_sequence(8,NULL);assert(keys[10]);
 qmp_input_cancel_sequence(1,NULL);assert(!keys[10] && !keys[11] && refs==0);
 assert(logkeys[2]==-11 && logkeys[3]==-10 && status(1)==3);
 paused=false;now=500;qmp_input_send_sequence(2,NULL,false,0,nodes,&err);
 advance(800);assert(touch);paused=true;qmp_input_cancel_sequence(2,NULL);assert(!touch && refs==0);
 paused=false;now=1000;qmp_input_send_sequence(3,NULL,false,0,nodes,&err);
 advance(1460);assert(status(3)==2 && refs==0 && !keys[10] && !touch);
 assert(status(2)==0 && status(0)==0);
 now=2000;qmp_input_send_sequence(4,NULL,false,0,nodes,&err);advance(2100);
 mask=2;qmp_input_send_sequence(5,NULL,false,0,nodes,&err);
 assert(err && keys[10] && status(4)==1);err=NULL;mask=7;
 e[1].at_ms=-1;qmp_input_send_sequence(5,NULL,false,0,nodes,&err);
 assert(err && keys[10] && status(4)==1);err=NULL;e[1].at_ms=100;
 e[6].u.touch.phase=1;qmp_input_send_sequence(5,NULL,false,0,nodes,&err);
 assert(err && status(4)==1);err=NULL;e[6].u.touch.phase=2;
 // A validated replacement releases old modifier ownership before admission.
 qmp_input_send_sequence(6,NULL,false,0,nodes,&err);assert(!err && !keys[10] && refs==1 && status(6)==1);
 advance(2200);assert(keys[10]);qemu_input_sequence_reset(NULL);
 assert(!keys[10] && status(6)==3 && refs==0);
 virtual_manual_keys[9]=true;qmp_input_send_sequence(7,NULL,false,0,nodes,&err);
 assert(err && status(6)==3 && refs==0);err=NULL;virtual_manual_keys[9]=false;
 virtual_manual_mtt[1]=true;qmp_input_send_sequence(7,NULL,false,0,nodes,&err);
 assert(err && refs==0);virtual_manual_mtt[1]=false;
 free(virtual_sequence_timer);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'check.c';p.write_text(header+s+checks)
 subprocess.run(['clang','-Wall','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-I',str(root/'include'),str(p),'-o',d+'/check'],check=True)
 subprocess.run([d+'/check'],check=True)
print('PASS: actual QMP admission/conversion, guest-time deadlines, reverse modifier cancellation while paused, touch release, reference ownership, malformed/unsupported replacement refusal and reset')
