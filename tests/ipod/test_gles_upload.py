#!/usr/bin/env python3
"""Demand-paged texture input is resident before host reads; sizes obey unpack alignment."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'contrib/it-gles/mbxshim.c').read_text()
a=s.index('static int guest_fault_read(');b=s.index('/* ------------------------------------------------------- implemented slots',a)
s=s[a:b]
check=r'''
#include <assert.h>
#include <stdint.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
typedef struct {unsigned host,unpack_alignment;} GuestGC;
'''+s+r'''
static unsigned faults;
static size_t page;
static void fault(int sig,siginfo_t *info,void*ctx){
 uintptr_t p=(uintptr_t)info->si_addr & ~(page-1);
 assert(!mprotect((void*)p,page,PROT_READ));faults++;
}
int main(void){
 GuestGC gc={0};
 assert(texture_bytes(&gc,3,2,0x1907,0x1401)==21);
 gc.unpack_alignment=1;assert(texture_bytes(&gc,3,2,0x1907,0x1401)==18);
 gc.unpack_alignment=8;assert(texture_bytes(&gc,3,2,0x1907,0x1401)==25);
 gc.unpack_alignment=4;assert(texture_bytes(&gc,3,2,0x1908,0x8033)==14);
 assert(!texture_bytes(&gc,~0u,2,0x1908,0x1401));
 assert(!texture_bytes(&gc,1,~0u,0x1908,0x1401));
 assert(!texture_bytes(&gc,1,1,0x1907,0x8033));
 assert(texture_bytes(&gc,4096,4096,0x1908,0x1401)==(64u<<20));
 assert(!texture_bytes(&gc,4096,4097,0x1908,0x1401));
 assert(!guest_fault_read(0,4096));assert(!guest_fault_read(~0UL-8,16));
 assert(!guest_fault_read(1,64u*1024*1024+1));
 page=sysconf(_SC_PAGESIZE);
 void*p=mmap(0,page*3,PROT_NONE,MAP_ANON|MAP_PRIVATE,-1,0);assert(p!=MAP_FAILED);
 struct sigaction action={0};action.sa_sigaction=fault;action.sa_flags=SA_SIGINFO;
 sigemptyset(&action.sa_mask);assert(!sigaction(SIGSEGV,&action,0));assert(!sigaction(SIGBUS,&action,0));
 assert(guest_fault_read((unsigned long)p+page-7,page+14));
 assert(faults==3);munmap(p,page*3);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'check.c';p.write_text(check);exe=Path(d)/'check'
 subprocess.run(['clang',str(p),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
print('PASS: real page faults, unaligned buffers, packed/RGB sizes, unpack alignment, overflow limits')
