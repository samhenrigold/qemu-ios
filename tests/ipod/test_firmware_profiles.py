#!/usr/bin/env python3
"""Exact kernel profiles and production patch guards, with no guest required."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[2]
mbx=(root/'hw/arm/ipod_touch_mbx.c').read_text()
patch=mbx[mbx.index('static void patch_kernel('):mbx.index('static uint64_t ipod_touch_mbx2_read(')]
board=(root/'hw/arm/ipod_touch_2g.c').read_text()
amfi=board[board.index('static bool it_amfi_patch_one('):board.index('static const char *ipod_touch_requested_boot_args(')]
source=r'''
#include "qemu/osdep.h"
#include "hw/arm/ipod_touch_firmware.h"
#include <assert.h>
static uint8_t ram[IT_KERNEL_SCAN_LEN];
static unsigned reads, writes, usb_patches;
static uint64_t addresses[32];
void cpu_physical_memory_read(uint64_t address, void *out, uint64_t size){
 assert(address==IT_KERNEL_SCAN_PA_START && size==sizeof(ram));reads++;memcpy(out,ram,size);
}
static void cpu_physical_memory_write(uint64_t address,const void *data,uint64_t size){
 assert(writes<32);addresses[writes++]=address;
}
static uint32_t reverse_byte_order(uint32_t value){return __builtin_bswap32(value);}
static void patch_usb_function_gate(void){usb_patches++;}
#define MEMTXATTRS_UNSPECIFIED 0
static void address_space_rw(void *space,uint64_t address,int attrs,void *data,unsigned size,int write){
 assert(size==4);
 if(write)cpu_physical_memory_write(address,data,size);
 else memcpy(data,"\xf0\xb5\0\0",4); // A matching push alone is not a firmware ID.
}
typedef struct{void *nsas;bool amfi_patched;} IPodTouchMachineState;
'''+patch+amfi+r'''
static void load(const ITFirmwareDesc *fw){
 memset(ram,0,sizeof(ram));
 if(fw)memcpy(ram+32,fw->kernel_banner,strlen(fw->kernel_banner)+1);
 it_firmware_reset();writes=usb_patches=0;
}
int main(void){
 const ITFirmwareDesc *old=it_firmware_by_build("5F138"),*current=it_firmware_by_build("7E18");
 assert(old && current && !it_firmware_by_build("7E19") && !it_firmware_by_build(NULL));
 assert(old->iboot_boot_args_pa==0x0ff2a584 && !current->iboot_boot_args_pa);
 assert(!it_firmware_detect_kernel(NULL,100));
 size_t len=strlen(current->kernel_banner)+1;
 assert(it_firmware_detect_kernel((const uint8_t*)current->kernel_banner,len)==current);
 assert(!it_firmware_detect_kernel((const uint8_t*)current->kernel_banner,len-1));
 load(current);ram[32+22]='X';assert(!it_firmware_loaded());
 load(NULL);assert(!it_firmware_loaded());
 memcpy(ram+32,current->kernel_banner,len);assert(it_firmware_loaded()==current);
 unsigned count=reads;assert(it_firmware_loaded()==current && reads==count);
 memcpy(ram+512,old->kernel_banner,strlen(old->kernel_banner)+1);
 assert(!it_firmware_detect_kernel(ram,sizeof(ram)));
 load(current);bool patched=false;patch_kernel(&patched);assert(writes==0 && usb_patches==0);
 load(NULL);patched=false;patch_kernel(&patched);assert(writes==0 && usb_patches==0);
 load(old);patched=false;patch_kernel(&patched);assert(writes==4 && usb_patches==1 && addresses[0]==0x08324aa8);
 patch_kernel(&patched);assert(writes==4);
 for(unsigned i=0;i<writes;i++)assert(addresses[i]!=0x0816b460);
 setenv("IT_AMFI_ALLOW_TASKPORT","1",1);
 IPodTouchMachineState machine={0};
 load(NULL);ipod_touch_amfi_patch_now(&machine);assert(!machine.amfi_patched && writes==0);
 load(old);ipod_touch_amfi_patch_now(&machine);assert(!machine.amfi_patched && writes==0);
 load(current);ipod_touch_amfi_patch_now(&machine);
 assert(machine.amfi_patched && writes==2 && addresses[0]==0x081ab2a0 && addresses[1]==0x081ab200);
 load(NULL);machine.amfi_patched=false;
 setenv("IT_AMFI_HOOK_SLIDE","0xb8000000",1);ipod_touch_amfi_patch_now(&machine);assert(writes==0);
 setenv("IT_AMFI_GET_TASK_NAME_VA","0xc0100000",1);setenv("IT_AMFI_GET_TASK_VA","0xc0200000",1);
 ipod_touch_amfi_patch_now(&machine);assert(machine.amfi_patched && writes==2);
 puts("PASS: exact/ambiguous/truncated firmware, retry/cache/reset, MBX patch isolation and AMFI profile/override guards");
}
'''
with tempfile.TemporaryDirectory() as temp:
 p=Path(temp);(p/'qemu').mkdir();(p/'exec').mkdir()
 (p/'qemu/osdep.h').write_text('''#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define g_try_malloc malloc
#define g_free free
''')
 (p/'exec/cpu-common.h').write_text('#include <stdint.h>\nvoid cpu_physical_memory_read(uint64_t,void *,uint64_t);\n')
 (p/'check.c').write_text(source)
 subprocess.run(['clang','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(p),'-I'+str(root/'include'),str(p/'check.c'),str(root/'hw/arm/ipod_touch_firmware.c'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
