#ifndef IPOD_TOUCH_PIEZO_H
#define IPOD_TOUCH_PIEZO_H

#include "hw/arm/ipod_touch_timer.h"

#define TYPE_IPOD_TOUCH_PIEZO "ipodtouch.piezo"

/* IPodTouchTimerState.output_hook for the timer channel the piezo is on. */
void ipod_touch_piezo_timer_output(void *opaque, unsigned ch, const IPodTouchTimerOutput *out);

#endif
