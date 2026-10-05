/*
 * The device's four hardware buttons, for host UIs that have no keyboard to
 * hide them behind.
 *
 * Deliberately free of any include: ipod_touch_2g.h pulls in cpu.h and the
 * qdev headers, which target-independent code (the iOS app bridge) cannot
 * include. Keeping the contract here lets both sides agree on it without
 * either mirroring constants the other might change.
 */

#ifndef HW_ARM_IPOD_TOUCH_BUTTONS_H
#define HW_ARM_IPOD_TOUCH_BUTTONS_H

#include <stdbool.h>

typedef enum {
    IPOD_TOUCH_BUTTON_HOME,
    IPOD_TOUCH_BUTTON_POWER,
    IPOD_TOUCH_BUTTON_VOLUP,
    IPOD_TOUCH_BUTTON_VOLDOWN,
} IPodTouchButton;

/*
 * Press or release a button. Home and power are the two that matter: a device
 * that has gone to sleep cannot be woken any other way.
 */
void ipod_touch_press_button(IPodTouchButton button, bool down);
/* The iPad 1's buttons (GPIO pins), same contract; no-op on other machines. */
void ipad1_press_button(IPodTouchButton button, bool down);
/* The iPod touch 1G's (GPIO pads 0x16), home and power only; no-op on other machines. */
void ipod_touch_1g_press_button(IPodTouchButton button, bool down);
/* The S5L8920 boards' (-M n18, n88) GPIO buttons; no-op on other machines. */
void s5l8920_press_button(IPodTouchButton button, bool down);

#endif /* HW_ARM_IPOD_TOUCH_BUTTONS_H */
