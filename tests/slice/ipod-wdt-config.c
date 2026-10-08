/* Production watchdog-option precedence and startup immutability.
 *
 * SLICE hw/arm/ipod_touch_2g.c range static void ipod_touch_get_wdt_noreset( | static void ipod_touch_get_osk(
 */
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
typedef struct {bool wdt_noreset, wdt_noreset_explicit; void *cpu;} IPodTouchMachineState;
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
#include "slice.h"

int main(void) {
    IPodTouchMachineState m={0};
    int error=0;Error *ep=&error;
    unsetenv("IT_WDT_NORESET");ipod_touch_wdt_env_alias(&m);assert(!m.wdt_noreset);
    setenv("IT_WDT_NORESET","0",1);ipod_touch_wdt_env_alias(&m);assert(m.wdt_noreset);
    Visitor off={true,false}, on={true,true}, bad={false,true};
    ipod_touch_set_wdt_noreset(&m,&off,"wdt-noreset",NULL,&ep);
    ipod_touch_wdt_env_alias(&m);assert(!m.wdt_noreset && m.wdt_noreset_explicit);
    ipod_touch_set_wdt_noreset(&m,&bad,"wdt-noreset",NULL,&ep);assert(error && !m.wdt_noreset);
    error=0;ipod_touch_set_wdt_noreset(&m,&on,"wdt-noreset",NULL,&ep);assert(m.wdt_noreset);
    m.cpu=&m;ipod_touch_set_wdt_noreset(&m,&off,"wdt-noreset",NULL,&ep);
    assert(error && m.wdt_noreset);
    m=(IPodTouchMachineState){0};setenv("IT_WDT_NORESET","",1);
    ipod_touch_wdt_env_alias(&m);assert(m.wdt_noreset && warnings==2);
    puts("PASS: watchdog defaults, alias presence, explicit precedence and runtime rejection");
}
