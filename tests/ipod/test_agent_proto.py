#!/usr/bin/env python3
"""Exercise the production agent transport with real queues and a guest-memory fixture."""
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
check = r'''
#include "qemu/osdep.h"
#include "hw/arm/ipod-agent.h"
static uint8_t memory[4096];
static bool fail;
static int copy(void *unused, uint32_t addr, uint8_t *data, size_t len, bool write) {
    if (fail || addr > sizeof(memory) || len > sizeof(memory)-addr) return -1;
    if (write) memcpy(memory+addr,data,len); else memcpy(data,memory+addr,len);
    return 0;
}
static int64_t call(IPodAgent *a,unsigned op,uint64_t token,unsigned off,unsigned len,int64_t now) {
    return ipod_agent_call(a,op,token,0,off,len,now,123+now,copy,NULL);
}
int main(void) {
    IPodAgent *a=ipod_agent_new();
    assert(!strcmp(ipod_agent_status(a,0),"absent"));
    assert(call(a,0x161,0,0,0,0)==-1);
    int64_t token=call(a,0x160,0,0,0,0);
    assert(token>0 && call(a,0x160,0,0,0,0)==0);
    assert(call(a,0x161,token+1,0,0,0)==-1);
    assert(!ipod_agent_submit(a,"bad"));
    assert(!ipod_agent_submit(a,"a ping\n%%%"));
    assert(!ipod_agent_submit(a,"a ping\nYQ="));
    uint8_t body[3000];
    for (int i=0;i<3000;i++) body[i]=i;
    char *encoded=g_base64_encode(body,sizeof(body));
    char *request=g_strdup_printf("first exec cat\n%s",encoded);
    assert(ipod_agent_submit(a,request));
    assert(!ipod_agent_submit(a,request));
    int len=call(a,0x161,token,0,0,0);
    assert(len==3015);
    uint8_t recovered[4096];
    for (int off=0;off<len;) {
        int n=call(a,0x162,token,off,1024,0);
        assert(n>0); memcpy(recovered+off,memory,n); off+=n;
    }
    assert(!memcmp(recovered,"first exec cat\n",15));
    assert(!memcmp(recovered+15,body,3000));
    assert(call(a,0x162,token,0,1025,0)==-1);
    fail=true; assert(call(a,0x162,token,0,10,0)==-1); fail=false;
    memcpy(memory,"abc",3);
    assert(call(a,0x163,token,1,3,0)==-1);
    assert(call(a,0x163,token,0,3,0)==3);
    fail=true; assert(call(a,0x163,token,0,3,0)==-1); fail=false;
    assert(call(a,0x163,token,3,3,0)==3);
    assert(call(a,0x163,token,UINT32_MAX,3,0)==-1);
    assert(call(a,0x164,token,7,0,0)==0);
    assert(!ipod_agent_submit(a,request)); /* duplicate finished id */
    char *result=ipod_agent_take_result(a);
    assert(!strcmp(result,"first 7\nYWJjYWJj")); g_free(result);
    assert(call(a,0x161,token,0,0,0)==0);
    assert(ipod_agent_submit(a,"lost exec touch /tmp/test\n"));
    assert(call(a,0x161,token,0,0,0)>0);
    assert(!strcmp(ipod_agent_status(a,10001),"stale"));
    int64_t next=call(a,0x160,0,0,0,10001);
    assert(next>0 && next!=token);
    assert(call(a,0x164,token,0,0,10001)==-1);
    result=ipod_agent_take_result(a);
    char *expected=g_strdup_printf("lost %d\n",-ECONNRESET);
    assert(!strcmp(result,expected)); g_free(result); g_free(expected);
    for (int i=0;i<IT_AGENT_QUEUE_MAX;i++) {
        char *r=g_strdup_printf("%d ping\n",i);
        assert(ipod_agent_submit(a,r));g_free(r);
    }
    assert(!ipod_agent_submit(a,"overflow ping\n"));
    ipod_agent_reset(a);
    assert(call(a,0x161,next,0,0,10002)==-1);
    assert(ipod_agent_submit(a,"fresh ping\n"));
    token=call(a,0x160,0,0,0,10002);
    assert(call(a,0x161,token,0,0,10002)==11);
    for (unsigned i=0;i<IT_AGENT_RESPONSE_MAX/1024;i++) {
        assert(call(a,0x163,token,i*1024,1024,10002)==1024);
    }
    assert(call(a,0x163,token,IT_AGENT_RESPONSE_MAX,1,10002)==-1);
    assert(ipod_agent_cancel(a,"fresh"));
    assert(!ipod_agent_cancel(a,"missing"));
    assert(call(a,0x164,token,0,0,10002)==-1);
    token=call(a,0x160,0,0,0,10003);
    assert(ipod_agent_submit(a,"pending ping\n"));
    assert(ipod_agent_cancel(a,"pending"));
    assert(call(a,0x161,token,0,0,10003)==0);
    assert(ipod_agent_submit(a,"ui type\nYWJj"));
    assert(call(a,0x161,token,0,0,10003)>0);
    assert(call(a,0x16a,token,42,0,10003)==0);
    assert(call(a,0x161,token,0,0,10003)==0);
    assert(call(a,0x166,0,41,0,10003)==0);
    int64_t cookie=call(a,0x166,0,42,0,10003);assert(cookie>0);
    assert(call(a,0x167,cookie+1,0,1024,10003)==-1);
    assert(call(a,0x167,cookie,0,1024,10003)==11);
    assert(!memcmp(memory,"ui type\nabc",11));
    assert(call(a,0x164,token,0,0,10003)==-1);
    assert(call(a,0x168,cookie,0,3,10003)==3);
    assert(call(a,0x169,cookie,0,0,10003)==0);
    result=ipod_agent_take_result(a);assert(!strcmp(result,"ui 0\ndWkg"));g_free(result);
    assert(ipod_agent_submit(a,"expired uidump\n"));
    assert(call(a,0x161,token,0,0,10003)>0);
    assert(call(a,0x16a,token,42,0,10003)==0);
    assert(call(a,0x169,cookie,0,0,10003)==-1);
    cookie=call(a,0x166,0,42,0,10003);
    assert(call(a,0x169,cookie,0,0,15003)==-1);
    assert(call(a,0x161,token,0,0,15003)==0);
    result=ipod_agent_take_result(a);
    expected=g_strdup_printf("expired %d\n",-ETIMEDOUT);
    assert(!strcmp(result,expected));g_free(result);g_free(expected);
    assert(ipod_agent_submit(a,"cancelled-result ping\n"));
    assert(call(a,0x161,token,0,0,15003)>0);
    assert(call(a,0x164,token,0,0,15003)==0);
    assert(ipod_agent_cancel(a,"cancelled-result"));
    result=ipod_agent_take_result(a);assert(!*result);g_free(result);
    ipod_agent_publish(a);
    IPodAgent *reader=ipod_agent_acquire();
    assert(reader==a);
    ipod_agent_publish(NULL);
    ipod_agent_free(a);
    assert(!strcmp(ipod_agent_status(reader,10003),"alive"));
    ipod_agent_free(reader);
    assert(!ipod_agent_acquire());
    g_free(request);g_free(encoded);
    puts("PASS: binary windows, malformed input, memory faults, bounds, ownership, restart, reset");
}
'''
with tempfile.TemporaryDirectory() as d:
    tmp = Path(d)
    (tmp/'qemu').mkdir()
    (tmp/'qemu/osdep.h').write_text('''#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <glib.h>
''')
    (tmp/'qemu/thread.h').write_text('''#include <pthread.h>
typedef pthread_mutex_t QemuMutex;
#define qemu_mutex_init(m) pthread_mutex_init(m,NULL)
#define qemu_mutex_destroy pthread_mutex_destroy
#define qemu_mutex_lock pthread_mutex_lock
#define qemu_mutex_unlock pthread_mutex_unlock
''')
    (tmp/'check.c').write_text(check)
    flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True))
    subprocess.run(['clang','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',
                    '-I'+d,'-I'+str(root/'include'),str(root/'hw/arm/ipod-agent.c'),
                    str(tmp/'check.c'),'-o',str(tmp/'check'),*flags],check=True)
    subprocess.run([str(tmp/'check')],check=True)
