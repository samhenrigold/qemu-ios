/* The A6 input adapter must receive both axes before a contact starts, keep
 * independent contact slots and lift the same contact at an exact panel edge.
 * SLICE hw/arm/ipod_touch_multitouch.c fn mt_host_event
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#define MT_MAX_FINGERS 5
#define INPUT_EVENT_ABS_MAX 32767
#define INPUT_AXIS_X 0
#define INPUT_AXIS_Y 1
enum {
    INPUT_MULTI_TOUCH_TYPE_DATA,
    INPUT_MULTI_TOUCH_TYPE_BEGIN,
    INPUT_MULTI_TOUCH_TYPE_UPDATE,
    INPUT_MULTI_TOUCH_TYPE_END,
    INPUT_MULTI_TOUCH_TYPE_CANCEL
};
typedef struct {
    int type, slot, axis, value;
} InputMultiTouchEvent;
typedef struct {
    struct {
        struct {
            InputMultiTouchEvent *data;
        } mtt;
    } u;
} InputEvent;
typedef struct {
    int host_mtt[5][2];
    uint8_t host_mtt_axes[5];
} IPodTouchMultitouchState;
typedef IPodTouchMultitouchState DeviceState;
typedef void QemuConsole;
#define IPOD_TOUCH_MULTITOUCH(d) (d)
static int calls, got_slot;
static float got_x, got_y;
static bool got_down;
static void ipod_touch_multitouch_set_finger(IPodTouchMultitouchState *s,
                                             int slot, float x, float y,
                                             bool down)
{
    calls++;
    got_slot = slot;
    got_x = x;
    got_y = y;
    got_down = down;
}
#include "slice.h"
static void send(DeviceState *s, int type, int slot, int axis, int value)
{
    InputMultiTouchEvent m = {type, slot, axis, value};
    InputEvent e = {.u.mtt.data = &m};
    mt_host_event(s, NULL, &e);
}
int main(void)
{
    DeviceState s = {0};
    send(&s, INPUT_MULTI_TOUCH_TYPE_BEGIN, 0, 0, 0);
    assert(!calls);
    send(&s, INPUT_MULTI_TOUCH_TYPE_DATA, 0, INPUT_AXIS_X, 0);
    send(&s, INPUT_MULTI_TOUCH_TYPE_BEGIN, 0, 0, 0);
    assert(!calls);
    send(&s, INPUT_MULTI_TOUCH_TYPE_DATA, 0, INPUT_AXIS_Y, 32767);
    send(&s, INPUT_MULTI_TOUCH_TYPE_BEGIN, 0, 0, 0);
    assert(calls == 1 && got_down && got_x == 0 && got_y == 0);
    send(&s, INPUT_MULTI_TOUCH_TYPE_DATA, 1, INPUT_AXIS_X, 32767);
    send(&s, INPUT_MULTI_TOUCH_TYPE_DATA, 1, INPUT_AXIS_Y, 0);
    send(&s, INPUT_MULTI_TOUCH_TYPE_BEGIN, 1, 0, 0);
    assert(calls == 2 && got_slot == 1 && got_down && got_x == 1 && got_y == 1);
    send(&s, INPUT_MULTI_TOUCH_TYPE_CANCEL, 0, 0, 0);
    assert(calls == 3 && got_slot == 0 && !got_down && got_x == 0 &&
           got_y == 0);
    send(&s, INPUT_MULTI_TOUCH_TYPE_UPDATE, 0, 0, 0);
    assert(calls == 3);
    send(&s, INPUT_MULTI_TOUCH_TYPE_END, 1, 0, 0);
    assert(calls == 4 && !got_down && got_slot == 1);
    send(&s, INPUT_MULTI_TOUCH_TYPE_DATA, -1, 0, 1);
    send(&s, INPUT_MULTI_TOUCH_TYPE_BEGIN, 5, 0, 0);
    assert(calls == 4);
    puts(
        "PASS: A6 independent contacts, both axes before BEGIN, edge endpoints and cancellation");
    return 0;
}
