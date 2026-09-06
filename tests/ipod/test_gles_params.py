#!/usr/bin/env python3
"""Actual CGL parameter queries/setters, exact guest bounds and ABI slot wiring."""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
src=(root/'hw/arm/gles-host.c').read_text()
shim=(root/'contrib/it-gles/mbxshim.c').read_text()
slots=dict(re.findall(r'^(\d+) (\w+)$',(root/'contrib/it-gles/slotmap.txt').read_text(),re.M))
macros=dict(re.findall(r'#define (GLES_SLOT_\w+)\s+(\d+)',(root/'include/hw/arm/guest-services/gles.h').read_text()))
cases=src[src.index('    case GLES_SLOT_GET_LIGHTFV:'):src.index('    case GLES_SLOT_TEX_PARAMETERI:')]
for macro in set(re.findall(r'case (GLES_SLOT_\w+):',cases)):
 slot=macros[macro]
 name=re.search(r'table\['+slot+r'\]\s*= \(void \*\)(s_\w+)',shim)[1]
 assert slots[slot].lower()==('gl'+name[2:]).lower()
 assert re.search(name+r'\([^}]+qc\('+slot+r',',shim)
for macro in ['LIGHTFV','MATERIALFV','TEX_ENVFV']:
 start=src.index('    case GLES_SLOT_'+macro+':')
 end=src.index('\n    case GLES_SLOT_',start+10)
 cases+=src[start:end]
helpers=[]
for name in ['gles_f','gles_fetch_params','gles_light_nparams','gles_material_nparams','gles_texenv_nparams','gles_texparam_nparams']:
 helpers.append(re.search(r'^static [^\n]*\b'+name+r'\([^)]*\)\s*\{.*?^}',src,re.M|re.S)[0])
code=r'''
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <limits.h>
typedef void CPUState;
#include "hw/arm/guest-services/gles.h"
static union { GLfloat f[16]; GLint i[16]; unsigned char bytes[64]; } guest;
static uint32_t base=0x1000;
static size_t capacity,last;
static unsigned reads,writes;
static GLenum error;
static int gles_reject(GLenum e) {error=e;return -1;}
static int cpu_memory_rw_debug(CPUState *c,uint32_t a,uint8_t *data,size_t n,int write) {
 if(a!=base || n>capacity)return -1;
 last=n;if(write){writes++;memcpy(guest.bytes,data,n);}else{reads++;memcpy(data,guest.bytes,n);}return 0;
}
'''+ '\n'.join(helpers)+r'''
static int dispatch(unsigned slot,unsigned target,unsigned pname,uint32_t ptr) {
 CPUState *cpu=NULL;uint32_t a[]={target,pname,ptr};switch(slot) {
'''+cases+r'''
 default:assert(0);
 }return -1;
}
static void destination(unsigned n) {memset(&guest,0xcc,sizeof(guest));capacity=n*4;last=0;reads=writes=0;error=0;}
static void query(unsigned slot,unsigned target,unsigned pname,unsigned n) {
 destination(n);assert(!dispatch(slot,target,pname,base));assert(writes==1 && !reads && last==n*4);
 for(unsigned i=n*4;i<sizeof(guest);i++)assert(guest.bytes[i]==0xcc);
}
int main(void) {
 CGLPixelFormatAttribute attrs[]={kCGLPFAAccelerated,0};CGLPixelFormatObj pf;GLint np;CGLContextObj ctx;
 assert(CGLChoosePixelFormat(attrs,&pf,&np)==kCGLNoError);assert(CGLCreateContext(pf,NULL,&ctx)==kCGLNoError);
 CGLDestroyPixelFormat(pf);assert(CGLSetCurrentContext(ctx)==kCGLNoError);
 glMatrixMode(GL_MODELVIEW);glLoadIdentity();
 destination(3);guest.f[0]=1;guest.f[1]=2;guest.f[2]=3;
 assert(!dispatch(GLES_SLOT_LIGHTFV,GL_LIGHT0,GL_SPOT_DIRECTION,base) && last==12);
 query(GLES_SLOT_GET_LIGHTFV,GL_LIGHT0,GL_SPOT_DIRECTION,3);assert(guest.f[0]==1 && guest.f[1]==2 && guest.f[2]==3);
 destination(1);guest.f[0]=12;assert(!dispatch(GLES_SLOT_MATERIALFV,GL_FRONT_AND_BACK,GL_SHININESS,base));
 query(GLES_SLOT_GET_MATERIALFV,GL_BACK,GL_SHININESS,1);assert(guest.f[0]==12);
 destination(4);for(unsigned i=0;i<4;i++)guest.f[i]=(i+1)*.25f;
 assert(!dispatch(GLES_SLOT_MATERIALFV,GL_FRONT_AND_BACK,GL_DIFFUSE,base));
 query(GLES_SLOT_GET_MATERIALFV,GL_FRONT,GL_DIFFUSE,4);for(unsigned i=0;i<4;i++)assert(guest.f[i]==(i+1)*.25f);
 destination(4);for(unsigned i=0;i<4;i++)guest.f[i]=(i+1)*.25f;
 assert(!dispatch(GLES_SLOT_TEX_ENVFV,GL_TEXTURE_ENV,GL_TEXTURE_ENV_COLOR,base));
 query(GLES_SLOT_GET_TEX_ENVFV,GL_TEXTURE_ENV,GL_TEXTURE_ENV_COLOR,4);assert(guest.f[1]==.5f);
 destination(4);guest.i[0]=INT_MAX;guest.i[1]=0;guest.i[2]=-INT_MAX;guest.i[3]=INT_MAX;
 assert(!dispatch(GLES_SLOT_TEX_ENVIV,GL_TEXTURE_ENV,GL_TEXTURE_ENV_COLOR,base));
 query(GLES_SLOT_GET_TEX_ENVIV,GL_TEXTURE_ENV,GL_TEXTURE_ENV_COLOR,4);assert(guest.i[0]==INT_MAX && guest.i[1]==0 && guest.i[2]==0 && guest.i[3]==INT_MAX);
 destination(1);guest.i[0]=GL_REPLACE;assert(!dispatch(GLES_SLOT_TEX_ENVIV,GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,base));
 query(GLES_SLOT_GET_TEX_ENVIV,GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,1);assert(guest.i[0]==GL_REPLACE);
 GLuint texture;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
 union {float f;uint32_t u;} value={.f=GL_LINEAR};
 assert(!dispatch(GLES_SLOT_TEX_PARAMETERF,GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,value.u));
 query(GLES_SLOT_GET_TEX_PARAMETERIV,GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,1);assert(guest.i[0]==GL_LINEAR);
 destination(1);guest.f[0]=GL_NEAREST;assert(!dispatch(GLES_SLOT_TEX_PARAMETERFV,GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,base));
 query(GLES_SLOT_GET_TEX_PARAMETERFV,GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,1);assert(guest.f[0]==GL_NEAREST);
 destination(1);guest.i[0]=GL_CLAMP_TO_EDGE;assert(!dispatch(GLES_SLOT_TEX_PARAMETERIV,GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,base));
 query(GLES_SLOT_GET_TEX_PARAMETERIV,GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,1);assert(guest.i[0]==GL_CLAMP_TO_EDGE);
 const unsigned invalid[][3]={{GLES_SLOT_GET_LIGHTFV,GL_LIGHT7+1,GL_AMBIENT},{GLES_SLOT_GET_LIGHTFV,GL_LIGHT0,0xffff},
 {GLES_SLOT_GET_MATERIALFV,GL_FRONT_AND_BACK,GL_DIFFUSE},{GLES_SLOT_GET_MATERIALFV,GL_FRONT,GL_AMBIENT_AND_DIFFUSE},
 {GLES_SLOT_GET_TEX_ENVFV,GL_TEXTURE_2D,GL_TEXTURE_ENV_COLOR},{GLES_SLOT_GET_TEX_ENVIV,GL_TEXTURE_ENV,GL_TEXTURE_BORDER_COLOR},
 {GLES_SLOT_GET_TEX_PARAMETERFV,GL_TEXTURE_3D,GL_TEXTURE_MIN_FILTER},{GLES_SLOT_GET_TEX_PARAMETERIV,GL_TEXTURE_2D,GL_TEXTURE_BORDER_COLOR},
 {GLES_SLOT_TEX_PARAMETERIV,GL_TEXTURE_2D,GL_TEXTURE_BORDER_COLOR},{GLES_SLOT_TEX_ENVIV,GL_TEXTURE_ENV,0xffff},
 {GLES_SLOT_LIGHTFV,GL_LIGHT0,0xffff},{GLES_SLOT_MATERIALFV,GL_FRONT_AND_BACK,0xffff}};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
  destination(4);assert(dispatch(invalid[i][0],invalid[i][1],invalid[i][2],base)==-1 && error==GL_INVALID_ENUM && !reads && !writes);
  for(unsigned j=0;j<sizeof(guest);j++)assert(guest.bytes[j]==0xcc);
 }
 destination(1);assert(dispatch(GLES_SLOT_GET_LIGHTFV,GL_LIGHT0,GL_AMBIENT,base)==-1 && !writes);
 destination(3);assert(dispatch(GLES_SLOT_TEX_ENVIV,GL_TEXTURE_ENV,GL_TEXTURE_ENV_COLOR,base)==-1 && !reads);
 base=0xfffffffcu;query(GLES_SLOT_GET_MATERIALFV,GL_FRONT,GL_SHININESS,1);assert(guest.f[0]==12);
 destination(4);assert(dispatch(GLES_SLOT_GET_LIGHTFV,GL_LIGHT0,GL_AMBIENT,base)==-1 && !writes);
 assert(dispatch(GLES_SLOT_TEX_ENVIV,GL_TEXTURE_ENV,GL_TEXTURE_ENV_COLOR,base)==-1 && !reads);
 assert(glGetError()==GL_NO_ERROR);glDeleteTextures(1,&texture);CGLSetCurrentContext(NULL);CGLDestroyContext(ctx);
 puts("PASS: light/material/texture parameter APIs, exact 1/3/4-word bounds, enum rejection, integer colors and ABI wiring");
}
'''
with tempfile.TemporaryDirectory(prefix='it-gles-params-') as tmp:
 tmp=Path(tmp);(tmp/'check.c').write_text(code)
 subprocess.run(['clang','-fsanitize=address,undefined','-I'+str(root/'include'),str(tmp/'check.c'),'-framework','OpenGL','-o',str(tmp/'check')],check=True)
 subprocess.run([str(tmp/'check')],check=True)
