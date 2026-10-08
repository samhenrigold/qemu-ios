/*
 * The iPhone's vibration motor, as the host sees it: on or off, and how many
 * times it has started since the machine was created. Whatever drives the
 * motor on a board (an S5L8920X PWM channel on the 3GS and iPhone 4) sets it;
 * the app reads it through qemu_ios_ui_vibrator().
 */
#ifndef HW_MISC_IOS_VIBRATOR_H
#define HW_MISC_IOS_VIBRATOR_H

#include "hw/core/irq.h"

/* Any thread. pulses (may be NULL) counts every off-to-on edge, so a pulse
 * shorter than the reader's poll still shows. */
bool ios_vibrator_get(uint64_t *pulses);

/* A new drive for the board to wire to the motor: it runs while any drive is high. The first call
 * also gives the machine a read-only "vibrator" property (QMP's view of the same state). */
qemu_irq ios_vibrator_line(Object *machine);

#endif
