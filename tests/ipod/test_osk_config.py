#!/usr/bin/env python3
"""Production keyboard-option precedence and startup immutability."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
s = (root / "hw/arm/ipod_touch_2g.c").read_text()
functions = s[s.index("static void ipod_touch_get_osk("):s.index("/*\n * Audio hardware")]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
typedef struct {bool osk_enabled, osk_explicit; void *cpu;} IPodTouchMachineState;
typedef IPodTouchMachineState Object;
typedef int Error;
typedef struct {bool valid, value;} Visitor;
#define IPOD_TOUCH_MACHINE(o) (o)
#define error_setg(e,...) (**(e)=1)
static int warnings;
#define warn_report_once(...) (warnings++)
static bool visit_type_bool(Visitor *v, const char *n, bool *p, Error **e) {
    if (!v->valid) {**e=1;return false;} *p=v->value;return true;
}
''' + functions + r'''
int main(void) {
    IPodTouchMachineState m={0};
    int error=0;Error *ep=&error;
    unsetenv("IT_OSK");ipod_touch_osk_env_alias(&m);assert(!m.osk_enabled);
    setenv("IT_OSK","0",1);ipod_touch_osk_env_alias(&m);assert(m.osk_enabled);
    Visitor off={true,false}, on={true,true}, bad={false,true};
    ipod_touch_set_osk(&m,&off,"osk",NULL,&ep);
    ipod_touch_osk_env_alias(&m);assert(!m.osk_enabled && m.osk_explicit);
    ipod_touch_set_osk(&m,&bad,"osk",NULL,&ep);assert(error && !m.osk_enabled);
    error=0;ipod_touch_set_osk(&m,&on,"osk",NULL,&ep);assert(m.osk_enabled);
    m.cpu=&m;ipod_touch_set_osk(&m,&off,"osk",NULL,&ep);
    assert(error && m.osk_enabled);
    m=(IPodTouchMachineState){0};setenv("IT_OSK","",1);
    ipod_touch_osk_env_alias(&m);assert(m.osk_enabled && warnings==2);
    puts("PASS: keyboard defaults, alias presence, explicit precedence and runtime rejection");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    tmp=Path(tmp);(tmp / "check.c").write_text(code)
    subprocess.run(["cc", "-fsanitize=address,undefined", str(tmp / "check.c"),
                    "-o", str(tmp / "check")], check=True)
    subprocess.run([str(tmp / "check")], check=True)
