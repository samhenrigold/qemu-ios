#!/usr/bin/env python3
"""Kernel identification and bounded iBoot command-line discovery, with no guest required."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[2]
source=r'''
#include "qemu/osdep.h"
#include "hw/arm/ipod_touch_firmware.h"
#include <assert.h>
static uint8_t ram[IT_KERNEL_SCAN_LEN];
static unsigned reads;
void cpu_physical_memory_read(uint64_t address, void *out, uint64_t size){
 assert(address==IT_KERNEL_SCAN_PA_START && size==sizeof(ram));reads++;memcpy(out,ram,size);
}
static void load(const ITFirmwareDesc *fw){
 memset(ram,0,sizeof(ram));
 if(fw)memcpy(ram+32,fw->kernel_banner,strlen(fw->kernel_banner)+1);
 it_firmware_reset();
}

static void put16(uint8_t *p, uint16_t v){p[0]=v;p[1]=v>>8;}
static void put32(uint8_t *p, uint32_t v){put16(p,v);put16(p+2,v>>16);}
static void command_line_fixture(uint8_t *image, uint32_t base){
 memset(image,0,1024);
 put16(image,0x4c0f);put16(image+2,0x4810);put16(image+4,0x1c21);
 put16(image+6,0xf000);put16(image+8,0xf800);
 put32(image+64,base+512);put32(image+68,base+128);
 memcpy(image+128,"gBootArgs.commandLine = [%s]\n",29);
}
static void test_command_line(void){
 uint8_t image[1024];uint32_t base=0x10000000;
 command_line_fixture(image,base);
 assert(it_firmware_find_iboot_command_line(image,sizeof(image),base)==base+512);
 command_line_fixture(image,base+0x1000);
 assert(it_firmware_find_iboot_command_line(image,sizeof(image),base+0x1000)==base+0x1200);
 command_line_fixture(image,base);image[128]='X';
 assert(!it_firmware_find_iboot_command_line(image,sizeof(image),base));
 command_line_fixture(image,base);put32(image+64,base+900);
 assert(!it_firmware_find_iboot_command_line(image,sizeof(image),base));
 command_line_fixture(image,base);put32(image+68,base-4);
 assert(!it_firmware_find_iboot_command_line(image,sizeof(image),base));
 command_line_fixture(image,base);image[4]^=1;
 assert(!it_firmware_find_iboot_command_line(image,sizeof(image),base));
 command_line_fixture(image,base);
 memcpy(image+16,image,10);put16(image+16,0x4c0b);put16(image+18,0x480c);
 assert(!it_firmware_find_iboot_command_line(image,sizeof(image),base));
 assert(!it_firmware_find_iboot_command_line(NULL,1024,base));
 command_line_fixture(image,base);
 for(size_t n=0;n<768;n++)assert(!it_firmware_find_iboot_command_line(image,n,base));
 assert(!it_firmware_find_iboot_command_line(image,1024,UINT32_MAX-512));
}
int main(void){
 test_command_line();
 const ITFirmwareDesc *old=it_firmware_by_build("5F138"),*current=it_firmware_by_build("7E18");
 assert(old && current && !it_firmware_by_build("7E19") && !it_firmware_by_build(NULL));
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
 puts("PASS: exact/ambiguous/truncated firmware, retry/cache/reset, bounded iBoot command-line discovery");
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
