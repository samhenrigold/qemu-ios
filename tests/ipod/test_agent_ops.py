#!/usr/bin/env python3
"""Run the actual daemon operations on host fixtures, including child cleanup."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = r'''
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
#include "agent-ops.h"
static void request(const char *header,const void *body,unsigned len) {
    size_t n=strlen(header);
    memcpy(ag_request,header,n);memcpy(ag_request+n,body,len);ag_request[n+len]=0;
    done=false;result_len=0;agent_dispatch(n+len);
    while (!done) {usleep(5000);agent_child_tick();}
}
int main(void) {
    request("1 ping\n","",0);assert(!result_status && result_len==12);
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
    request("10 ping\n","",0);assert(!result_status && result_len==12);
    puts("PASS: binary commands/files, stderr/status, permissions, overflow, timeout and recovery");
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'check.c').write_text(source)
    subprocess.run(['clang','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',
                    '-I'+str(root/'contrib/it-agent'),str(p/'check.c'),'-o',str(p/'check')],check=True)
    subprocess.run([str(p/'check')],cwd=d,check=True)
