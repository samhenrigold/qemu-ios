/* Boot arguments use immutable, bounded startup properties and one resettable timer. Production-board qtests
 * cover handoff discovery and native gates cover factory identity plus early kernel console; no iBoot literal is
 * redirected.
 *
 * SLICE hw/arm/ipod_touch_2g.c define BOOT_ARGS_CMDLINE_LEN
 * SLICE hw/arm/ipod_touch_2g.c fn ipod_touch_requested_boot_args ipod_touch_set_boot_args ipod_touch_stage_boot_args
 * PKG glib-2.0
 */
#include <glib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {void*cpu;void*boot_args_timer;unsigned boot_args_writes;uint32_t boot_args_delay_ms,boot_args_repeat,boot_args_interval_ms;uint64_t boot_args_scan_deadline;bool boot_args_scan_failed,boot_args_explicit;char boot_args[512];} IPodTouchMachineState;
typedef IPodTouchMachineState Object;
typedef int Error;
#define IPOD_TOUCH_MACHINE(o) (o)
#define error_setg(errp,...) (**(errp)=1)
#define QEMU_CLOCK_VIRTUAL 0
static unsigned allocations, schedules;
static uint64_t expected_schedule=2123;
static int timer;
static void *timer_new_ms(int clock, void (*fn)(void*), void*opaque){allocations++;return &timer;}
static uint64_t qemu_clock_get_ms(int clock){return 123;}
static void timer_mod(void*t,uint64_t when){assert(t==&timer&&when==expected_schedule);schedules++;}
static void ipod_touch_set_boot_args_now(void*p){}
#include "slice.h"

int main(void){
 IPodTouchMachineState machine={.boot_args_delay_ms=2000,.boot_args_repeat=24,.boot_args_interval_ms=500};
 setenv("IT_BOOT_ARGS","-v",1);
 assert(!ipod_touch_requested_boot_args(&machine));
 char oversized[512];memset(oversized,'x',511);oversized[511]=0;
 int error=0;Error*ep=&error;
 ipod_touch_set_boot_args(&machine,"",&ep);
 assert(!error&&machine.boot_args_explicit&&!ipod_touch_requested_boot_args(&machine));
 char exact[256];memset(exact,'x',255);exact[255]=0;
 ipod_touch_set_boot_args(&machine,exact,&ep);assert(!error&&strlen(machine.boot_args)==255);
 ipod_touch_set_boot_args(&machine,oversized,&ep);assert(error&&strlen(machine.boot_args)==255);error=0;
 machine.cpu=&machine;ipod_touch_set_boot_args(&machine,"changed",&ep);
 assert(error&&strlen(machine.boot_args)==255);
 ipod_touch_stage_boot_args(&machine);ipod_touch_stage_boot_args(&machine);
 assert(allocations==1&&schedules==2);
 /* Resets use resolved startup settings, not later environment changes. */
 setenv("IT_BOOT_ARGS_DELAY_MS","999999",1);
 ipod_touch_stage_boot_args(&machine);assert(allocations==1&&schedules==3);
 machine.boot_args_delay_ms=1500;expected_schedule=1623;
 ipod_touch_stage_boot_args(&machine);assert(allocations==1&&schedules==4);
 puts("PASS: bounded startup arguments, explicit empty override and resettable timer from machine properties only");
}
