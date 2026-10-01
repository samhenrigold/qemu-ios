#!/usr/bin/env python3
"""Run the real READ ID programs of 7E18 and 8C148 iBoot through fmss_run_script."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/ipod_touch_fmss.c').read_text()
header = (root / 'include/hw/arm/ipod_touch_fmss.h').read_text()
functions = []
for name in ('fmss_var_read', 'fmss_run_script'):
    match = re.search(r'^static [^\n]*\b' + name + r'\([^)]*\)\s*\{.*?^}', source, re.M | re.S)
    assert match, name
    functions.append(match.group())
defines = '\n'.join(l for l in source.splitlines() if l.startswith('#define FMSS_CHIP') or l.startswith('#define FMSS_SCRIPT'))
constants = '\n'.join(l for l in header.splitlines() if l.startswith('#define FMSS'))

# iBoot-636.66.33 (7E18) at 0x25330 and iBoot-931.71.16 (8C148) at 0x25a60.
ID_7E18 = """01000c0c 000000ff 01000c10 00000001 01000c58 00000004 01000c4c 00000128
01000004 033338e0 01000048 0ff00ffe 04070d08 ffffffff 04060d0c 0000000f
05050000 00000002 0b000005 00077001 02000000 00000000 01000008 00000090
07000000 00000000 01000048 00000002 0100002c 00000000 0100000c 00000000
01000004 00000001 07010000 00000000 01000048 00000004 01000030 00000004
01000014 00000010 01000004 000000e2 07030000 00000000 01000048 00000008
01000040 00000052 04040040 00000002 0e040000 000000c8 04000060 ffffffff
11000007 00000000 0d060006 00000001 17060000 00000110 0c070007 00000004
13050005 00000001 0e060000 00000048 04000000 fffffe01 02000000 00000000
00000000 00000000"""
ID_8C148 = """01000c0c 000000ff 01000c10 00000001 01000c58 00000004 01000c4c 00000140
01000004 033338e0 01000048 0ff00ffe 04070d08 ffffffff 04060d0c 0000000f
05050000 00000002 0b000005 00077001 02000000 00000000 01000008 00000090
07000000 00000000 01000048 00000002 0100002c 00000000 0100000c 00000000
01000004 00000001 07010000 00000000 01000048 00000004 01000030 00000007
01000014 00000010 01000004 000000e2 07030000 00000000 01000048 00000008
01000040 00000082 04040040 00000002 0e040000 000000c8 04000060 ffffffff
11000007 00000000 04000064 ffffffff 0c070007 00000004 11000007 00000000
0d060006 00000001 17060000 00000128 0c070007 00000004 13050005 00000001
0e060000 00000048 04000000 fffffe01 02000000 00000000 00000000 00000000"""

def words(text):
    return ','.join('0x' + w + 'u' for w in text.split())

prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define LOG_UNIMP 0
#define LOG_GUEST_ERROR 0
#define qemu_log_mask(...) ((void)0)
#define ctz32(x) ((x) ? (unsigned)__builtin_ctz(x) : 32u)
typedef struct {
    uint32_t reg_cs_script, reg_cinfo_target_addr, reg_pages_in_addr, reg_cs_buf_addr;
    uint32_t reg_page_spare_out_addr, reg_pages_out_addr, reg_csgenrc, reg_script_param_d38, reg_script_param_d34, reg_script_param_d48, reg_script_param_d4c, reg_num_pages, reg_chunks_per_page, reg_script_csgenr15, reg_script_scratch_d7c, reg_script_scratch_d3c;
} IPodTouchFMSSState;
static uint8_t mem[0x10000];
typedef struct { int unused; } AddressSpace;
static AddressSpace address_space_memory;
#define MEMTXATTRS_UNSPECIFIED 0
#define MEMTX_OK 0
#define MEMTX_DECODE_ERROR 1
static unsigned descriptor_reads;
static uint32_t descriptor_addresses[8];
static int address_space_read(AddressSpace *as, uint32_t a, int attrs, void *p, size_t n) {
    assert(n == 4 && descriptor_reads < 8);
    descriptor_addresses[descriptor_reads++] = a;
    if ((uint64_t)a + n > sizeof(mem)) {
        memset(p, 0xdd, n); /* A failed transaction must never become a word. */
        return MEMTX_DECODE_ERROR;
    }
    memcpy(p, mem + a, n);
    return MEMTX_OK;
}
static uint32_t ldl_le_p(const void *p) {
    const uint8_t *b = p;
    return b[0] | (uint32_t)b[1]<<8 | (uint32_t)b[2]<<16 | (uint32_t)b[3]<<24;
}
static void cpu_physical_memory_read(uint32_t a, void *p, size_t n) { assert(a + n <= sizeof(mem)); memcpy(p, mem + a, n); }
static void cpu_physical_memory_write(uint32_t a, const void *p, size_t n) { assert(a + n <= sizeof(mem)); memcpy(mem + a, p, n); }
'''
tests = r'''
static const uint32_t id3[] = {%s}, id4[] = {%s};
static void run(const uint32_t *prog, size_t n) {
    IPodTouchFMSSState s = {.reg_cs_script = 0x1000, .reg_cinfo_target_addr = 0x8000,
                            .reg_pages_in_addr = 8};
    memset(mem, 0, sizeof(mem));
    memcpy(mem + 0x1000, prog, n);
    fmss_run_script(&s);
}
static IPodTouchFMSSState run_counter(const uint32_t *prog, size_t n,
                                      uint32_t chunks, uint32_t initial) {
    IPodTouchFMSSState s = {.reg_cs_script=0x1000,
                            .reg_chunks_per_page=chunks,
                            .reg_script_csgenr15=initial};
    memset(mem, 0, sizeof(mem));
    memcpy(mem + 0x1000, prog, n);
    fmss_run_script(&s);
    return s;
}
int main(void) {
    uint32_t *out = (uint32_t *)(mem + 0x8000);
    run(id3, sizeof(id3)); /* 4 bytes per CE, 8 CEs */
    for (int i = 0; i < 8; i++) assert(out[i] == (i < 4 ? 0xb614d5adu : 0));
    assert(out[8] == 0);
    run(id4, sizeof(id4)); /* 8 bytes per CE, 8 CEs */
    for (int i = 0; i < 8; i++) assert(out[2*i] == (i < 4 ? 0xb614d5adu : 0) && out[2*i+1] == 0);
    /* QEMU ctz32(0) returns 32: reject an empty selector before shifting.
     * Multiple CEs and unpopulated CE4 also retain the existing no-chip result. */
    uint32_t selector[] = {
        0x05000000u, 0, 0x02000000u, 0,
        0x01000008u, 0x90u, 0x04010060u, 0xffffffffu,
        0x05020000u, 0x9000u, 0x11010002u, 0, 0, 0
    };
    const unsigned selects[] = {0, 3, 0x10, 1, 2, 4, 8};
    for (unsigned j = 0; j < sizeof(selects)/sizeof(selects[0]); j++) {
        selector[1] = (selects[j] << 1) | 1;
        run(selector, sizeof(selector));
        uint32_t expected = (selects[j]==1 || selects[j]==2 ||
                             selects[j]==4 || selects[j]==8) ? 0xb614d5adu : 0;
        assert(ldl_le_p(mem + 0x9000) == expected);
    }
    /* Captured13030002/0 shifts current destination by source register.
     * Direct stores observe destination and unchanged count without OR oracle. */
    uint32_t register_shift[] = {
        0x05020000u,0, 0x05030000u,0,
        0x13030002u,0, 0x05070000u,0x9e00u,
        0x11030007u,0, 0x0c070007u,4, 0x11020007u,0, 0,0
    };
    const uint32_t shifts[][2] = {
        {0x801u,0}, {0x80000001u,0}, {0x80000001u,1},
        {3,1}, {1,31}, {0x12345678u,4}, {0,31}
    };
    for (unsigned j=0; j<sizeof(shifts)/sizeof(shifts[0]); j++) {
        register_shift[1]=shifts[j][1]; register_shift[3]=shifts[j][0];
        run(register_shift,sizeof(register_shift));
        assert(ldl_le_p(mem+0x9e00)==(shifts[j][0]<<shifts[j][1]));
        assert(ldl_le_p(mem+0x9e04)==shifts[j][1]);
    }
    uint32_t chip_mask[] = {
        0x05020000u,0, 0x05030000u,1,
        0x13030002u,0, 0x13000003u,1,
        0x02000000u,0, 0x04010000u,0xffffffffu,
        0x05070000u,0x9e00u, 0x11030007u,0,
        0x0c070007u,4, 0x11010007u,0, 0,0
    };
    for (unsigned chip=0; chip<4; chip++) {
        chip_mask[1]=chip;run(chip_mask,sizeof(chip_mask));
        assert(ldl_le_p(mem+0x9e00)==(1u<<chip));
        assert(ldl_le_p(mem+0x9e04)==(2u<<chip));
    }
    const uint32_t immediate_shift[] = {
        0x05020000u,3, 0x05030000u,1,
        0x13030002u,1, 0x05070000u,0x9e00u,
        0x11030007u,0, 0,0
    };
    run(immediate_shift,sizeof(immediate_shift));
    assert(ldl_le_p(mem+0x9e00)==6); /* Preserve source-based nonzero form. */
    uint32_t unsupported_register_shift[] = {
        0x05020000u,32, 0x05030000u,0x89abcdefu,
        0x05070000u,0x9e00u, 0x11030007u,0,
        0x13030002u,0, 0x11030007u,0, 0,0
    };
    const uint32_t unmeasured_counts[] = {32,33,0xffffffffu};
    for (unsigned j=0;j<sizeof(unmeasured_counts)/sizeof(unmeasured_counts[0]);j++) {
        unsupported_register_shift[1]=unmeasured_counts[j];
        run(unsupported_register_shift,sizeof(unsupported_register_shift));
        assert(ldl_le_p(mem+0x9e00)==0x89abcdefu); /* No later store. */
    }
    /* Captured zero-immediate OR preserves destination config bits.
     * Direct RAM observations exclude a replacement assignment or AND. */
    uint32_t register_or[] = {
        0x05000000u, 0, 0x05010000u, 0,
        0x0b000001u, 0, 0x05070000u, 0x9d00u,
        0x11000007u, 0, 0x0c070007u, 4,
        0x11010007u, 0, 0, 0
    };
    const uint32_t or_values[][2] = {
        {0x801u,0}, {0x801u,0x20011000u}, {0xff00ff00u,0x0ff00ff0u},
        {0,0x89abcdefu}, {0xffffffffu,0}, {0x80000000u,1}
    };
    for (unsigned j=0; j<sizeof(or_values)/sizeof(or_values[0]); j++) {
        register_or[1]=or_values[j][0]; register_or[3]=or_values[j][1];
        run(register_or,sizeof(register_or));
        assert(ldl_le_p(mem+0x9d00)==(or_values[j][0]|or_values[j][1]));
        assert(ldl_le_p(mem+0x9d04)==or_values[j][1]);
    }
    const uint32_t config_or[] = {
        0x05000000u,0x801u, 0x04010d4cu,0xffffffffu,
        0x0b000001u,0, 0x02000000u,0,
        0x04020000u,0xffffffffu, 0x05070000u,0x9d00u,
        0x11020007u,0, 0x05020000u,0xdeadbeeeu,
        0x0b020001u,0x01000801u, 0x0c070007u,4,
        0x11020007u,0, 0x0b010001u,0,
        0x0c070007u,4, 0x11010007u,0, 0,0
    };
    const uint32_t configs[] = {0,0x20011000u,0xffffffffu};
    for (unsigned j=0; j<sizeof(configs)/sizeof(configs[0]); j++) {
        IPodTouchFMSSState state={.reg_cs_script=0x1000,
                                  .reg_script_param_d4c=configs[j]};
        memset(mem,0,sizeof(mem));
        memcpy(mem+0x1000,config_or,sizeof(config_or));
        fmss_run_script(&state);
        assert(ldl_le_p(mem+0x9d00)==(configs[j]|0x801u));
        assert(ldl_le_p(mem+0x9d04)==(configs[j]|0x01000801u));
        assert(ldl_le_p(mem+0x9d08)==configs[j]); /* Self OR preserves source. */
        assert(state.reg_script_param_d4c==configs[j]);
    }
    /* Captured read-loop D54 forms: D28 seeds CSGENR15; each iteration
     * reads, decrements and writes it. Counts vary to exclude a forced two. */
    const uint32_t counter_loop[] = {
        0x04000d28u, 0xffffffffu, 0x02000d54u, 0,
        0x05010000u, 0, 0x05020000u, 0x9c00u,
        0x04070d54u, 0xffffffffu, 0x0d070007u, 1,
        0x02070d54u, 0, 0x0c010001u, 1,
        0x0e070000u, 0x20u,
        0x04000d54u, 0xffffffffu, 0x11000002u, 0,
        0x0c020002u, 4, 0x11010002u, 0, 0, 0
    };
    const uint32_t chunk_counts[] = {1, 2, 3, 7};
    for (unsigned j=0; j<sizeof(chunk_counts)/sizeof(chunk_counts[0]); j++) {
        IPodTouchFMSSState state=run_counter(counter_loop,sizeof(counter_loop),
                                           chunk_counts[j],0xdeadbeefu);
        assert(ldl_le_p(mem+0x9c00)==0);
        assert(ldl_le_p(mem+0x9c04)==chunk_counts[j]);
        assert(state.reg_script_csgenr15==0);
        assert(state.reg_chunks_per_page==chunk_counts[j]);
    }
    const uint32_t scalar_counter[] = {
        0x04000d28u, 0xffffffffu, 0x02000d54u, 0,
        0x04010d54u, 0x00ff00ffu, 0x05020000u, 0x9c00u,
        0x11010002u, 0, 0, 0
    };
    const uint32_t scalars[] = {0,0x12345678u,0xffffffffu};
    for (unsigned j=0; j<sizeof(scalars)/sizeof(scalars[0]); j++) {
        IPodTouchFMSSState state=run_counter(scalar_counter,sizeof(scalar_counter),
                                           scalars[j],0xdeadbeefu);
        assert(state.reg_script_csgenr15==scalars[j]);
        assert(ldl_le_p(mem+0x9c00)==(scalars[j]&0x00ff00ffu));
    }
    uint32_t unsupported_counter[] = {
        0x05000000u, 7, 0x01000d54u, 3,
        0x05020000u, 0x9c00u, 0x11000002u, 0, 0, 0
    };
    for (unsigned form=0; form<2; form++) {
        unsupported_counter[2]=form ? 0x02000d54u : 0x01000d54u;
        IPodTouchFMSSState state=run_counter(unsupported_counter,
                sizeof(unsupported_counter),2,0xdeadbeefu);
        assert(state.reg_script_csgenr15==0xdeadbeefu);
        assert(ldl_le_p(mem+0x9c00)==0); /* Stop before later store. */
    }
    /* Captured bulk/read immediate-16 forms, bit31-clear operands only.
     * These values establish field splitting, not logical-vs-arithmetic SHR. */
    uint32_t shift_right[] = {
        0x05010000u, 0, 0x05000000u, 0x76543210u,
        0x14000001u, 16, 0x05060000u, 0x9b00u,
        0x11000006u, 0,
        0x05070000u, 0, 0x05010000u, 0x76543210u,
        0x14010007u, 16, 0x0c060006u, 4,
        0x11010006u, 0, 0, 0
    };
    const uint32_t rows[] = {0, 0x12345u, 0x00ffffffu, 0x12345678u, 0x7fffffffu};
    for (unsigned j = 0; j < sizeof(rows)/sizeof(rows[0]); j++) {
        shift_right[1] = rows[j];
        shift_right[11] = rows[j];
        run(shift_right, sizeof(shift_right));
        assert(ldl_le_p(mem + 0x9b00) == rows[j] >> 16);
        assert(ldl_le_p(mem + 0x9b04) == rows[j] >> 16);
    }
    uint32_t unsupported_shift[] = {
        0x05010000u, 0x12345678u, 0x05000000u, 7,
        0x05060000u, 0x9b00u, 0x11000006u, 0,
        0x14000001u, 0, 0x11000006u, 0, 0, 0
    };
    const uint32_t counts[] = {0, 1, 15, 17, 31, 32};
    for (unsigned j = 0; j < sizeof(counts)/sizeof(counts[0]); j++) {
        unsupported_shift[9] = counts[j];
        run(unsupported_shift, sizeof(unsupported_shift));
        assert(ldl_le_p(mem + 0x9b00) == 7);
    }
    unsupported_shift[9] = 16;
    const uint32_t unmeasured[] = {0x80000000u, 0x89abcdefu, 0xffffffffu};
    for (unsigned j = 0; j < sizeof(unmeasured)/sizeof(unmeasured[0]); j++) {
        unsupported_shift[1] = unmeasured[j];
        run(unsupported_shift, sizeof(unsupported_shift));
        assert(ldl_le_p(mem + 0x9b00) == 7);
    }
    /* Actual stock 5F138 opcode06 forms, observed with existing RAM stores.
     * Distinct values and zero-overwrite distinguish assignment from OR,
     * addition or accidental operand reversal. Self-copy must preserve input. */
    uint32_t copies[] = {
        0x05030000u, 0x89abcdefu, 0x05040000u, 0x13579bdfu,
        0x06040003u, 0, 0x05070000u, 0x9300u,
        0x11040007u, 0, 0x05010000u, 0x89abcdefu,
        0x06010001u, 0,
        0x0c070007u, 4, 0x11010007u, 0,
        0x05030000u, 0, 0x06040003u, 0,
        0x0c070007u, 4, 0x11040007u, 0,
        0x05030000u, 4, 0x05040000u, 8,
        0x06020003u, 0, 0x06030004u, 0,
        0x0c070007u, 4, 0x11020007u, 0,
        0x0c070007u, 4, 0x11030007u, 0, 0, 0
    };
    run(copies, sizeof(copies));
    uint32_t *copied = (uint32_t *)(mem + 0x9300);
    assert(copied[0] == 0x89abcdefu && copied[1] == 0x89abcdefu);
    assert(copied[2] == 0 && copied[3] == 4 && copied[4] == 8);
    uint32_t unknown_copy[] = {
        0x05030000u, 7, 0x05070000u, 0x9400u,
        0x06040003u, 1, 0x11040007u, 0, 0, 0
    };
    run(unknown_copy, sizeof(unknown_copy));
    assert(*(uint32_t *)(mem + 0x9400) == 0); /* Stop before later DMA. */
    /* Exact immediate-zero opcode03 forms from stock 5F138 scripts.
     * Observe words directly through opcode11, without disputed arithmetic. */
    uint32_t descriptors[] = {
        0x05000000u, 0x9800u, 0x05010000u, 0xdeadc0deu,
        0x03010000u, 0, 0x05060000u, 0x9900u,
        0x11010006u, 0, 0x0c000000u, 4,
        0x05020000u, 0xdeadc0deu, 0x03020000u, 0,
        0x0c060006u, 4, 0x11020006u, 0,
        0x05010000u, 0x9808u, 0x05070000u, 0xdeadc0deu,
        0x03070001u, 0, 0x0c060006u, 4,
        0x11070006u, 0, 0x0c010001u, 4,
        0x05000000u, 0xdeadc0deu, 0x03000001u, 0,
        0x0c060006u, 4, 0x11000006u, 0, 0, 0
    };
    const uint8_t values[] = {1,0x23,0x45,0x67, 0xff,0xee,0xdd,0xcc,
                              0,0,0,0, 0x78,0x56,0x34,0x12};
    IPodTouchFMSSState descriptor_state = {.reg_cs_script=0x1000};
    memset(mem, 0, sizeof(mem));
    memcpy(mem + 0x1000, descriptors, sizeof(descriptors));
    memcpy(mem + 0x9800, values, sizeof(values));
    descriptor_reads = 0;
    fmss_run_script(&descriptor_state);
    const uint32_t expected_words[] = {0x67452301u,0xccddeeffu,0,0x12345678u};
    assert(descriptor_reads == 4);
    for (unsigned j = 0; j < 4; j++) {
        assert(descriptor_addresses[j] == 0x9800u + 4*j);
        assert(ldl_le_p(mem + 0x9900 + 4*j) == expected_words[j]);
    }
    assert(!memcmp(mem + 0x9800, values, sizeof(values)));
    uint32_t rejected_load[] = {
        0x05000000u, 0xfffffff0u, 0x05020000u, 7,
        0x03020000u, 1, 0x05060000u, 0x9900u,
        0x11020006u, 0, 0, 0
    };
    uint32_t sentinel = 0xabcddcbau;
    for (unsigned immediate = 0; immediate < 2; immediate++) {
        rejected_load[5] = immediate;
        memcpy(mem + 0x1000, rejected_load, sizeof(rejected_load));
        memcpy(mem + 0x9900, &sentinel, 4);
        descriptor_reads = 0;
        fmss_run_script(&descriptor_state);
        assert(descriptor_reads == (immediate == 0 ? 1u : 0u));
        assert(ldl_le_p(mem + 0x9900) == sentinel);
    }
    uint32_t bad[] = {0x05000000u, 7, 0x11000001u, 0, 0x99000000u, 0, 0x11000000u, 0, 0, 0};
    run(bad, sizeof(bad)); /* an unknown op stops the program */
    assert(mem[0] == 7 && mem[7] == 0);
    /* Stock 5F138 NAND scripts read CPU parameter D4C before writing
     * FMCTRL0. Keep the observed read/OR/write sequence and inspect its value
     * through a sequencer memory store, without faking NAND completion. */
    uint32_t parameter[] = {
        0x04010d4cu, 0xffffffffu, 0x0b000001u, 0x801u,
        0x02000000u, 0, 0x04020000u, 0xffffffffu,
        0x05030000u, 0x9000u, 0x11020003u, 0, 0, 0
    };
    IPodTouchFMSSState s = {.reg_cs_script = 0x1000,
                            .reg_script_param_d4c = 0x20011000u};
    memcpy(mem + 0x1000, parameter, sizeof(parameter));
    fmss_run_script(&s);
    assert(*(uint32_t *)(mem + 0x9000) == 0x20011801u);
    s.reg_script_param_d4c = 0;
    fmss_run_script(&s);
    assert(*(uint32_t *)(mem + 0x9000) == 0x801u);
    bool ok = true;
    assert(fmss_var_read(&s, FMSS_SCRIPT_PARAM_D4C, &ok) == 0 && ok);
    ok = true; fmss_var_read(&s, 0xd50, &ok); assert(!ok);
    /* Stock bulk-read programs consume D18 as a page counter. Exercise
     * the observed read/check/decrement loop using supported instructions. */
    uint32_t page_count[] = {
        0x04070d18u, 0xffffffffu, 0x17070000u, 0x40u,
        0x05000000u, 0x9100u, 0x11070000u, 0,
        0x0d070007u, 1, 0x0c000000u, 4,
        0x0e070000u, 0x18u, 0, 0, 0, 0
    };
    memcpy(mem + 0x1000, page_count, sizeof(page_count));
    for (unsigned count = 0; count <= 3; count++) {
        memset(mem + 0x9100, 0, 16);
        s.reg_num_pages = count;
        fmss_run_script(&s);
        uint32_t *observed = (uint32_t *)(mem + 0x9100);
        for (unsigned j = 0; j < count; j++) assert(observed[j] == count-j);
        assert(observed[count] == 0);
        assert(s.reg_num_pages == count); /* Register input is not loop scratch. */
    }
    uint32_t chunks[] = {
        0x04000d28u, 0xffffffffu, 0x05010000u, 0x9200u,
        0x11000001u, 0, 0, 0
    };
    memcpy(mem + 0x1000, chunks, sizeof(chunks));
    for (unsigned count = 0; count <= 4; count++) {
        s.reg_chunks_per_page = count;
        fmss_run_script(&s);
        assert(*(uint32_t *)(mem + 0x9200) == count);
    }
    /* Stock bulk0A000004/0 intersects current and previous chip masks.
     * Observe through existing opcode11, without disputed OR/SHL. */
    uint32_t register_and[] = {
        0x05000000u, 1, 0x05040000u, 1,
        0x0a000004u, 0, 0x05070000u, 0x9a00u,
        0x11000007u, 0, 0, 0
    };
    const uint32_t intersections[][2] = {
        {1,1}, {1,2}, {0xff00ff00u,0x0ff00ff0u},
        {0,0xffffffffu}, {0xffffffffu,0}, {0x80000000u,0x80000001u}
    };
    for (unsigned j = 0; j < sizeof(intersections)/sizeof(intersections[0]); j++) {
        register_and[1] = intersections[j][0];
        register_and[3] = intersections[j][1];
        run(register_and, sizeof(register_and));
        assert(ldl_le_p(mem + 0x9a00) == (intersections[j][0] & intersections[j][1]));
    }
    /* Actual status0A020006/0 operand form; destination must participate. */
    uint32_t status_and[] = {
        0x05020000u, 8, 0x05060000u, 0xau,
        0x0a020006u, 0, 0x05070000u, 0x9a00u,
        0x11020007u, 0, 0, 0
    };
    run(status_and, sizeof(status_and));
    assert(ldl_le_p(mem + 0x9a00) == 8);
    /* Nonzero mask selects source&immediate, not destination&source. */
    uint32_t immediate_and[] = {
        0x05000000u, 0x12345679u, 0x05010000u, 0xdeadbeeeu,
        0x0a010000u, 1, 0x05070000u, 0x9a00u,
        0x11010007u, 0, 0, 0
    };
    const uint32_t masks[] = {1,0x1fu,0xffu,0xfffffe01u};
    for (unsigned j = 0; j < sizeof(masks)/sizeof(masks[0]); j++) {
        immediate_and[5] = masks[j];
        run(immediate_and, sizeof(immediate_and));
        assert(ldl_le_p(mem + 0x9a00) == (0x12345679u & masks[j]));
    }
    puts("PASS: register OR config, D54 chunk loops, bounded opcode14 right16, opcode0A forms, FMSS opcode03 LE DMA/rejection, opcode06 copies/rejection, D18 page counter, D28 chunks, D4C parameter, READ ID programs of 7E18 and 8C148, unknown op stops");
}
''' % (words(ID_7E18), words(ID_8C148))
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'check.c'
    c.write_text(prelude + constants + '\n' + defines + '\n' + '\n'.join(functions) + tests)
    binary = str(Path(tmp) / 'check')
    subprocess.run(['clang', '-std=c11', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-g', str(c), '-o', binary], check=True)
    subprocess.run([binary], check=True)
