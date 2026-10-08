/* Validate actual FMSS interpreter memory results and LE wire bytes.
 *
 * SLICE include/hw/arm/ipod_touch_fmss.h define FMSS
 * SLICE hw/arm/ipod_touch_fmss.c define FMSS_CHIP|FMSS_SCRIPT
 * SLICE hw/arm/ipod_touch_fmss.c fn fmss_var_read fmss_run_script
 */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define fmss_script_trace(...) ((void)0)
#define LOG_UNIMP 0
#define LOG_GUEST_ERROR 1
#define MEMTXATTRS_UNSPECIFIED 0
#define MEMTX_OK 0
#define MEMTX_ERROR 1
#define MEMTX_DECODE_ERROR 2
#define MEMTX_ACCESS_ERROR 4
#define ctz32(x) ((x) ? (unsigned)__builtin_ctz(x) : 32u)
typedef struct {
 uint32_t reg_cs_script, reg_cinfo_target_addr, reg_pages_in_addr, reg_cs_buf_addr;
 uint32_t reg_num_pages, reg_page_spare_out_addr, reg_pages_out_addr, reg_chunks_per_page;
 uint32_t reg_csgenrc, reg_script_param_d34, reg_script_param_d38;
 uint32_t reg_script_param_d48, reg_script_param_d4c, reg_script_csgenr15;
 uint32_t reg_script_scratch_d7c, reg_script_scratch_d3c;
} IPodTouchFMSSState;
typedef struct {int unused;} AddressSpace;
static AddressSpace address_space_memory;
static uint8_t mem[4096];
static uint32_t read_fault = UINT32_MAX, write_fault = UINT32_MAX;
static unsigned fault_result, fetch_errors, store_errors, write_attempts;
static void qemu_log_mask(int flag, const char *fmt, ...) {
 if (strstr(fmt,"instruction fetch")) fetch_errors++;
 if (strstr(fmt,"sequencer store")) store_errors++;
}
static uint32_t ldl_le_p(const void *v) {
 const uint8_t *p=v;
 return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void stl_le_p(void *v, uint32_t x) {
 uint8_t *p=v;
 for (unsigned i=0;i<4;i++) p[i]=x>>(8*i);
}
static unsigned address_space_read(AddressSpace *as,uint32_t addr,int attrs,void *v,size_t n) {
 assert(n==4 || n==8);
 if (addr==read_fault || (uint64_t)addr+n>sizeof(mem)) {
  memset(v,0,n); /* An unchecked fault can be confused with END. */
  return fault_result;
 }
 memcpy(v,mem+addr,n);return MEMTX_OK;
}
static unsigned address_space_write(AddressSpace *as,uint32_t addr,int attrs,const void *v,size_t n) {
 assert(n==4);write_attempts++;
 if (addr==write_fault || (uint64_t)addr+n>sizeof(mem)) return fault_result;
 memcpy(mem+addr,v,n);return MEMTX_OK;
}
static void cpu_physical_memory_read(uint32_t addr,void *v,size_t n) {
 (void)address_space_read(&address_space_memory,addr,0,v,n);
}
static void cpu_physical_memory_write(uint32_t addr,const void *v,size_t n) {
 (void)address_space_write(&address_space_memory,addr,0,v,n);
}
static void reset_probe(void) {
 memset(mem,0,sizeof(mem));read_fault=write_fault=UINT32_MAX;
 fetch_errors=store_errors=write_attempts=0;fault_result=MEMTX_ERROR;
}
static void program(const uint32_t *words,unsigned count) {
 for (unsigned i=0;i<count;i++) stl_le_p(mem+0x100+4*i,words[i]);
}
static void fmss_run_script(IPodTouchFMSSState *s);
static void run_probe(void) {
 IPodTouchFMSSState s={.reg_cs_script=0x100};fmss_run_script(&s);
}
#include "slice.h"
static void valid_store(void) {
 const uint32_t words[]={0x05000000,0x67452301,0x05010000,0x900,
                        0x11000001,0,0,0};
 reset_probe();program(words,8);run_probe();
 const uint8_t expected[]={1,0x23,0x45,0x67};
 assert(!memcmp(mem+0x900,expected,4));
 assert(write_attempts==1 && !store_errors && !fetch_errors);
}
static void failed_store(void) {
 const uint32_t words[]={0x05000000,0x12345678,0x05010000,0xeeee,
                        0x11000001,0,0x05010000,0x900,
                        0x11000001,0,0,0};
 for (unsigned status=1;status<=7;status++) {
  reset_probe();fault_result=status;write_fault=0xeeee;
  program(words,12);stl_le_p(mem+0x900,0xabcddcba);run_probe();
  assert(write_attempts==1 && store_errors==1 && !fetch_errors);
  assert(ldl_le_p(mem+0x900)==0xabcddcba);
 }
}
static void failed_fetch(void) {
 const uint32_t words[]={0x05000000,0x89abcdef,0x05010000,0x900,
                        0x11000001,0,0x05010000,0x904,
                        0x11000001,0,0,0};
 for (unsigned status=1;status<=7;status++) {
  reset_probe();fault_result=status;read_fault=0x118;
  program(words,12);stl_le_p(mem+0x904,0xabcddcba);run_probe();
  assert(fetch_errors==1 && !store_errors && write_attempts==1);
  assert(ldl_le_p(mem+0x900)==0x89abcdef);
  assert(ldl_le_p(mem+0x904)==0xabcddcba);
 }
 reset_probe();read_fault=0x100;fault_result=MEMTX_DECODE_ERROR;run_probe();
 assert(fetch_errors==1 && !write_attempts);
}
int main(void) {
 valid_store();
 failed_store();failed_fetch();
 puts("PASS actual interpreter checked fetch/store results and explicit little-endian RAM bytes");
}
