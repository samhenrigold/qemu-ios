#!/usr/bin/env python3
"""Kernel identification with no guest required (the iBoot literal finders live in test_iboot_literals.py)."""
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

int main(void){
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
 puts("PASS: exact/ambiguous/truncated firmware, retry/cache/reset");
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
