/* CPU compatibility DMA checks using actual FMSS handlers and adversarial transactions.
 *
 * SLICE hw/arm/ipod_touch_fmss.c fn fmss_write_dma_read fmss_read_dma_word fmss_read_dma_write find_bit_index read_nand_pages
 */
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define NAND_BYTES_PER_PAGE 4096
#define qemu_log_mask(...) ((void)0)
#define LOG_GUEST_ERROR 0
#define MEMTX_OK 0
#define MEMTX_DECODE_ERROR 2
static uint8_t ram[65536];
static uint64_t read_fault,write_fault;
static bool partial_write;
static bool allow_high_spare;
static uint8_t high_spare[12];
static uint64_t final_write_address;
static unsigned io_errors;
static bool mapped=true;
static unsigned loads,writes,read_failures,write_failures;
static uint32_t loaded_page;
typedef struct {
 uint32_t reg_pages_in_addr,reg_cs_buf_addr,reg_num_pages;
 uint32_t reg_pages_out_addr,reg_page_spare_out_addr;
 uint8_t page_buffer[4096],page_spare_buffer[64];
 int before_read;
} IPodTouchFMSSState;
static void notifier_list_notify(void*n,void*s){(void)n;(void)s;}
static unsigned checked_read(uint64_t addr,void*p,size_t n){
 if(addr==read_fault||addr>sizeof(ram)||n>sizeof(ram)-addr){memset(p,0xff,n);read_failures++;return MEMTX_DECODE_ERROR;}
 memcpy(p,ram+addr,n);return MEMTX_OK;
}
static unsigned checked_write(uint64_t addr,const void*p,size_t n){
 final_write_address=addr;
 if(allow_high_spare&&addr==0xfffffff4ULL&&n==12){memcpy(high_spare,p,n);writes++;return MEMTX_OK;}
 if(addr==write_fault||addr>sizeof(ram)||n>sizeof(ram)-addr){if(partial_write&&addr<sizeof(ram)&&n>8)memcpy(ram+addr,p,8);write_failures++;return MEMTX_DECODE_ERROR;}
 memcpy(ram+addr,p,n);writes++;return MEMTX_OK;
}
/* Match existing void physical-memory API discarding MemTxResult. */
static void cpu_physical_memory_read(uint64_t addr,void*p,size_t n){(void)checked_read(addr,p,n);}
static void cpu_physical_memory_write(uint64_t addr,const void*p,size_t n){(void)checked_write(addr,p,n);}
static void fmss_load_page(IPodTouchFMSSState*s,uint32_t cs,uint32_t page,uint8_t*data,uint8_t*spare){
 (void)s;assert(cs==0);loads++;loaded_page=page;memset(data,0xa3,4096);memset(spare,0xb4,64);
}
#define MEMTXATTRS_UNSPECIFIED 0
static int address_space_memory;
static unsigned address_space_read(void*space,uint64_t addr,int attrs,void*p,size_t n){return checked_read(addr,p,n);}
static unsigned address_space_write(void*space,uint64_t addr,int attrs,const void*p,size_t n){return checked_write(addr,p,n);}
static uint32_t ldl_le_p(const void*p){const uint8_t*b=p;return b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;}
typedef struct {void*mr;size_t size;} MemoryRegionSection;
static void *get_system_memory(void){return NULL;}
static MemoryRegionSection memory_region_find(void*m,uint64_t addr,size_t n){return (MemoryRegionSection){mapped?(void*)1:NULL,n};}
static bool memory_region_is_ram(void*m){return true;}
static bool int128_eq(size_t a,size_t b){return a==b;}
static size_t int128_make64(size_t a){return a;}
static void memory_region_unref(void*m){}
static bool fmss_io_error(const char *s,int e){io_errors++;return false;}
static void word(unsigned addr,uint32_t value){for(unsigned i=0;i<4;i++)ram[addr+i]=value>>(8*i);}
static IPodTouchFMSSState setup(void){
 memset(ram,0xcc,sizeof(ram));loads=writes=read_failures=write_failures=0;loaded_page=0;
 read_fault=write_fault=UINT64_MAX;partial_write=false;io_errors=0;mapped=true;allow_high_spare=false;memset(high_spare,0xcc,12);final_write_address=UINT64_MAX;word(0x100,37);word(0x104,38);word(0x200,1);word(0x204,1);
 word(0x300,0x1000);word(0x304,0x2000);word(0x308,0x4000);word(0x30c,0x5000);
 return(IPodTouchFMSSState){.reg_pages_in_addr=0x100,.reg_cs_buf_addr=0x200,.reg_num_pages=2,.reg_pages_out_addr=0x300,.reg_page_spare_out_addr=0x6000};
}
#include "slice.h"
static int run_case(const char *c){
 setbuf(stdout,NULL);IPodTouchFMSSState state=setup();
 if(!strcmp(c,"page-descriptor")){
  read_fault=0x100;read_nand_pages(&state);
  printf("page-descriptor fault=%u loads=%u lastpage=%u writes=%u destination=%02x spare=%02x\n",read_failures,loads,loaded_page,writes,ram[0x1000],ram[0x6000]);
  assert(read_failures==1);assert(loads==0); /* Expected bounded failure before NAND/output effects. */
 }else if(!strcmp(c,"data-write")){
  write_fault=0x2000;read_nand_pages(&state);
  printf("data-write fault=%u loads=%u validwrites=%u priorhalf=%02x laterpage=%02x spare=%02x\n",write_failures,loads,writes,ram[0x1000],ram[0x4000],ram[0x6000]);
  assert(write_failures==1);assert(loads==1);assert(writes==1);assert(ram[0x1000]==0xa3&&ram[0x4000]==0xcc&&ram[0x6000]==0xcc);
 }else if(!strcmp(c,"output-descriptor")){
  read_fault=0x304;read_nand_pages(&state);
  printf("output-descriptor readfault=%u writefault=%u loads=%u validwrites=%u laterpage=%02x\n",read_failures,write_failures,loads,writes,ram[0x4000]);
  assert(read_failures==1);assert(write_failures==0);assert(loads==1&&writes==1);assert(ram[0x2000]==0xcc&&ram[0x4000]==0xcc&&ram[0x6000]==0xcc);
 }else if(!strcmp(c,"spare-write")){
  write_fault=0x6000;read_nand_pages(&state);
  printf("spare-write fault=%u loads=%u validwrites=%u laterpage=%02x\n",write_failures,loads,writes,ram[0x4000]);
  assert(write_failures==1);assert(loads==1);assert(writes==2&&ram[0x4000]==0xcc);
 }else if(!strcmp(c,"cs-descriptor")){
  read_fault=0x200;read_nand_pages(&state);assert(read_failures==1&&loads==0&&writes==0);
 }else if(!strcmp(c,"later-descriptor")){
  read_fault=0x104;read_nand_pages(&state);assert(read_failures==1&&loads==1&&writes==3);assert(ram[0x1000]==0xa3&&ram[0x6000]==0xb4&&ram[0x4000]==0xcc);
 }else if(!strcmp(c,"partial-data")){
  write_fault=0x2000;partial_write=true;read_nand_pages(&state);assert(write_failures==1&&loads==1&&writes==1);assert(ram[0x1000]==0xa3&&ram[0x2000]==0xa3&&ram[0x2007]==0xa3&&ram[0x2008]==0xcc&&ram[0x4000]==0xcc&&ram[0x6000]==0xcc);
 }else if(!strcmp(c,"partial-spare")){
  write_fault=0x6000;partial_write=true;read_nand_pages(&state);assert(write_failures==1&&loads==1&&writes==2);assert(ram[0x6000]==0xb4&&ram[0x6007]==0xb4&&ram[0x6008]==0xcc&&ram[0x4000]==0xcc);
 }else if(!strcmp(c,"write-source")){
  uint8_t data[16]={0};read_fault=0x1000;assert(!fmss_write_dma_read(0x1000,data,16));assert(io_errors==1&&read_failures==1); /* RAM span exists but transaction fails. */
 }else if(!strcmp(c,"unmapped-source")){
  uint8_t data[16]={0};mapped=false;assert(!fmss_write_dma_read(0x1000,data,16));assert(io_errors==1&&read_failures==0); /* Preserve RAM-only source guard. */
 }else if(!strcmp(c,"spare-address-wrap")){
  /* Preserve existing expression width; synthetic high mapping tests current
   * CPU code arithmetic only, not physical bus/address-generator behavior. */
  state.reg_page_spare_out_addr=0xfffffff4u;allow_high_spare=true;
  read_nand_pages(&state);assert(loads==2&&writes==6&&write_failures==0);
  assert(high_spare[0]==0xb4&&high_spare[11]==0xb4);
  assert(final_write_address==0&&ram[0]==0xb4&&ram[11]==0xb4&&ram[12]==0xcc);
 }else if(!strcmp(c,"success")){
  read_nand_pages(&state);assert(loads==2&&writes==6&&read_failures==0&&write_failures==0);assert(loaded_page==38&&ram[0x1000]==0xa3&&ram[0x2000]==0xa3&&ram[0x4000]==0xa3&&ram[0x5000]==0xa3&&ram[0x6000]==0xb4&&ram[0x600c]==0xb4);
  uint8_t data[16]={0};assert(fmss_write_dma_read(0x1000,data,16));assert(data[0]==0xa3&&io_errors==0);
 }else{
  assert(false);
 }
 printf("PASS %s\n",c);fflush(stdout);return 0;
}
int main(void){
 const char *cases[]={"success","page-descriptor","cs-descriptor","data-write","output-descriptor","spare-write","later-descriptor","partial-data","partial-spare","write-source","unmapped-source","spare-address-wrap"};
 for(unsigned i=0;i<sizeof cases/sizeof*cases;i++) run_case(cases[i]);
 return 0;
}
