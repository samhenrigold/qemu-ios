/* CLCD mask, pending status, W1C and migration compatibility under sanitizers.
 *
 * SLICE hw/arm/ipod_touch_lcd.c range static void lcd_update_irq( | static uint64_t ipod_touch_lcd_read(
 */
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
typedef struct { int irq; uint32_t irq_enable,irq_status,render,plane_regs[0xc0]; } IPodTouchLCDState;
static bool raised;
static void qemu_set_irq(int irq,int level) { raised=level; }
#include "slice.h"
int main(void) {
 IPodTouchLCDState s={0};
 lcd_vblank_irq(&s);assert(s.irq_status==1 && !raised);
 lcd_write_irq(&s,8,0x701);assert(raised);
 lcd_write_irq(&s,12,1);assert(!raised && !s.irq_status && s.irq_enable==0x701);
 lcd_vblank_irq(&s);assert(raised);
 lcd_write_irq(&s,8,0x700);assert(!raised && s.irq_status==1);
 for(int i=0;i<600;i++)lcd_vblank_irq(&s);
 assert(!raised && s.irq_status==1);
 lcd_write_irq(&s,12,0);assert(s.irq_status==1);
 s.irq_status=0x701;lcd_write_irq(&s,12,0x100);assert(s.irq_status==0x601 && raised);
 lcd_write_irq(&s,12,0x600);assert(s.irq_status==1 && !raised);
 lcd_write_irq(&s,8,1);assert(raised);
 lcd_restore_irq(&s,3);assert(raised && s.irq_status==1 && s.irq_enable==1);
 s.plane_regs[2]=0x700;lcd_restore_irq(&s,2);
 assert(!raised && !s.irq_status && s.irq_enable==0x700);
 s.render=1;lcd_restore_irq(&s,1);assert(s.irq_enable==1 && !raised);
 lcd_vblank_irq(&s);assert(raised);
 s.render=0xff;lcd_restore_irq(&s,1);assert(!s.irq_enable && !raised);
 puts("PASS: CLCD masked vblank, pending delivery, W1C and legacy/current restore");
}
