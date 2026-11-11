/* Exercise the production agent transport with real queues and a guest-memory fixture.
 *
 * SLICE include/hw/arm/ipod-agent.h file
 * SLICE hw/arm/ipod-agent.c range typedef struct AgentItem { | int64_t ipod_agent_call(
 * SLICE hw/arm/ipod-agent.c fn ipod_agent_call
 * PKG glib-2.0
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <glib.h>
typedef pthread_mutex_t QemuMutex;
#define qemu_mutex_init(m) pthread_mutex_init(m,NULL)
#define qemu_mutex_destroy pthread_mutex_destroy
#define qemu_mutex_lock pthread_mutex_lock
#define qemu_mutex_unlock pthread_mutex_unlock
static time_t rtc_now = 1265068800;   /* QEMU's RTC (-rtc base=2010-02-02), not the host's */
static void qemu_get_timedate(struct tm *tm, int offset) { time_t t = rtc_now + offset; gmtime_r(&t, tm); }
#define mktimegm timegm
#include "slice.h"
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
    assert(ipod_agent_submit(a,"bad")==-EINVAL);
    assert(ipod_agent_submit(a,"a ping\n%%%")==-EINVAL);
    assert(ipod_agent_submit(a,"a ping\nYQ=")==-EINVAL);
    /* Largest body that fits beside its header is accepted; one more byte is -EFBIG. */
    static uint8_t big[IT_AGENT_REQUEST_MAX];
    const char *h="big putpart 0 1 644 /x\n";
    char *b=g_base64_encode(big,IT_AGENT_REQUEST_MAX-strlen(h));
    char *r=g_strdup_printf("%s%s",h,b);
    assert(!ipod_agent_submit(a,r));assert(ipod_agent_cancel(a,"big"));g_free(r);g_free(b);
    b=g_base64_encode(big,IT_AGENT_REQUEST_MAX-strlen(h)+1);r=g_strdup_printf("%s%s",h,b);
    assert(ipod_agent_submit(a,r)==-EFBIG);g_free(r);g_free(b);
    assert(strstr(ipod_agent_submit_error(-EFBIG),"putpart"));
    uint8_t body[3000];
    for (int i=0;i<3000;i++) body[i]=i;
    char *encoded=g_base64_encode(body,sizeof(body));
    char *request=g_strdup_printf("first exec cat\n%s",encoded);
    assert(!ipod_agent_submit(a,request));
    assert(ipod_agent_submit(a,request)==-EBUSY);
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
    assert(ipod_agent_submit(a,request)==-EBUSY); /* duplicate finished id */
    char *result=ipod_agent_take_result(a);
    assert(!strcmp(result,"first 7\nYWJjYWJj")); g_free(result);
    assert(call(a,0x161,token,0,0,0)==0);
    assert(!ipod_agent_submit(a,"lost exec touch /tmp/test\n"));
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
        assert(!ipod_agent_submit(a,r));g_free(r);
    }
    assert(ipod_agent_submit(a,"overflow ping\n")==-EBUSY);
    ipod_agent_reset(a);
    assert(call(a,0x161,next,0,0,10002)==-1);
    assert(!ipod_agent_submit(a,"fresh ping\n"));
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
    assert(!ipod_agent_submit(a,"pending ping\n"));
    assert(ipod_agent_cancel(a,"pending"));
    assert(call(a,0x161,token,0,0,10003)==0);
    assert(!ipod_agent_submit(a,"ui type\nYWJj"));
    assert(call(a,0x161,token,0,0,10003)>0);
    /* a request is routed only where an it_typein polls: with none (or none for 2 s) the agent hears no at once */
    assert(call(a,0x16a,token,42,0,10003)==-1);
    assert(call(a,0x166,0,7,0,8000)==0 && call(a,0x16a,token,42,0,10003)==-1);
    assert(call(a,0x166,0,7,0,8003)==0);
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
    assert(!ipod_agent_submit(a,"expired uidump\n"));
    assert(call(a,0x161,token,0,0,10003)>0);
    assert(call(a,0x16a,token,42,0,10003)==0);
    assert(call(a,0x169,cookie,0,0,10003)==-1);
    cookie=call(a,0x166,0,42,0,10003);
    assert(call(a,0x169,cookie,0,0,15003)==-1);
    assert(call(a,0x161,token,0,0,15003)==0);
    result=ipod_agent_take_result(a);
    expected=g_strdup_printf("expired %d\n",-ETIMEDOUT);
    assert(!strcmp(result,expected));g_free(result);g_free(expected);
    assert(!ipod_agent_submit(a,"cancelled-result ping\n"));
    assert(call(a,0x161,token,0,0,15003)>0);
    assert(call(a,0x164,token,0,0,15003)==0);
    assert(ipod_agent_cancel(a,"cancelled-result"));
    result=ipod_agent_take_result(a);assert(!*result);g_free(result);
    assert(!ipod_agent_submit(a,"springboard uidump\n"));
    assert(call(a,0x161,token,0,0,15003)>0);
    assert(call(a,0x166,0,7,0,15003)==0);   /* SpringBoard's it_typein, polling as it does 20 times a second */
    assert(call(a,0x16a,token,0,0,15003)==0);
    cookie=call(a,0x166,0,0,0,15003);assert(cookie>0);
    assert(call(a,0x161,token,0,0,15003)==0);
    assert(call(a,0x169,cookie,0,0,15003)==0);
    assert(ipod_agent_cancel(a,"springboard"));
    ipod_agent_publish(a);
    IPodAgent *reader=ipod_agent_acquire();
    assert(reader==a);
    ipod_agent_publish(NULL);
    ipod_agent_free(a);
    assert(!strcmp(ipod_agent_status(reader,10003),"alive"));
    ipod_agent_free(reader);
    assert(!ipod_agent_acquire());
    g_free(request);g_free(encoded);
    /* op 0x165, the clock the agent keeps the guest on: QEMU's RTC, plus a pinned machine clock's offset */
    IPodAgent *c=ipod_agent_new();
    int64_t ct=call(c,0x160,0,0,0,0);
    assert(call(c,0x165,ct,0,0,0)==1265068800);
    ipod_agent_set_clock_offset(c,34931200);
    assert(call(c,0x165,ct,0,0,0)==1300000000);
    ipod_agent_free(c);
    /* the poll's window is the agent's state report: taken from its session only, handed to a reader once per change,
     * and dropped with whatever ends the session, so nobody reads an old report as the guest's state now */
    IPodAgent *g=ipod_agent_new();
    uint64_t serial=0;
    char *state;
    int64_t gt=call(g,0x160,0,0,0,0);
    assert(!ipod_agent_report(g,0,&serial) && serial==0);
    memcpy(memory,"agent 6\nspringboard 0\n",22);
    assert(call(g,0x161,gt+1,0,22,0)==-1 && !ipod_agent_report(g,0,&serial));
    assert(call(g,0x161,gt,0,22,0)==0);
    state=ipod_agent_report(g,0,&serial);
    assert(state && !strcmp(state,"agent 6\nspringboard 0\n") && serial==1);free(state);
    assert(call(g,0x161,gt,0,22,0)==0 && !ipod_agent_report(g,0,&serial));        /* the same report again */
    assert(call(g,0x161,gt,0,0,0)==0 && !ipod_agent_report(g,0,&serial));         /* no window: it stands */
    assert(call(g,0x161,gt,0,IT_AGENT_REPORT_MAX+1,0)==0 && !ipod_agent_report(g,0,&serial));
    fail=true;assert(call(g,0x161,gt,0,22,0)==0 && !ipod_agent_report(g,0,&serial));fail=false;
    memory[20]='1';
    assert(call(g,0x161,gt,0,22,5000)==0);
    state=ipod_agent_report(g,5000,&serial);assert(state && state[20]=='1' && serial==2);free(state);
    state=ipod_agent_report(g,15001,&serial);assert(state && !*state && serial==3);free(state);   /* a lease of silence */
    assert(call(g,0x161,gt,0,22,15001)==0);                                       /* the agent polls again */
    state=ipod_agent_report(g,15001,&serial);assert(state && state[20]=='1');free(state);
    assert(!ipod_agent_submit(g,"gone exec sleep 9\n") && call(g,0x161,gt,0,22,15001)>0);
    assert(ipod_agent_cancel(g,"gone"));                                          /* revokes the session */
    state=ipod_agent_report(g,15001,&serial);assert(state && !*state);free(state);
    gt=call(g,0x160,0,0,0,15001);assert(gt>0 && call(g,0x161,gt,0,22,15001)==0);
    state=ipod_agent_report(g,15001,&serial);assert(state && *state);free(state);
    ipod_agent_reset(g);
    state=ipod_agent_report(g,15001,&serial);assert(state && !*state);free(state);
    assert(!ipod_agent_report(g,15001,&serial));
    uint64_t fresh=0;                                                              /* a reader that starts now */
    state=ipod_agent_report(g,15001,&fresh);assert(state && !*state && fresh==serial);free(state);
    ipod_agent_free(g);
    puts("PASS: binary windows, malformed input, memory faults, bounds, ownership, restart, reset, clock, state report");
}
