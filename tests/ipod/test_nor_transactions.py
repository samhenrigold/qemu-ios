#!/usr/bin/env python3
"""GPIO-selected NOR reads do not infer command boundaries from data bytes."""
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
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define NUM_GPIO_PADS 16
#define IPOD_TOUCH_GPIO(s) (s)
#define IPOD_TOUCH_NOR_SPI(s) (s)
#define LOG_GUEST_ERROR 0
#define qemu_log_mask(...) ((void)0)
#define trace_ipod_touch_gpio_write(...) ((void)0)
#define trace_ipod_touch_nor_command(...) ((void)0)
typedef uint64_t hwaddr;
typedef struct {
 uint32_t cur_cmd,in_buf_size,out_buf_size,in_buf_cur_ind,out_buf_cur_ind;
 uint8_t in_buf[4096],out_buf[4096],write_enabled,*nor_data;
 size_t nor_size; uint32_t nor_read_ind;bool nor_initialized;
} IPodTouchNORSPIState;
typedef IPodTouchNORSPIState SSIPeripheral;
typedef IPodTouchNORSPIState DeviceState;
typedef struct IPodTouchGPIOState {uint32_t gpio_state[16];int outputs[128];} IPodTouchGPIOState;
static int levels[128];
static void qemu_set_irq(int pin, int level) {assert(pin>=0 && pin<128);levels[pin]=level;}
static void initialize_nor(IPodTouchNORSPIState*s) {assert(0);}
'''
code += '\n'.join(re.findall(r'^#define NOR_.*$',header,re.M))+'\n'
code += function(gpio,'s5l8900_gpio_write')
for name in ['nor_reset_transaction','ipod_touch_nor_spi_set_cs','ipod_touch_nor_spi_reset','ipod_touch_nor_spi_transfer']:
    code += function(nor,name)
code += r'''
int main(void) {
 IPodTouchGPIOState g={0};for(int i=0;i<128;i++)g.outputs[i]=i;
 s5l8900_gpio_write(&g,0x1e0,0x0000000f,4);assert(levels[0]==1 && g.gpio_state[0]==1);
 s5l8900_gpio_write(&g,0x1e0,0x0000000e,4);assert(levels[0]==0 && g.gpio_state[0]==0);
 s5l8900_gpio_write(&g,0x1e0,0x000f070f,4);assert(levels[127]==1 && g.gpio_state[15]==128);
 s5l8900_gpio_write(&g,0x1e0,0x0010000f,4);
 s5l8900_gpio_write(&g,0x1e0,0x0000080f,4);
 s5l8900_gpio_write(&g,0x1e0,0x0000000b,4);
 s5l8900_gpio_write(&g,0,0x0000000f,4);assert(g.gpio_state[0]==0);
 uint8_t bytes[]={0x10,0x20,0x30,0x40};
 IPodTouchNORSPIState s={.nor_data=bytes,.nor_size=sizeof(bytes),.nor_initialized=true};
 ipod_touch_nor_spi_set_cs(&s,false);
 ipod_touch_nor_spi_transfer(&s,NOR_READ_DATA_CMD);
 for(int i=0;i<3;i++)ipod_touch_nor_spi_transfer(&s,0);
 /* MOSI values are irrelevant while reading, even if they look like commands. */
 assert(ipod_touch_nor_spi_transfer(&s,NOR_GET_JEDECID)==0x10);
 assert(ipod_touch_nor_spi_transfer(&s,NOR_ENABLE_WRITE)==0x20);
 assert(ipod_touch_nor_spi_transfer(&s,0xff)==0x30);
 ipod_touch_nor_spi_set_cs(&s,true);assert(s.cur_cmd==0);
 ipod_touch_nor_spi_set_cs(&s,false);
 ipod_touch_nor_spi_transfer(&s,NOR_GET_JEDECID);
 assert(ipod_touch_nor_spi_transfer(&s,0xff)==0x1f);
 assert(ipod_touch_nor_spi_transfer(&s,0xff)==0x45);
 assert(ipod_touch_nor_spi_transfer(&s,0xff)==0x02);
 ipod_touch_nor_spi_set_cs(&s,true);
 ipod_touch_nor_spi_transfer(&s,NOR_READ_DATA_CMD);
 ipod_touch_nor_spi_transfer(&s,0); /* An interrupted address must not leak. */
 ipod_touch_nor_spi_set_cs(&s,true);assert(!s.cur_cmd && !s.in_buf_cur_ind);
 s.write_enabled=1;ipod_touch_nor_spi_reset(&s);assert(!s.write_enabled);
 puts("PASS: FSEL bounds/output levels, CS framing, read data and interrupted commands");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    tmp=Path(tmp);(tmp/'check.c').write_text(code)
    subprocess.run(['cc','-fsanitize=address,undefined',str(tmp/'check.c'),'-o',str(tmp/'check')],check=True)
    subprocess.run([str(tmp/'check')],check=True)
