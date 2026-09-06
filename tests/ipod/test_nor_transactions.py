#!/usr/bin/env python3
"""Production NOR command checks: GPIO framing, protection, programming and erase."""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
nor = (root/'hw/arm/ipod_touch_nor_spi.c').read_text()
gpio = (root/'hw/arm/ipod_touch_gpio.c').read_text()
header = (root/'include/hw/arm/ipod_touch_nor_spi.h').read_text()
def function(source, name):
    m = re.search(r'^static [^\n]*\b'+name+r'\([^)]*\)\s*\{.*?^}',source,re.M|re.S)
    assert m, name
    return m[0]+'\n'
definitions = '\n'.join(re.findall(r'^#define NOR_.*$',header,re.M))+'\n'
definitions += '\n'.join(re.findall(r'^#define NOR_STATUS.*$',nor,re.M))+'\n'
state = re.search(r'typedef struct IPodTouchNORSPIState \{.*?} IPodTouchNORSPIState;',header,re.S)[0]
state = state.replace('    SSIPeripheral ssidev;\n','')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define NUM_GPIO_PADS 16
#define IPOD_TOUCH_GPIO(s) (s)
#define IPOD_TOUCH_NOR_SPI(s) (s)
#define trace_ipod_touch_gpio_write(...) ((void)0)
#define trace_ipod_touch_nor_command(...) ((void)0)
#define trace_ipod_touch_nor_finish(...) ((void)0)
typedef uint64_t hwaddr;
''' + definitions + state + r'''
typedef IPodTouchNORSPIState SSIPeripheral;
typedef IPodTouchNORSPIState DeviceState;
typedef struct IPodTouchGPIOState {uint32_t gpio_state[16];int outputs[128];} IPodTouchGPIOState;
static int levels[128];
static void qemu_set_irq(int pin, int level) {assert(pin>=0 && pin<128);levels[pin]=level;}
static void initialize_nor(IPodTouchNORSPIState*s) {
 memset(s->nor_data,0xff,sizeof(s->nor_data));s->nor_size=NOR_FLASH_SIZE;s->nor_initialized=true;
}
'''
code += function(gpio,'s5l8900_gpio_write')
for name in ['nor_reset_transaction','nor_finish_transaction','ipod_touch_nor_spi_set_cs',
             'ipod_touch_nor_spi_reset','ipod_touch_nor_spi_transfer','ipod_touch_nor_spi_post_load']:
    code += function(nor,name)
code += r'''
static IPodTouchNORSPIState s;
static void begin(uint8_t cmd) {ipod_touch_nor_spi_set_cs(&s,false);ipod_touch_nor_spi_transfer(&s,cmd);}
static uint8_t byte(uint8_t value) {return ipod_touch_nor_spi_transfer(&s,value);}
static void end(void) {ipod_touch_nor_spi_set_cs(&s,true);}
static void address(uint32_t value) {byte(value>>16);byte(value>>8);byte(value);}
static void enable(void) {begin(NOR_ENABLE_WRITE);end();assert(s.write_enabled);}
static void status(uint8_t value) {enable();begin(NOR_WRITE_TO_STATUS_REG);byte(value);end();}
static void program(uint32_t where, uint8_t value) {enable();begin(NOR_WRITE_DATA_CMD);address(where);byte(value);end();}
int main(void) {
 IPodTouchGPIOState g={0};for(int i=0;i<128;i++)g.outputs[i]=i;
 s5l8900_gpio_write(&g,0x1e0,0x0000000f,4);assert(levels[0]==1 && g.gpio_state[0]==1);
 s5l8900_gpio_write(&g,0x1e0,0x0000000e,4);assert(levels[0]==0 && g.gpio_state[0]==0);
 s5l8900_gpio_write(&g,0x1e0,0x000f070f,4);assert(levels[127]==1 && g.gpio_state[15]==128);
 s5l8900_gpio_write(&g,0x1e0,0x0010000f,4);s5l8900_gpio_write(&g,0x1e0,0x0000080f,4);
 s5l8900_gpio_write(&g,0x1e0,0x0000000b,4);s5l8900_gpio_write(&g,0,0x0000000f,4);
 assert(g.gpio_state[0]==0);
 initialize_nor(&s);ipod_touch_nor_spi_reset(&s);
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
 s.nor_size=0;program(0,0);assert((s.status & NOR_STATUS_EPE) && s.nor_data[0]==0x10);s.nor_size=NOR_FLASH_SIZE;
 program(0,0);assert(!(s.status & NOR_STATUS_EPE) && s.nor_data[0]==0);
 assert(ipod_touch_nor_spi_post_load(&s,2)==0);
 s.data_count=257;assert(ipod_touch_nor_spi_post_load(&s,2)==-EINVAL);s.data_count=0;
 s.nor_read_ind=NOR_FLASH_SIZE;assert(ipod_touch_nor_spi_post_load(&s,2)==-EINVAL);
 s.nor_read_ind=0;s.nor_initialized=false;assert(ipod_touch_nor_spi_post_load(&s,2)==-EINVAL);
 puts("PASS: GPIO/CS, status/protection, page wrap/AND, erase sizes, read wrap and snapshot bounds");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    tmp=Path(tmp);(tmp/'check.c').write_text(code)
    subprocess.run(['cc','-fsanitize=address,undefined',str(tmp/'check.c'),'-o',str(tmp/'check')],check=True)
    subprocess.run([str(tmp/'check')],check=True)
