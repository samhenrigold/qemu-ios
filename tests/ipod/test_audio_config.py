#!/usr/bin/env python3
"""Audio machine-option precedence, validation and legacy default preservation."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'hw/arm/ipod_touch_2g.c').read_text()
a=source.index('static bool ipod_touch_audio_hw_enabled(')
b=source.index('\n/*',source.index('static void ipod_touch_audio_env_alias(',a))
functions=source[a:b]
code=r'''
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
typedef enum {ON_OFF_AUTO_AUTO,ON_OFF_AUTO_ON,ON_OFF_AUTO_OFF} OnOffAuto;
typedef struct {OnOffAuto audio_hw;bool audio_hw_explicit;void *cpu;} IPodTouchMachineState;
typedef IPodTouchMachineState Object;
typedef int Error;
typedef struct {bool valid;OnOffAuto value;} Visitor;
#define IPOD_TOUCH_MACHINE(o) (o)
static int warnings;
#define warn_report_once(...) (warnings++)
#define error_setg(errp,...) (**(errp)=1)
static bool visit_type_OnOffAuto(Visitor*v,const char*n,OnOffAuto*p,Error**e) {
 if(!v->valid){**e=1;return false;}*p=v->value;return true;
}
'''+functions+r'''
int main(void) {
 int error=0;Error *ep=&error;
 unsetenv("IT_AUDIO_HW");unsetenv("IT_DIRECT_IBOOT");
 IPodTouchMachineState m={.audio_hw=ON_OFF_AUTO_AUTO};
 ipod_touch_audio_env_alias(&m);assert(!ipod_touch_audio_hw_enabled(&m));
 setenv("IT_DIRECT_IBOOT","iBoot.bin",1);assert(ipod_touch_audio_hw_enabled(&m));
 setenv("IT_AUDIO_HW","0",1);ipod_touch_audio_env_alias(&m);assert(!ipod_touch_audio_hw_enabled(&m));
 Visitor on={true,ON_OFF_AUTO_ON};ipod_touch_set_audio_hw(&m,&on,"audio-hw",NULL,&ep);
 ipod_touch_audio_env_alias(&m);assert(ipod_touch_audio_hw_enabled(&m));
 Visitor automatic={true,ON_OFF_AUTO_AUTO};ipod_touch_set_audio_hw(&m,&automatic,"audio-hw",NULL,&ep);
 ipod_touch_audio_env_alias(&m);assert(m.audio_hw==ON_OFF_AUTO_AUTO && ipod_touch_audio_hw_enabled(&m));
 unsetenv("IT_DIRECT_IBOOT");assert(!ipod_touch_audio_hw_enabled(&m));
 Visitor invalid={false,ON_OFF_AUTO_ON};ipod_touch_set_audio_hw(&m,&invalid,"audio-hw",NULL,&ep);
 assert(error && m.audio_hw==ON_OFF_AUTO_AUTO);error=0;
 m.cpu=&m;ipod_touch_set_audio_hw(&m,&on,"audio-hw",NULL,&ep);
 assert(error && m.audio_hw==ON_OFF_AUTO_AUTO);
 m=(IPodTouchMachineState){.audio_hw=ON_OFF_AUTO_AUTO};setenv("IT_AUDIO_HW","",1);
 ipod_touch_audio_env_alias(&m);assert(ipod_touch_audio_hw_enabled(&m));
 assert(warnings==2);
 puts("PASS: audio auto defaults, explicit option precedence, legacy aliases, invalid and runtime rejection");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 tmp=Path(tmp);(tmp/'check.c').write_text(code)
 subprocess.run(['cc','-fsanitize=address,undefined',str(tmp/'check.c'),'-o',str(tmp/'check')],check=True)
 subprocess.run([str(tmp/'check')],check=True)
