/* Run the actual daemon operations on host fixtures, including child cleanup.
 *
 * SLICE:agent-ops contrib/it-agent/agent-ops.h file
 * SLICE:agent-sbs contrib/it-agent/agent-sbs.h file
 * SLICE:sbs-launch contrib/it-agent/sbs-launch.h file
 * CFLAGS -Wno-deprecated-declarations
 */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <string.h>
static unsigned long long agent_token=123;
static unsigned char result[1024*1024];
static unsigned result_len;
static int result_status;
static bool done;
static int64_t qc(uint32_t op,void *buf,uint32_t off,uint32_t len) {
    if (op==0x163) { assert(off+len<=sizeof(result));memcpy(result+off,buf,len);result_len=off+len;return len; }
    if (op==0x164) {done=true;result_status=(int32_t)off;return 0;}
    assert(0);return -1;
}
#include "agent-ops.h"   /* the whole production agent-ops.h, with its agent-sbs.h and sbs-launch.h */
static void request(const char *header,const void *body,unsigned len) {
    size_t n=strlen(header);
    memcpy(ag_request,header,n);memcpy(ag_request+n,body,len);ag_request[n+len]=0;
    done=false;result_len=0;agent_dispatch(n+len);
    while (!done) {usleep(5000);agent_child_tick();}
}
int main(void) {
    request("1 ping\n","",0);
    assert(!result_status && result_len==strlen(AG_HELLO) && !memcmp(result,"it_agent v4\nops ",16));
    assert(strstr(AG_HELLO," cadebug ") && strstr(AG_HELLO," statusbar\n"));
    /* malformed requests are refused before any framework is touched */
    request("1b cadebug 4\n","",0);assert(result_status==-EINVAL);
    request("1c cadebug x 4\n","",0);assert(result_status==-EINVAL);
    request("1d statusbar\n","",0);assert(result_status==-EINVAL);
    assert(strstr(AG_HELLO," putpart "));
    assert(strstr(AG_HELLO," spawn ") && strstr(AG_HELLO," chown ") && strstr(AG_HELLO," dlicon ") && !strstr(AG_HELLO," kill "));
    char binary[10000];for(int i=0;i<sizeof(binary);i++)binary[i]=i;
    request("2 exec cat\n",binary,sizeof(binary));
    assert(!result_status && result_len==sizeof(binary) && !memcmp(binary,result,sizeof(binary)));
    request("3 exec printf err >&2; exit 7\n","",0);
    assert(result_status==7 && result_len==3 && !memcmp(result,"err",3));
    request("4 put file with spaces 600\n",binary,sizeof(binary));assert(!result_status);
    request("5 get file with spaces\n","",0);
    assert(!result_status && result_len==sizeof(binary) && !memcmp(binary,result,sizeof(binary)));
    request("5b getrange 257 2048 file with spaces\n","",0);
    assert(!result_status && result_len==2048 && !memcmp(binary+257,result,2048));
    request("5c getrange 999999 2048 file with spaces\n","",0);
    assert(!result_status && result_len==0);
    request("5d getrange -1 2048 file with spaces\n","",0);assert(result_status==-EINVAL);
    request("5e getrange 0 1048577 file with spaces\n","",0);assert(result_status==-EINVAL);
    request("5f getrange 0 4294967296 file with spaces\n","",0);assert(result_status==-EINVAL);
    struct stat st;assert(!stat("file with spaces",&st) && (st.st_mode&0777)==0600);
    request("6 put file with spaces bad\n","",0);assert(result_status==-EINVAL);
    request("7 get /dev/zero\n","",0);assert(result_status==-EINVAL);
    request("8 exec yes x\n","",0);assert(result_status==-EFBIG && !ag_child && ag_output==-1);
    request("9 exec sleep 60\n","",0);assert(result_status==-ETIMEDOUT && !ag_child && ag_output==-1);
    request("10 ping\n","",0);assert(!result_status && result_len==strlen(AG_HELLO));
    static const char argv[]="/bin/echo\0one two\0$HOME;x";
    request("11 spawn\n",argv,sizeof(argv));
    assert(!result_status && result_len==16 && !memcmp(result,"one two $HOME;x\n",16));
    request("12 spawn\n","echo",5);assert(result_status==-EINVAL);
    request("13 spawn\n","/bin/echo",9);assert(result_status==-EINVAL);
    request("14 spawn\n","/nonexistent",13);assert(result_status==-ENOENT);
    request("15 sync\n","",0);assert(!result_status && !result_len);
    char header[256];
    snprintf(header,sizeof(header),"16 chown %d %d file with spaces\n",(int)getuid(),(int)getgid());
    request(header,"",0);assert(!result_status);
    request("17 chown 1 file with spaces\n","",0);assert(result_status==-EINVAL);
    request("18 chown -1 0 file with spaces\n","",0);assert(result_status==-EINVAL);
    request("19 chown 0 0\n","",0);assert(result_status==-EINVAL);
    request("20 chown 0 0 /nonexistent/x\n","",0);assert(result_status==-ENOENT);
    request("21 unlink file with spaces\n","",0);assert(!result_status);
    assert(stat("file with spaces",&st) && errno==ENOENT);
    request("22 unlink file with spaces\n","",0);assert(result_status==-ENOENT);
    request("23 unlink\n","",0);assert(result_status==-EINVAL);
    request("24 kill SpringBoard\n","",0);assert(result_status==-ENOSYS);
    request("25 dlicon add x\n","",0);assert(result_status==-ENOSYS); /* no SpringBoardServices on the host */
    /* putpart: in-order chunks into a part file, renamed only by the final one. */
    static char large[600000];for(int i=0;i<sizeof(large);i++)large[i]=i*7+(i>>9);
    request("26 putpart 0 0 644 big file\n",large,250000);assert(!result_status && !result_len);
    assert(stat("big file",&st) && !stat("big file.it-agent-part",&st) && st.st_size==250000);
    request("27 putpart 250000 0 644 big file\n",large+250000,250000);assert(!result_status);
    request("28 putpart 500000 1 640 big file\n",large+500000,100000);assert(!result_status);
    assert(!stat("big file",&st) && st.st_size==sizeof(large) && (st.st_mode&0777)==0640);
    assert(stat("big file.it-agent-part",&st) && errno==ENOENT);
    request("29 get big file\n","",0);
    assert(!result_status && result_len==sizeof(large) && !memcmp(result,large,sizeof(large)));
    /* A gap or replay discards the part and leaves the target untouched. */
    request("30 putpart 0 0 644 big file\n","abc",3);assert(!result_status);
    request("31 putpart 5 1 644 big file\n","def",3);assert(result_status==-EINVAL);
    assert(stat("big file.it-agent-part",&st) && errno==ENOENT);
    request("32 putpart 3 1 644 big file\n","def",3);assert(result_status==-ENOENT);
    assert(!stat("big file",&st) && st.st_size==sizeof(large));
    request("33 putpart 0 1 644 big file\n","",0);assert(!result_status);
    assert(!stat("big file",&st) && st.st_size==0);
    request("34 putpart 0 2 644 x\n","",0);assert(result_status==-EINVAL);
    request("35 putpart 0 1 999 x\n","",0);assert(result_status==-EINVAL);
    request("36 putpart -1 1 644 x\n","",0);assert(result_status==-EINVAL);
    request("37 putpart 0 1 644\n","",0);assert(result_status==-EINVAL);
    request("38 putpart 0 1 644 \n","",0);assert(result_status==-EINVAL);
    assert(!symlink("elsewhere","link.it-agent-part"));
    request("39 putpart 0 1 644 link\n","x",1);assert(result_status==-ELOOP);
    puts("PASS: binary commands/files, stderr/status, permissions, overflow, timeout, recovery, shell-free spawn, sync, chown, unlink, versioned ping, no kill, chunked putpart");
}
