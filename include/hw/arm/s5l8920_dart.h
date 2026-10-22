#ifndef HW_ARM_S5L8920_DART_H
#define HW_ARM_S5L8920_DART_H

#include "exec/hwaddr.h"

/* IOVA -> PA through a DART (its device as opaque); va itself outside the window, -1 for an invalid PTE. */
hwaddr s5l8920_dart_xlate(void *opaque, uint32_t va, unsigned sid);

#endif
