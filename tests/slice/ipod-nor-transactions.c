/* Production NOR command checks: GPIO framing, protection, programming and erase.
 *
 * SLICE include/hw/arm/ipod_touch_nor_spi.h define NOR_
 * SLICE hw/arm/ipod_touch_nor_spi.c define NOR_STATUS
 * SLICE include/hw/arm/ipod_touch_nor_spi.h typedef IPodTouchNORSPIState
 * SLICE:fns hw/arm/ipod_touch_nor_spi.c fn nor_persist
 * SLICE:fns hw/arm/ipod_touch_gpio.c fn s5l8900_gpio_write
 * SLICE:fns hw/arm/ipod_touch_nor_spi.c fn nor_reset_transaction nor_finish_transaction ipod_touch_nor_spi_set_cs ipod_touch_nor_spi_reset ipod_touch_nor_spi_transfer ipod_touch_nor_spi_post_load
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef void BlockBackend;
#define NUM_GPIO_PADS 16
#define IPOD_TOUCH_GPIO(s) (s)
#define IPOD_TOUCH_NOR_SPI(s) ((IPodTouchNORSPIState *)(s))
#define trace_ipod_touch_gpio_write(...) ((void)0)
#define trace_ipod_touch_nor_command(...) ((void)0)
#define trace_ipod_touch_nor_finish(...) ((void)0)
typedef uint64_t hwaddr;
typedef struct { int unused; } SSIPeripheral;
typedef SSIPeripheral DeviceState;
#include "slice.h"
typedef struct IPodTouchGPIOState {uint32_t gpio_state[16];int outputs[128];uint32_t fsel_offset;} IPodTouchGPIOState;
static int levels[128];
static void qemu_set_irq(int pin, int level) {assert(pin>=0 && pin<128);levels[pin]=level;}
static void initialize_nor(IPodTouchNORSPIState*s) {
 memset(s->nor_data,0xff,sizeof(s->nor_data));s->nor_size=NOR_FLASH_SIZE;s->nor_initialized=true;
}

#define BDRV_REQ_FUA 1
#define RUN_STATE_IO_ERROR 1
#define qatomic_set(p,v) (*(p)=(v))
#define error_report(...) ((void)0)
static bool nor_io_failed;
static int persist_error, storage_stops, full_writes;
static uint32_t persisted_start, persisted_length;
static int blk_pwrite(void *blk,uint32_t start,uint32_t length,const void*data,int flags) {
 assert(blk && flags==BDRV_REQ_FUA);persisted_start=start;persisted_length=length;if(length==NOR_FLASH_SIZE)full_writes++;return persist_error;
}
static void qemu_system_vmstop_request(int state) {assert(state==RUN_STATE_IO_ERROR);storage_stops++;}
#include "fns.h"

static IPodTouchNORSPIState s;
static void begin(uint8_t cmd) {ipod_touch_nor_spi_set_cs(&s.ssidev,false);ipod_touch_nor_spi_transfer(&s.ssidev,cmd);}
static uint8_t byte(uint8_t value) {return ipod_touch_nor_spi_transfer(&s.ssidev,value);}
static void end(void) {ipod_touch_nor_spi_set_cs(&s.ssidev,true);}
static void address(uint32_t value) {byte(value>>16);byte(value>>8);byte(value);}
static void enable(void) {begin(NOR_ENABLE_WRITE);end();assert(s.write_enabled);}
static void status(uint8_t value) {enable();begin(NOR_WRITE_TO_STATUS_REG);byte(value);end();}
static void program(uint32_t where, uint8_t value) {enable();begin(NOR_WRITE_DATA_CMD);address(where);byte(value);end();}
int main(void) {
 IPodTouchGPIOState g={.fsel_offset=0x1e0};for(int i=0;i<128;i++)g.outputs[i]=i;
 s5l8900_gpio_write(&g,0x1e0,0x0000000f,4);assert(levels[0]==1 && g.gpio_state[0]==1);
 s5l8900_gpio_write(&g,0x1e0,0x0000000e,4);assert(levels[0]==0 && g.gpio_state[0]==0);
 s5l8900_gpio_write(&g,0x1e0,0x000f070f,4);assert(levels[127]==1 && g.gpio_state[15]==128);
 s5l8900_gpio_write(&g,0x1e0,0x0010000f,4);s5l8900_gpio_write(&g,0x1e0,0x0000080f,4);
 s5l8900_gpio_write(&g,0x1e0,0x0000000b,4);s5l8900_gpio_write(&g,0,0x0000000f,4);
 assert(g.gpio_state[0]==0);
 initialize_nor(&s);ipod_touch_nor_spi_reset(&s.ssidev);
 begin(NOR_GET_STATUS_CMD);assert(byte(0xff)==0x1c);assert(byte(0xff)==0);assert(byte(0xff)==0x1c);end();
 program(0,0);assert(s.nor_data[0]==0xff && !s.write_enabled); /* Protected. */
 status(0);assert(!(s.status & NOR_STATUS_SWP));
 begin(NOR_WRITE_DATA_CMD);address(0);byte(0);end();assert(s.nor_data[0]==0xff); /* No WREN. */
 enable();begin(NOR_WRITE_DATA_CMD);address(0x1fe);byte(0xf0);byte(0x0f);byte(0xaa);
 assert(s.nor_data[0x1fe]==0xff);end(); /* CS commits the complete page buffer. */
 assert(s.nor_data[0x1fe]==0xf0 && s.nor_data[0x1ff]==0x0f && s.nor_data[0x100]==0xaa);
 assert(s.nor_data[0x200]==0xff);program(0x1fe,0xff);assert(s.nor_data[0x1fe]==0xf0);
 enable();begin(NOR_WRITE_DATA_CMD);address(0x200);
 for(int i=0;i<256;i++)byte(0);byte(0xff);end();
 assert(s.nor_data[0x200]==0xff && s.nor_data[0x201]==0); /* Last buffer value wins. */
 for(int i=0;i<3;i++) {
  uint8_t cmd[]={NOR_ERASE_BLOCK,NOR_ERASE_32K,NOR_ERASE_64K};
  uint32_t size[]={4096,32768,65536};uint32_t start=0x2abcd & ~(size[i]-1);
  memset(s.nor_data,0,sizeof(s.nor_data));enable();begin(cmd[i]);address(0x2abcd);end();
  assert(s.nor_data[start-1]==0 && s.nor_data[start]==0xff && s.nor_data[start+size[i]-1]==0xff && s.nor_data[start+size[i]]==0);
 }
 status(0xff);assert((s.status & (NOR_STATUS_SPRL|NOR_STATUS_SWP))==(NOR_STATUS_SPRL|NOR_STATUS_SWP));
 status(0);assert(s.status & NOR_STATUS_SWP);status(0);assert(!(s.status & NOR_STATUS_SWP));
 enable();begin(NOR_WRITE_DATA_CMD);byte(0);end();assert(!s.write_enabled && !s.command_active);
 s.nor_data[0]=0x10;s.nor_data[1]=0x20;s.nor_data[NOR_FLASH_SIZE-1]=0x40;
 begin(NOR_READ_DATA_CMD);address(NOR_FLASH_SIZE-1);
 assert(byte(NOR_GET_JEDECID)==0x40 && byte(NOR_ENABLE_WRITE)==0x10 && byte(0xff)==0x20);end();
 begin(NOR_GET_JEDECID);assert(byte(0xff)==0x1f && byte(0xff)==0x45 && byte(0xff)==0x02 && byte(0xff)==0 && byte(0xff)==0xff);end();
 begin(0);byte(NOR_ENABLE_WRITE);end();assert(!s.write_enabled);
 s.blk=(void*)1;persist_error=-EIO;program(4,0);
 assert((s.status & NOR_STATUS_EPE) && nor_io_failed && storage_stops==1);
 persist_error=0;s.restore_pending=true;program(4,0);
 assert(!(s.status & NOR_STATUS_EPE) && !s.restore_pending && nor_io_failed);
 assert(full_writes==1 && persisted_start==0 && persisted_length==NOR_PAGE_SIZE);s.blk=NULL;
 s.nor_size=0;program(0,0);assert((s.status & NOR_STATUS_EPE) && s.nor_data[0]==0x10);s.nor_size=NOR_FLASH_SIZE;
 program(0,0);assert(!(s.status & NOR_STATUS_EPE) && s.nor_data[0]==0);
 assert(ipod_touch_nor_spi_post_load(&s,2)==0);
 s.data_count=257;assert(ipod_touch_nor_spi_post_load(&s,2)==-EINVAL);s.data_count=0;
 s.nor_read_ind=NOR_FLASH_SIZE;assert(ipod_touch_nor_spi_post_load(&s,2)==-EINVAL);
 s.nor_read_ind=0;s.nor_initialized=false;assert(ipod_touch_nor_spi_post_load(&s,2)==-EINVAL);
 puts("PASS: GPIO/CS, status/protection, page wrap/AND, erase sizes, read wrap and snapshot bounds");
}
