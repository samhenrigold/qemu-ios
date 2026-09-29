#!/usr/bin/env python3
"""The early handoff redirects the iBoot literal hw/arm/it_iboot.c finds, bounds its SRAM command line, and takes
the boot-args machine property as its only input (no environment)."""
from pathlib import Path
import subprocess,shlex,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'hw/arm/ipod_touch_2g.c').read_text()
assert 'getenv("IT_BOOT_ARGS' not in s, 'the machine must not read IT_BOOT_ARGS* from the environment'
a=s.index('static const char *ipod_touch_requested_boot_args(');helper=s[a:s.index('\n}',a)+2]
a=s.index('static void ipod_touch_set_boot_args(');setter=s[a:s.index('\n}',a)+2]
a=s.index('static void ipod_touch_stage_boot_args(');stage=s[a:s.index('\n}',a)+2]
a=s.index('static void ipod_touch_inject_boot_args(');s=stage+'\n'+setter+'\n'+helper+'\n'+s[a:s.index('\n}',a)+2]
code=r'''
#include "qemu/osdep.h"
#include "hw/arm/it_iboot.h"
#include <assert.h>
#include <stdio.h>
#define IBOOT_MEM_BASE 0x0ff00000
#define BOOT_ARGS_CMDLINE_LEN 256
#define BOOT_ARGS_STAGING_BASE 0x220fff00
typedef struct {AddressSpace*nsas;void*cpu;void*boot_args_timer;unsigned boot_args_writes;uint32_t boot_args_delay_ms;bool boot_args_scan_failed,boot_args_explicit;char boot_args[512];} IPodTouchMachineState;
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
static const char *ipod_touch_requested_boot_args(IPodTouchMachineState*);
static uint8_t image[0x27000],staging[256];
static unsigned writes;
static uint8_t*memory(hwaddr a,size_t n){
 if(a>=IBOOT_MEM_BASE&&a+n<=IBOOT_MEM_BASE+sizeof(image))return image+(a-IBOOT_MEM_BASE);
 assert(a==BOOT_ARGS_STAGING_BASE&&n==sizeof(staging));return staging;
}
MemTxResult address_space_read(AddressSpace*as,hwaddr a,MemTxAttrs at,void*out,hwaddr n){memcpy(out,memory(a,n),n);return 0;}
MemTxResult address_space_write(AddressSpace*as,hwaddr a,MemTxAttrs at,const void*in,hwaddr n){memcpy(memory(a,n),in,n);writes++;return 0;}
'''+s+r'''
int main(void){
 IPodTouchMachineState machine={.boot_args_delay_ms=2000};
 /* 7E18's layout: ldr r0,[pc,#0xa8] at 0x11a7c -> empty-string literal 0x11b28, restore literal right after it. */
 static const char restore[]="rd=md0 nand-enable-reformat=1 -progress";
 #define PLACE(ldr,e,str) do{ image[(ldr)]=(uint8_t)(((e)-(((ldr)+4)&~3))/4); image[(ldr)+1]=0x48; \
   stl_le_p(image+(e),IBOOT_MEM_BASE+0x1dba0); stl_le_p(image+(e)+4,IBOOT_MEM_BASE+(str)); \
   memcpy(image+(str),restore,sizeof(restore)); }while(0)
 PLACE(0x11a7c,0x11b28,0x1c000);
 /* No property: nothing is written, whatever the environment says. */
 setenv("IT_BOOT_ARGS","-v",1);
 assert(!ipod_touch_requested_boot_args(&machine));
 ipod_touch_inject_boot_args(&machine,sizeof(image));assert(!writes);
 strcpy(machine.boot_args,"-v");
 ipod_touch_inject_boot_args(&machine,sizeof(image));
 assert(writes==2&&!strcmp((char*)staging,"-v"));assert(ldl_le_p(image+0x11b28)==BOOT_ARGS_STAGING_BASE);
 /* No Thumb load of the word: not the handoff. */
 stl_le_p(image+0x11b28,IBOOT_MEM_BASE+0x1dba0);image[0x11a7c]^=1;ipod_touch_inject_boot_args(&machine,sizeof(image));assert(writes==2);
 image[0x11a7c]^=1;char oversized[512];memset(oversized,'x',511);oversized[511]=0;strcpy(machine.boot_args,oversized);
 ipod_touch_inject_boot_args(&machine,sizeof(image));assert(writes==4&&staging[255]==0&&strlen((char*)staging)==255);
 stl_le_p(image+0x11b28,IBOOT_MEM_BASE+0x1dba0);
 strcpy(machine.boot_args,"serial=3 debug=0x8");
 ipod_touch_inject_boot_args(&machine,sizeof(image));
 assert(writes==6&&!strcmp((char*)staging,machine.boot_args));
 assert(ipod_touch_requested_boot_args(&machine)==machine.boot_args);
 /* 8C148's layout elsewhere in the image: follows the literals, not an offset. */
 memset(image,0,sizeof(image));PLACE(0xa100,0xa190,0x1ee68);
 ipod_touch_inject_boot_args(&machine,sizeof(image));assert(writes==8);
 assert(ldl_le_p(image+0xa190)==BOOT_ARGS_STAGING_BASE);
 /* Two literals for the restore string are ambiguous; nothing may change. */
 stl_le_p(image+0xa190,IBOOT_MEM_BASE+0x1dba0);stl_le_p(image+0x2000,IBOOT_MEM_BASE+0x1ee68);
 ipod_touch_inject_boot_args(&machine,sizeof(image));assert(writes==8);
 /* The normal-boot word must point at an empty string inside the image. */
 stl_le_p(image+0x2000,0);stl_le_p(image+0xa190,IBOOT_MEM_BASE+sizeof(image));
 ipod_touch_inject_boot_args(&machine,sizeof(image));assert(writes==8);
 stl_le_p(image+0xa190,IBOOT_MEM_BASE+0x1ee68);
 ipod_touch_inject_boot_args(&machine,sizeof(image));assert(writes==8);
 ipod_touch_inject_boot_args(&machine,8);assert(writes==8);
 ipod_touch_inject_boot_args(&machine,0);assert(writes==8);
 machine.boot_args[0]=0;assert(!ipod_touch_requested_boot_args(&machine));
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
}
'''
flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True))
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'qemu').mkdir();(p/'exec').mkdir()
 (p/'qemu/osdep.h').write_text('''#pragma once
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static inline uint32_t ldl_le_p(const void *ptr){uint32_t x;memcpy(&x,ptr,4);return GUINT32_FROM_LE(x);}
static inline void stl_le_p(void *ptr,uint32_t x){x=GUINT32_TO_LE(x);memcpy(ptr,&x,4);}
''')
 (p/'exec/hwaddr.h').write_text('#pragma once\n#include <stdint.h>\ntypedef uint64_t hwaddr;\n')
 (p/'exec/memory.h').write_text('''#pragma once
#include "exec/hwaddr.h"
typedef struct AddressSpace AddressSpace;
typedef int MemTxAttrs, MemTxResult;
#define MEMTXATTRS_UNSPECIFIED 0
MemTxResult address_space_read(AddressSpace*,hwaddr,MemTxAttrs,void*,hwaddr);
MemTxResult address_space_write(AddressSpace*,hwaddr,MemTxAttrs,const void*,hwaddr);
''')
 (p/'check.c').write_text(code);exe=p/'check'
 subprocess.run(['clang','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(p),'-I'+str(root/'include'),
                 str(p/'check.c'),str(root/'hw/arm/it_iboot.c'),'-o',str(exe),*flags],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: early iBoot arguments from the machine property only, unknown firmware rejection, disabled path, bounded string, explicit empty override and immutable startup arguments')
