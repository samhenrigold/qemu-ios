/* Use the actual paused-runstate input functions, not a permissive pointer mock.
 *
 * SLICE:input ui/input.c fn qemu_input_event_send qemu_input_event_sync qemu_input_queue_abs qemu_input_queue_btn
 * SLICE:input contrib/ios-app/qemu-ios-ui.c fn ios_touch_bh
 * SLICE:mtt contrib/macos-app/qemu-macos-extras.c fn mtt_bh
 * CFLAGS -Wall -Werror -Wno-unused-function -Wno-comment
 */
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#define INPUT_EVENT_ABS_MAX 32767
#define INPUT_EVENT_ABS_MIN 0
#define INPUT_AXIS_X 0
#define INPUT_AXIS_Y 1
#define INPUT_BUTTON_LEFT 0
#define INPUT_EVENT_KIND_KEY 0
#define INPUT_EVENT_KIND_BTN 1
#define INPUT_EVENT_KIND_ABS 2
#define INPUT_EVENT_KIND_MTT 3
#define INPUT_MULTI_TOUCH_TYPE_BEGIN 0
#define INPUT_MULTI_TOUCH_TYPE_UPDATE 1
#define INPUT_MULTI_TOUCH_TYPE_END 2
#define QEMU_IOS_TOUCH_END 2
#define KEY_VALUE_KIND_NUMBER 0
#define KEY_VALUE_KIND_QCODE 1
#define Q_KEY_CODE_SYSRQ 99
#define Q_KEY_CODE_PRINT 98
#define RUN_STATE_SUSPENDED 1
#define g_free free
typedef int QemuConsole,InputButton,InputAxis;
typedef struct {int type;union {struct {int data;} qcode;} u;} KeyValue;
typedef struct {KeyValue*key;bool down;} InputKeyEvent;
typedef struct {int button;bool down;} InputBtnEvent;
typedef struct {int axis,value;} InputMoveEvent;
typedef int InputMultiTouchType;
typedef struct {int type,slot,tracking_id;} InputMultiTouchEvent;
typedef struct {int type;union {struct {InputMultiTouchEvent*data;} mtt;struct {InputBtnEvent*data;} btn;struct {InputMoveEvent*data;} abs;struct {InputKeyEvent*data;} key;} u;} InputEvent;
static bool paused,virtual_sequence_emitting,ios_manual_touch,touch;
static int x,y,releases,mtt_releases;
static bool second_touch,second_owned;
struct mtt_touch {int phase;double nx,ny;};

static struct {QemuConsole*con;} ios={(void*)1};
struct ios_touch {double nx,ny;bool down;};
static QemuConsole* con0(void) {return ios.con;}
static void qemu_ios_ui_manual_touch2(bool down) {second_owned=down;}
static bool runstate_is_running(void) {return !paused;}
static bool runstate_check(int s) {return false;}
static void ios_sequence_cancel_current(void) {}
static void qemu_input_sequence_cancel_current(void) {}
static int qemu_input_scale_axis(int v,int min,int max,int omin,int omax) {return v;}
static void qemu_input_event_send_impl(QemuConsole*c,InputEvent*e) {
 if(e->type==INPUT_EVENT_KIND_MTT){second_touch=e->u.mtt.data->type!=INPUT_MULTI_TOUCH_TYPE_END;if(!second_touch)mtt_releases++;}
 else if(e->type==INPUT_EVENT_KIND_BTN){touch=e->u.btn.data->down;if(!touch)releases++;}
 else if(e->type==INPUT_EVENT_KIND_ABS){if(e->u.abs.data->axis==0)x=e->u.abs.data->value;else y=e->u.abs.data->value;}
}
static void qemu_input_event_sync_impl(void) {}
static void replay_input_event(QemuConsole*c,InputEvent*e) {qemu_input_event_send_impl(c,e);}
static void replay_input_sync_event(void) {qemu_input_event_sync_impl();}
#include "input.h"
static void qemu_input_queue_mtt_abs(QemuConsole*c,int axis,int value,int min,int max,int slot,int tracking) {if(!paused){if(axis==0)x=value;else y=value;}}
static void qemu_input_queue_mtt(QemuConsole*c,int type,int slot,int tracking) {InputMultiTouchEvent m={type,slot,tracking};InputEvent e={.type=INPUT_EVENT_KIND_MTT,.u.mtt.data=&m};qemu_input_event_send(c,&e);}
#include "mtt.h"
static void event(bool down,double nx,double ny) {struct ios_touch*t=calloc(1,sizeof(*t));t->down=down;t->nx=nx;t->ny=ny;ios_touch_bh(t);}
static void event2(int phase,double nx,double ny) {struct mtt_touch*t=calloc(1,sizeof(*t));t->phase=phase;t->nx=nx;t->ny=ny;mtt_bh(t);}
int main(void) {
 event(true,.2,.3);assert(touch && ios_manual_touch);int px=x,py=y;
 paused=true;event(false,.8,.9);assert(!touch && !ios_manual_touch && releases==1);
 assert(x==px && y==py); /* No synthetic paused motion. */
 event(true,.8,.9);assert(!touch && !ios_manual_touch && x==px && y==py);
 paused=false;event(true,.7,.6);assert(touch && ios_manual_touch);
 event(false,.7,.6);assert(!touch && !ios_manual_touch && releases==2);
 event2(0,.2,.3);assert(second_touch && second_owned);px=x;py=y;
 paused=true;event2(1,.8,.9);assert(second_touch && second_owned && x==px && y==py);
 event2(2,.8,.9);assert(!second_touch && !second_owned && mtt_releases==1 && x==px && y==py);
 event2(0,.8,.9);assert(!second_touch && !second_owned && x==px && y==py);
 paused=false;event2(0,.5,.4);assert(second_touch && second_owned);
 event2(2,.5,.4);assert(!second_touch && !second_owned && mtt_releases==2);
 puts("PASS: actual paused-runstate handler releases only owned manual touch, skips paused down/motion, preserves normal input");
}
