/* SHA completion interrupts follow config bit 2, not SHA_INTENABLE alone.
 *
 * SLICE include/hw/arm/ipod_touch_sha1.h typedef IPodTouchSHA1State
 * SLICE include/hw/arm/sha1_compress.h file
 * SLICE hw/arm/ipod_touch_sha1.c fn sha1_trace sha1_publish sha1_clear_irq sha1_run
 */
#include <assert.h>
#define trace_event_get_state_backends(id) 0
#define TRACE_PRINTF(fn, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int SysBusDevice, MemoryRegion, qemu_irq;
static int line;
static void qemu_irq_raise(qemu_irq irq) {line=1;}
static void qemu_irq_lower(qemu_irq irq) {line=0;}
static void cpu_physical_memory_read(uint64_t a,void *p,size_t n) {memset(p,0,n);}
#define IT_SHA1_DMA_CHUNK (64 * 1024)
#define MIN(a,b) ((a)<(b)?(a):(b))
typedef uint64_t hwaddr;
#define g_malloc malloc
#define g_free free
#include "slice.h"
int main(void) {
 IPodTouchSHA1State s={0};
 s.irq=1;s.int_enable=1;
 /* A polled start (0x2/0xa) left over from an interrupt-driven job. */
 sha1_run(&s,false);assert(!s.int_status && !line);
 /* An interrupt-driven start (0x6/0xe). */
 sha1_run(&s,true);assert(s.int_status && line);
 sha1_clear_irq(&s);assert(!s.int_status && !line);
 s.int_enable=0;sha1_run(&s,true);assert(!s.int_status && !line);
 puts("PASS: SHA completion interrupt only for bit-2 starts with SHA_INTENABLE set");
}
