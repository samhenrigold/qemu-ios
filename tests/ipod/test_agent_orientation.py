#!/usr/bin/env python3
"""The actual SBS operation reads a fresh server port and validates every result."""
import subprocess
import tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[2]
source=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <mach/mach.h>
#include <mach/ndr.h>
static unsigned char ag_response[4096];
#define AG_RESPONSE_MAX sizeof(ag_response)
static unsigned ag_response_len;
static int degrees, failed, missing, malformed, destroyed;
static unsigned char orientation_stub[] __attribute__((aligned(4))) = {0xf0,0xb5,0x03,0xaf,0x8f,0xb0,0x34,0x4b};
static unsigned port_number=1;
static void *mock_open(const char *name,int mode){return (void *)1;}
static void release(void *value){}
static unsigned server(void){return port_number++;}
static kern_return_t allocate(mach_port_t task,mach_port_right_t right,mach_port_t *out){*out=123;return 0;}
static kern_return_t destroy(mach_port_t task,mach_port_t port){assert(port==123);destroyed++;return 0;}
static mach_msg_return_t message(mach_msg_header_t *h,mach_msg_option_t options,mach_msg_size_t send,mach_msg_size_t receive,mach_port_name_t port,mach_msg_timeout_t timeout,mach_port_name_t notify){
 assert(h->msgh_remote_port==port_number-1 && h->msgh_id==0x1e8496);
 assert(options==(MACH_SEND_MSG|MACH_RCV_MSG|MACH_SEND_TIMEOUT|MACH_RCV_TIMEOUT));
 assert(send==24 && receive>=48 && port==123 && timeout==250);
 if(failed)return MACH_RCV_TIMED_OUT;
 h->msgh_id+=100;h->msgh_size=malformed?36:40;h->msgh_bits=0;
 memcpy((char *)h+24,&NDR_record,8);int status=0;
 memcpy((char *)h+32,&status,4);memcpy((char *)h+36,&degrees,4);return 0;
}
#define mach_port_allocate allocate
#define mach_port_destroy destroy
#define mach_msg message
static void *mock_symbol(void *handle,const char *name){
 if(!strcmp(name,"CFRelease"))return release;
 if(!strcmp(name,"SBSSpringBoardServerPort"))return server;
 if(!strcmp(name,"SBGetUIOrientation"))return missing ? 0 : orientation_stub;
 assert(!"unexpected symbol lookup");return 0;
}
static int qc(unsigned op,void *buf,unsigned off,unsigned len){assert(0);return -1;}
#define dlopen mock_open
#define dlsym mock_symbol
#include "agent-sbs.h"
int main(void){
 int values[]={0,90,180,-90};
 for(unsigned i=0;i<4;i++){
  degrees=values[i];assert(!agent_sbs_inner("orientation",""));
  char expected[32];int count=snprintf(expected,sizeof(expected),"%d\n",degrees);
  assert(ag_response_len==count && !memcmp(ag_response,expected,count));
 }
 assert(port_number==5);
 degrees=17;assert(agent_sbs_inner("orientation","")==-ERANGE);
 malformed=1;assert(agent_sbs_inner("orientation","")==-EIO);malformed=0;
 failed=1;assert(agent_sbs_inner("orientation","")==-ETIMEDOUT);failed=0;
 assert(destroyed==7);
 orientation_stub[0]=0;assert(agent_sbs_inner("orientation","")==-ENOSYS);
 missing=1;assert(agent_sbs_inner("orientation","")==-ENOSYS);
 puts("PASS: bounded orientation MIG, port cleanup, firmware guard, valid angles and failure rejection");
}
'''
with tempfile.TemporaryDirectory() as temp:
 p=Path(temp);(p/'check.c').write_text(source)
 subprocess.run(['clang','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(root/'contrib/it-agent'),str(p/'check.c'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
