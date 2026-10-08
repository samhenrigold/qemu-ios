/* Shared stubs for the FMSS script interpreter slices (ipod-fmss-script.c, ipod-fmss-script-trace.c): a 64 KiB
 * little-endian RAM behind address_space_read/write that counts reads, writes and 4-byte descriptor reads.
 * A test that traces defines FMSS_SCRIPT_TRACE_TEST first and supplies TRACE_PRINTF and fmss_script_trace_on.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef FMSS_SCRIPT_TRACE_TEST
#define fmss_script_trace(...) ((void)0)
#endif
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
static unsigned descriptor_reads, reads, writes;
static uint32_t descriptor_addresses[8];
static int address_space_read(AddressSpace *as, uint32_t a, int attrs, void *p, size_t n) {
    reads++; assert(n == 4 || n == 8);
    if (n == 4) {
        assert(descriptor_reads < 8);
        descriptor_addresses[descriptor_reads++] = a;
    }
    if ((uint64_t)a + n > sizeof(mem)) {
        memset(p, 0xdd, n); /* A failed transaction must never become a word. */
        return MEMTX_DECODE_ERROR;
    }
    memcpy(p, mem + a, n);
    return MEMTX_OK;
}
static int address_space_write(AddressSpace *as, uint32_t a, int attrs,
                               const void *p, size_t n) {
    writes++; if ((uint64_t)a + n > sizeof(mem)) return MEMTX_DECODE_ERROR;
    memcpy(mem + a, p, n);
    return MEMTX_OK;
}
static void stl_le_p(void *p, uint32_t value) {
    uint8_t *bytes = p;
    for (unsigned i = 0; i < 4; i++) bytes[i] = value >> (8 * i);
}
static uint32_t ldl_le_p(const void *p) {
    const uint8_t *b = p;
    return b[0] | (uint32_t)b[1]<<8 | (uint32_t)b[2]<<16 | (uint32_t)b[3]<<24;
}
static void cpu_physical_memory_read(uint32_t a, void *p, size_t n) { assert(a + n <= sizeof(mem)); memcpy(p, mem + a, n); }
static void cpu_physical_memory_write(uint32_t a, const void *p, size_t n) { assert(a + n <= sizeof(mem)); memcpy(mem + a, p, n); }
