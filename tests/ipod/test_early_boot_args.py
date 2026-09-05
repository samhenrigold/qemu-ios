#!/usr/bin/env python3
"""The 7E18 handoff edit is version checked and bounds its SRAM command line."""
from pathlib import Path
import subprocess,shlex,tempfile
s=(Path(__file__).resolve().parents[2]/'hw/arm/ipod_touch_2g.c').read_text()
a=s.index('static const char *ipod_touch_requested_boot_args(');helper=s[a:s.index('\n}',a)+2]
a=s.index('static void ipod_touch_inject_boot_args(');s=helper+'\n'+s[a:s.index('\n}',a)+2]
code=r'''
#include <glib.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t hwaddr;
#define IBOOT_MEM_BASE 0x0ff00000
#define BOOT_ARGS_CMDLINE_LEN 256
#define BOOT_ARGS_STAGING_BASE 0x220fff00
#define MEMTXATTRS_UNSPECIFIED 0
typedef struct {void*nsas;char boot_args[256];} IPodTouchMachineState;
static uint8_t image[0x27000],staging[256];
static unsigned writes;
static uint8_t*memory(hwaddr a,size_t n){
 if(a>=IBOOT_MEM_BASE&&a+n<=IBOOT_MEM_BASE+sizeof(image))return image+(a-IBOOT_MEM_BASE);
 assert(a==BOOT_ARGS_STAGING_BASE&&n==sizeof(staging));return staging;
}
static void address_space_read(void*as,hwaddr a,int at,void*out,size_t n){memcpy(out,memory(a,n),n);}
static void address_space_write(void*as,hwaddr a,int at,const void*in,size_t n){memcpy(memory(a,n),in,n);writes++;}
static uint32_t ldl_le_p(void*p){uint32_t x;memcpy(&x,p,4);return GUINT32_FROM_LE(x);}
static void stl_le_p(void*p,uint32_t x){x=GUINT32_TO_LE(x);memcpy(p,&x,4);}
'''+s+r'''
int main(void){
 IPodTouchMachineState machine={0};
 const uint8_t signature[]={0x2c,0x4b,0x9b,0x46,0x1b,0x68,0x00,0x2b,0x03,0xd1,0x2a,0x48,0x06,0x1c,0x01,0x90,0x02,0xe0,0x29,0x4e,0x28,0x49,0x01,0x91};
 memcpy(image+0x11a72,signature,sizeof(signature));stl_le_p(image+0x11b28,0x0ff1dba0);
 unsetenv("IT_BOOT_ARGS");ipod_touch_inject_boot_args(&machine);assert(!writes);
 setenv("IT_BOOT_ARGS","-v",1);ipod_touch_inject_boot_args(&machine);
 assert(writes==2&&!strcmp((char*)staging,"-v"));assert(ldl_le_p(image+0x11b28)==BOOT_ARGS_STAGING_BASE);
 stl_le_p(image+0x11b28,0x0ff1dba0);image[0x11a72]^=1;ipod_touch_inject_boot_args(&machine);assert(writes==2);
 image[0x11a72]^=1;char oversized[512];memset(oversized,'x',511);oversized[511]=0;setenv("IT_BOOT_ARGS",oversized,1);
 ipod_touch_inject_boot_args(&machine);assert(writes==4&&staging[255]==0&&strlen((char*)staging)==255);
 stl_le_p(image+0x11b28,0x0ff1dba0);
 strcpy(machine.boot_args,"serial=3 debug=0x8");
 ipod_touch_inject_boot_args(&machine);
 assert(writes==6&&!strcmp((char*)staging,machine.boot_args));
 assert(ipod_touch_requested_boot_args(&machine)==machine.boot_args);
 unsetenv("IT_BOOT_ARGS");stl_le_p(image+0x11b28,0x0ff1dba0);
 ipod_touch_inject_boot_args(&machine);assert(writes==8);
 machine.boot_args[0]=0;assert(!ipod_touch_requested_boot_args(&machine));
}
'''
flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True))
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'check.c';p.write_text(code);exe=Path(d)/'check'
 subprocess.run(['clang','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(p),'-o',str(exe),*flags],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: early iBoot arguments, unknown firmware rejection, disabled path, bounded string')
