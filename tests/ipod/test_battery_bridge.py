#!/usr/bin/env python3
"""App-thread battery updates are validated and marshalled onto the emulator thread."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'contrib/macos-app/qemu-macos-extras.c').read_text()
functions=source[source.index('struct battery_input {'):source.index('static void paste_bh(')]
code=r'''
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef int Object;
typedef int Error;
#define OBJECT(x) (x)
#define g_new(type,n) ((type *)calloc(n,sizeof(type)))
#define g_free free
static bool ready;
static int level=-1,updates;
static char charging[8];
static void (*pending)(void *);
static void *argument;
static bool qemu_ios_ui_ready(void) { return ready; }
static Object *qdev_get_machine(void) { return (Object *)1; }
static void *qemu_get_aio_context(void) { return (void *)2; }
static void aio_bh_schedule_oneshot(void *ctx,void (*fn)(void *),void *arg)
{ assert(ctx==(void *)2 && !pending);pending=fn;argument=arg; }
static void object_property_set_int(Object *obj,const char *name,int value,Error **err)
{ assert(obj==(Object *)1 && !strcmp(name,"battery-level"));level=value;updates++; }
static void object_property_set_str(Object *obj,const char *name,const char *value,Error **err)
{ assert(obj==(Object *)1 && !strcmp(name,"battery-charging"));strcpy(charging,value);updates++; }
static const char *error_get_pretty(Error *error) { return "error"; }
static void error_free(Error *error) {}
'''+functions+r'''
int main(void) {
 assert(!qemu_ios_ui_battery(60,0) && !pending);
 ready=true;
 assert(!qemu_ios_ui_battery(-1,0));assert(!qemu_ios_ui_battery(101,0));
 assert(!qemu_ios_ui_battery(60,-1));assert(!qemu_ios_ui_battery(60,3));
 assert(!pending && !updates);
 const char *modes[]={"auto","on","off"};
 for(int mode=0;mode<3;mode++) {
  assert(qemu_ios_ui_battery(mode*50,mode));assert(updates==mode*2);
  assert(pending);pending(argument);pending=NULL;
  assert(level==mode*50 && !strcmp(charging,modes[mode]));
 }
 puts("PASS: battery bridge readiness, bounds, queued delivery and charging modes");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 path=Path(tmp)/'check.c';path.write_text(code)
 subprocess.run(['clang','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(path),'-o',tmp+'/check'],check=True)
 subprocess.run([tmp+'/check'],check=True)
