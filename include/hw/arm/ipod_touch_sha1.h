#ifndef HW_ARM_IPOD_TOUCH_SHA1_H
#define HW_ARM_IPOD_TOUCH_SHA1_H

#include "qemu/osdep.h"
#include "hw/platform-bus.h"
#include "hw/hw.h"
#include "exec/hwaddr.h"
#include "exec/memory.h"
#include "hw/irq.h"
#include "hw/sysbus.h"

#define TYPE_IPOD_TOUCH_SHA1                "ipodtouch.sha1"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchSHA1State, IPOD_TOUCH_SHA1)

#define SHA_CONFIG         0x0
#define SHA_RESET          0x4
#define SHA_INTSTATUS      0x8  // completion status, write to acknowledge
#define SHA_INTENABLE      0xc  // 1 = raise the completion interrupt
#define SHA_HASHOUT        0x20 // 5 words of chaining state, big-endian
#define SHA_HASHOUT_END    0x30
#define SHA_HWBUF          0x40 // 16 words = one 512-bit message block
#define SHA_HWBUF_END      0x7c
#define SHA_MEMORY_MODE    0x80 // whether we read from the memory
#define SHA_MEMORY_START   0x84
#define SHA_INSIZE         0x8c

typedef struct IPodTouchSHA1State {
	SysBusDevice busdev;
    MemoryRegion iomem;
    uint32_t config;
    uint32_t memory_start;
    uint32_t memory_mode;
    uint32_t insize;
    /*
     * The engine is a raw SHA1 block compressor: it chains from the state the
     * guest loads into the hash registers and leaves the resulting state
     * there. It never pads and never finalises - the guest's software does
     * both (see XNU's SHA1Final, and iBoot, which pre-pads its messages).
     */
    uint32_t state[5];
    uint32_t hw_buffer[0x10]; // hardware buffer
    bool hw_buffer_dirty;
    /*
     * 3.1.3's AppleS5L8900XSHA1 switches from polling to interrupt-driven
     * partway through boot: it sets SHA_INTENABLE and then starts jobs with
     * the interrupt bit in the config word, sleeping on its command gate
     * until the completion interrupt arrives.
     */
    uint32_t int_enable;
    uint32_t int_status;
    qemu_irq irq;
} IPodTouchSHA1State;

/* Most recent chaining state produced by the SHA1 engine, big-endian. For a
 * message the guest pre-padded this is the finished digest. False if the
 * engine has not run. */
bool ipod_touch_sha1_last_hash(uint8_t out[20]);

#endif
