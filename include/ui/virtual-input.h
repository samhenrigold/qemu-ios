/* Generic ordered input in guest time; no firmware or UI knowledge. */
#ifndef QEMU_IOS_VIRTUAL_INPUT_H
#define QEMU_IOS_VIRTUAL_INPUT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#define IOS_INPUT_MAX_EVENTS 256
#define IOS_INPUT_MAX_KEYS 512
/* kind: 0 hardware button (value=0..3, phase=0 up/1 down),
 *       1 absolute single touch (value=0, phase=0 begin/1 move/2 end),
 *       2 QEMU keycode (value=0..511, phase=0 up/1 down). */
typedef struct {
    int64_t at_ms;
    int32_t kind, value, phase;
    double x, y;
} IosInputEvent;
typedef struct {
    IosInputEvent events[IOS_INPUT_MAX_EVENTS];
    size_t count, next;
    int64_t origin;
    bool buttons[4], touch;
    bool keys[IOS_INPUT_MAX_KEYS];
    int key_order[IOS_INPUT_MAX_EVENTS];
    size_t key_count;
    double x, y;
} IosInputSequence;
typedef void (*IosInputEmit)(void *, const IosInputEvent *);
static inline bool ios_input_valid(const IosInputEvent *events, size_t count)
{
    bool buttons[4] = {0}, touch = false;
    bool keys[IOS_INPUT_MAX_KEYS] = {0};
    int64_t previous = 0;
    if (!events || !count || count > IOS_INPUT_MAX_EVENTS) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        const IosInputEvent *e = &events[i];
        if (e->at_ms < previous || e->at_ms > 600000) {
            return false;
        }
        previous = e->at_ms;
        if (e->kind == 0) {
            if (e->value < 0 || e->value > 3 || e->phase < 0 ||
                e->phase > 1 || buttons[e->value] == !!e->phase) {
                return false;
            }
            buttons[e->value] = !!e->phase;
        } else if (e->kind == 1) {
            if (e->value != 0 || e->phase < 0 || e->phase > 2 ||
                !isfinite(e->x) || !isfinite(e->y) ||
                e->x < 0 || e->x > 1 || e->y < 0 || e->y > 1 ||
                (e->phase == 0 ? touch : !touch)) {
                return false;
            }
            touch = e->phase != 2;
        } else if (e->kind == 2) {
            if (e->value < 0 || e->value >= IOS_INPUT_MAX_KEYS ||
                e->phase < 0 || e->phase > 1 || keys[e->value] == !!e->phase) {
                return false;
            }
            keys[e->value] = !!e->phase;
        } else {
            return false;
        }
    }
    for (int k = 0; k < IOS_INPUT_MAX_KEYS; ++k) {
        if (keys[k]) { return false; }
    }
    return !touch && !buttons[0] && !buttons[1] && !buttons[2] && !buttons[3];
}
/* Called only by the QEMU thread. Cancellation releases only signals this
 * sequence pressed, even when guest virtual time is paused. */
static inline void ios_input_cancel(IosInputSequence *s, IosInputEmit emit,
                                    void *opaque)
{
    for (size_t i = s->key_count; i > 0; --i) {
        int key = s->key_order[i - 1];
        if (s->keys[key]) {
            IosInputEvent e = {.kind = 2, .value = key, .phase = 0};
            emit(opaque, &e);
            s->keys[key] = false;
        }
    }
    s->key_count = 0;
    for (int b = 0; b < 4; ++b) {
        if (s->buttons[b]) {
            IosInputEvent e = {.kind = 0, .value = b, .phase = 0};
            emit(opaque, &e);
            s->buttons[b] = false;
        }
    }
    if (s->touch) {
        IosInputEvent e = {.kind = 1, .phase = 2, .x = s->x, .y = s->y};
        emit(opaque, &e);
        s->touch = false;
    }
    s->count = s->next = 0;
}
static inline bool ios_input_step(IosInputSequence *s, int64_t now,
                                  IosInputEmit emit, void *opaque)
{
    while (s->next < s->count &&
           now - s->origin >= s->events[s->next].at_ms) {
        const IosInputEvent *e = &s->events[s->next++];
        emit(opaque, e);
        if (e->kind == 0) {
            s->buttons[e->value] = !!e->phase;
        } else if (e->kind == 2) {
            if (e->phase) {
                s->key_order[s->key_count++] = e->value;
            }
            s->keys[e->value] = !!e->phase;
        } else {
            s->touch = e->phase != 2;
            s->x = e->x;
            s->y = e->y;
        }
    }
    return s->next == s->count;
}
#endif
