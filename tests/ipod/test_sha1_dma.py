#!/usr/bin/env python3
"""Full-width SHA DMA hashes restore-sized messages with bounded host storage."""
from pathlib import Path
import hashlib, re, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/ipod_touch_sha1.c').read_text()
header = (root / 'include/hw/arm/ipod_touch_sha1.h').read_text()
state = re.search(r'typedef struct IPodTouchSHA1State.*?} IPodTouchSHA1State;', header, re.S)[0]
constants = '\n'.join(re.findall(r'^#define SHA_.*$', header, re.M))
production = source[source.index('static const uint32_t sha1_iv'):source.index('static const MemoryRegionOps')]
length = 0x1824000
message = bytes((i * 31 + (i >> 19)) & 255 for i in range(length))
expected = ','.join(str(x) for x in hashlib.sha1(message).digest())
code = r'''
#include <assert.h>
#define trace_event_get_state_backends(id) 0
#define TRACE_PRINTF(fn, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int SysBusDevice, MemoryRegion, qemu_irq;
typedef uint64_t hwaddr;
#define MIN(a,b) ((a)<(b)?(a):(b))
#define IT_SHA1_DMA_CHUNK (64 * 1024)
#define bswap32 __builtin_bswap32
#define g_malloc dma_alloc
#define g_free free
static void *dma_alloc(size_t n) {assert(n <= 65536);return malloc(n);}
static int line;
static void qemu_irq_raise(qemu_irq irq) {line=1;}
static void qemu_irq_lower(qemu_irq irq) {line=0;}
static uint8_t *memory;
static size_t memory_size, biggest_read;
static void cpu_physical_memory_read(uint64_t a, void *p, size_t n) {
 assert(a <= memory_size && n <= memory_size-a && n <= 65536);
 if(n>biggest_read)biggest_read=n;
 memcpy(p,memory+a,n);
}
''' + (root / 'include/hw/arm/sha1_compress.h').read_text() + constants + '\n' + state + '\n' + production + '\n' + f'static const uint8_t expected[20]={{{expected}}};\n' + r'''
#define W(o,v) ipod_touch_sha1_write(&s,(o),(v),4)
int main(void) {
 const unsigned n=0x1824000, padded=n+64;
 memory_size=padded+7;memory=calloc(1,memory_size);
 for(unsigned i=0;i<n;i++)memory[i]=(uint8_t)(i*31+(i>>19));
 memory[n]=0x80;
 uint64_t bits=(uint64_t)n*8;
 for(unsigned i=0;i<8;i++)memory[padded-1-i]=bits>>(8*i);
 memset(memory+padded,0xa5,7); /* Raw hardware ignores incomplete blocks. */
 IPodTouchSHA1State s={.irq=1};
 W(SHA_INSIZE,UINT32_MAX);
 assert(ipod_touch_sha1_read(&s,SHA_INSIZE,4)==UINT32_MAX);
 W(SHA_RESET,1);W(SHA_MEMORY_MODE,1);W(SHA_INSIZE,memory_size);
 W(SHA_INTENABLE,1);W(SHA_CONFIG,6);
 assert(s.last_hash_valid && !memcmp(s.last_hash,expected,20) && line);
 assert(biggest_read<=65536);
 /* Continuing from an earlier DMA job must yield the same digest. */
 W(SHA_RESET,1);W(SHA_MEMORY_MODE,1);W(SHA_INSIZE,65536);W(SHA_CONFIG,2);
 W(SHA_MEMORY_START,65536);W(SHA_INSIZE,padded-65536);W(SHA_CONFIG,2);
 assert(!memcmp(s.last_hash,expected,20) && !line);
 free(memory);
 puts("PASS: complete restore-sized SHA DMA matches hashlib, continued state and ignored partial block");
}
'''
with tempfile.TemporaryDirectory(prefix='it-sha-dma-') as tmp:
    tmp = Path(tmp)
    (tmp / 'check.c').write_text(code)
    subprocess.run(['cc','-fsanitize=address,undefined',str(tmp/'check.c'),'-o',str(tmp/'check')],check=True)
    subprocess.run([str(tmp/'check')],check=True)
