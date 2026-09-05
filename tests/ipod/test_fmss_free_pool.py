#!/usr/bin/env python3
"""Generated FTL allocation must not overlap any page in the GPT volume."""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / 'hw/arm/ipod_touch_fmss.c').read_text()
functions = []
for name in ('fmss_packed_page', 'fmss_total_blocks', 'fmss_fix_generated_free_pool'):
    match = re.search(r'^static [^\n]*\b' + name + r'\([^)]*\)\s*\{.*?^}', source, re.M | re.S)
    assert match, name
    functions.append(match.group())
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#define NAND_BYTES_PER_PAGE 4096
#define NAND_BYTES_PER_SPARE 64
#define NAND_DEFAULT_TOTAL_BLOCKS 128000
#define NAND_MAX_TOTAL_BLOCKS 2090000
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define LOG_GUEST_ERROR 1
#define qemu_log_mask(...) ((void)0)
typedef struct {
    const uint8_t *packed, *packed_records;
    const uint32_t *packed_index;
    uint32_t packed_num_cs, packed_pages_per_cs, total_blocks;
    size_t packed_record_count;
    const char *nand_path;
    bool total_blocks_from_gpt;
} IPodTouchFMSSState;
static bool physical;
static unsigned errors;
static bool fmss_physical(void) { return physical; }
static bool fmss_io_error(const char *s, int e) { errors++; return false; }
static uint32_t ldl_le_p(const void *p) { uint32_t v; memcpy(&v,p,4); return v; }
static uint64_t ldq_le_p(const void *p) { uint64_t v; memcpy(&v,p,8); return v; }
static uint16_t lduw_le_p(const void *p) { uint16_t v; memcpy(&v,p,2); return v; }
static void stw_le_p(void *p, uint16_t v) { memcpy(p,&v,2); }
static void stl(void *p, uint32_t v) { memcpy(p,&v,4); }
static void stq(void *p, uint64_t v) { memcpy(p,&v,8); }
static void context(uint8_t *data, uint8_t *spare) {
    memset(data,0,4096); memset(spare,0,64);
    stl(data+8,20); spare[9]=0x43;
    stl(data+2040,0x46560000); stl(data+2044,0xb9a9ffff);
    for (int i=0;i<20;i++) stw_le_p(data+14+2*i,i+3);
}
'''
tests = r'''
int main(int argc, char **argv) {
    uint8_t record[4160]={0}, data[4096], spare[64], before[4096];
    uint32_t index[3*257]={0}; index[2*257+256]=1;
    stq(record+32,3); stq(record+40,1835021);
    IPodTouchFMSSState s={.packed=record,.packed_records=record,
        .packed_index=index,.packed_num_cs=3,.packed_pages_per_cs=257,
        .packed_record_count=1};
    /* Packed images must not silently fall back to the old 128000-page size. */
    assert(fmss_total_blocks(&s)==1835019 && s.total_blocks_from_gpt);
    context(data,spare); memcpy(before,data,4096);
    fmss_fix_generated_free_pool(&s,3,255,data,spare);
    assert(errors==0 && lduw_le_p(data+14)==1794);
    for(int i=0;i<20;i++) {
        unsigned vb=lduw_le_p(data+14+2*i);
        assert(vb==1794+i);
        assert((vb-1)*1024 > 1835021);
    }
    memcpy(before+14,data+14,40); assert(!memcmp(data,before,4096));
    fmss_fix_generated_free_pool(&s,3,255,data,spare);
    assert(!memcmp(data,before,4096)); /* idempotent */
    context(data,spare); memcpy(before,data,4096);
    physical=true; fmss_fix_generated_free_pool(&s,3,255,data,spare);
    physical=false; assert(!memcmp(data,before,4096));
    data[2040]^=1; memcpy(before,data,4096);
    fmss_fix_generated_free_pool(&s,3,255,data,spare);
    assert(!memcmp(data,before,4096)); /* unknown metadata untouched */
    context(data,spare); stw_le_p(data+14,80); memcpy(before,data,4096);
    fmss_fix_generated_free_pool(&s,3,255,data,spare);
    assert(!memcmp(data,before,4096)); /* live/custom free list untouched */
    context(data,spare); s.total_blocks=2089999;
    fmss_fix_generated_free_pool(&s,3,255,data,spare);
    assert(errors==1 && lduw_le_p(data+14)==3); /* no room: no partial patch */
    s.total_blocks=0; stq(record+32,0);
    s.total_blocks_from_gpt=false;
    fmss_fix_generated_free_pool(&s,3,255,data,spare);
    assert(errors==2 && lduw_le_p(data+14)==3);
    /* Directory and packed capacity must agree. */
    char path[1024]; snprintf(path,sizeof(path),"%s/cs2",argv[1]);
    assert(mkdir(path,0700)==0);
    snprintf(path,sizeof(path),"%s/cs2/256.page",argv[1]);
    stq(record+32,3); FILE *f=fopen(path,"wb"); assert(f);
    assert(fwrite(record,1,sizeof(record),f)==sizeof(record)); fclose(f);
    IPodTouchFMSSState dir={.nand_path=argv[1]};
    assert(fmss_total_blocks(&dir)==1835019 && dir.total_blocks_from_gpt);
    puts("PASS: disjoint generated FTL pool, packed/directory capacity, guards and bounds");
}
'''
with tempfile.TemporaryDirectory() as d:
    c = Path(d) / 'check.c'
    c.write_text(prelude + '\n'.join(functions) + tests)
    exe = Path(d) / 'check'
    subprocess.run(['clang', '-fsanitize=address,undefined', '-g', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe), d], check=True)
