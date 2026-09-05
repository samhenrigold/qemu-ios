#!/usr/bin/env python3
"""Exercise native context isolation and sharegroup lifetime from the real bridge."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
s = (root / 'hw/arm/gles-host.c').read_text()
def function(name):
    a = s.index('static ', s.index(name) - 50)
    # Locate declarations by the actual function name, then its preceding line.
    a = s.rfind('\nstatic ', 0, s.index(name)) + 1
    return s[a:s.index('\n}', s.index(name)) + 2]
types = s[s.index('typedef struct {'):s.index('} GLESHost;') + len('} GLESHost;')]
a = s.rindex('static bool gles_platform_context_create(')
create = s[a:s.index('\n}', a) + 2]
a = s.index('static GHashTable *gles_contexts, *gles_groups;')
ops = s[a:s.index('\n#endif', a)]
a = s.index('void gles_host_reset(void)')
ops += s[a:s.index('\n}', a) + 2]
header = r'''
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define GLES_MAX_TEXUNITS 8
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define GLES_OP_NEW_SHAREGROUP 0x1004
#define GLES_OP_DELETE_SHAREGROUP 0x1005
#define GLES_OP_NEW_CONTEXT 0x1006
#define GLES_OP_DELETE_CONTEXT 0x1007
'''
state = r'''
static GLESHost gh_legacy;
static GLESHost *gh_current = &gh_legacy;
#define gh (*gh_current)
static GLESHost *select_context(unsigned id)
{
    GLESHost *s=g_hash_table_lookup(gles_contexts,GUINT_TO_POINTER(id));assert(s);
    gh_current=s;
    if(!s->cgl) assert(gles_platform_context_create());
    assert(!CGLSetCurrentContext(s->cgl));
    return s;
}
'''
shim = (root / 'contrib/it-gles/mbxshim.c').read_text()
start = shim.index('static int GLESCreateSharegroup(')
end = shim.index('\n}', shim.index('static int GLESDestroySharegroup', start)) + 2
sharegroup = r''' 
typedef struct { unsigned host; } GuestGC;
#define A(...) ((unsigned[]){__VA_ARGS__})
static long long qc(unsigned slot, void *gc, unsigned argc, const unsigned *args)
{ return gles_context_operation(slot,gc?((GuestGC*)gc)->host:0,argc,args); }
''' + shim[start:end]
check = r'''
int main(void)
{
    void *guest_group=NULL;
    assert(!GLESCreateSharegroup(NULL));
    assert(GLESCreateSharegroup(&guest_group)==1 && guest_group);
    uint32_t group=((GuestGC*)guest_group)->host;
    uint32_t one=gles_context_operation(GLES_OP_NEW_CONTEXT,0,1,&group);
    uint32_t two=gles_context_operation(GLES_OP_NEW_CONTEXT,0,1,&group);
    assert(one!=two && one>=0x80000000);
    GLESHost *a=select_context(one);
    glEnable(GL_BLEND);a->vertex.enabled=1;
    GLuint texture;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    uint8_t pixel[]={7,29,61,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    glFinish();
    GLESHost *b=select_context(two);
    assert(!glIsEnabled(GL_BLEND) && !b->vertex.enabled);
    assert(a->buffers==b->buffers && a->surfaces==b->surfaces);
    assert(glIsTexture(texture));
    glBindTexture(GL_TEXTURE_2D,texture);
    uint8_t got[4];glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,got);
    assert(!memcmp(got,pixel,4));
    select_context(one);assert(glIsEnabled(GL_BLEND) && gh.vertex.enabled);
    GLESBuffer *old=gles_buffer_intern(17);
    old->data=g_memdup2(pixel,4);old->size=4;
    gles_buffer_bind(&a->array_buffer,old);
    select_context(two);gles_buffer_bind(&b->vertex.vbo,old);
    select_context(one);gles_buffer_forget(old);
    g_hash_table_remove(gh.buffers,GUINT_TO_POINTER(17));
    assert(!a->array_buffer && !memcmp(b->vertex.vbo->data,pixel,4));
    GLESBuffer *fresh=gles_buffer_intern(17);
    assert(fresh!=old && !fresh->size && b->vertex.vbo==old);
    uint32_t isolated=gles_context_operation(GLES_OP_NEW_SHAREGROUP,0,0,NULL);
    uint32_t three=gles_context_operation(GLES_OP_NEW_CONTEXT,0,1,&isolated);
    select_context(three);assert(!glIsTexture(texture));
    assert(GLESDestroySharegroup(guest_group)==0);
    assert(gles_context_operation(GLES_OP_NEW_CONTEXT,0,1,&group)==-1);
    assert(!gles_context_operation(GLES_OP_DELETE_CONTEXT,one,0,NULL));
    select_context(two);assert(glIsTexture(texture));
    assert(!gles_context_operation(GLES_OP_DELETE_CONTEXT,two,0,NULL));
    assert(gh_current==&gh_legacy);
    assert(gles_context_operation(GLES_OP_DELETE_CONTEXT,two,0,NULL)==-1);
    assert(!gles_context_operation(GLES_OP_DELETE_CONTEXT,three,0,NULL));
    assert(!gles_context_operation(GLES_OP_DELETE_SHAREGROUP,0,1,&isolated));
    assert(!g_hash_table_size(gles_contexts) && !g_hash_table_size(gles_groups));
    /* Reboot while a live context outlasts its deleted guest sharegroup. */
    isolated=gles_context_operation(GLES_OP_NEW_SHAREGROUP,0,0,NULL);
    three=gles_context_operation(GLES_OP_NEW_CONTEXT,0,1,&isolated);
    select_context(three);
    assert(!gles_context_operation(GLES_OP_DELETE_SHAREGROUP,0,1,&isolated));
    gh_current=&gh_legacy;
    assert(gles_platform_context_create());
    gles_buffer_intern(22);
    gles_host_reset();
    assert(!gh_legacy.cgl && !gh_legacy.buffers && gh_current==&gh_legacy);
    assert(!g_hash_table_size(gles_contexts) && !g_hash_table_size(gles_groups));
    assert(gles_context_operation(GLES_OP_DELETE_CONTEXT,three,0,NULL)==-1);
    isolated=gles_context_operation(GLES_OP_NEW_SHAREGROUP,0,0,NULL);
    assert(isolated>three);
    three=gles_context_operation(GLES_OP_NEW_CONTEXT,0,1,&isolated);
    select_context(three);
    gles_host_reset();gles_host_reset();
    g_hash_table_destroy(gles_contexts);g_hash_table_destroy(gles_groups);
    puts("PASS: native GL state isolation, shared textures and deleted buffers, independent groups and destruction order");
}
'''
# State declarations precede the real functions; the selector uses those functions.
declarations, selector = state.split('static GLESHost *select_context', 1)
code = header + types + declarations + create + ''.join(function(n+'(') for n in ('gles_buffer_destroy', 'gles_buffer_bind', 'gles_buffer_intern', 'gles_buffer_forget')) + ops + sharegroup + 'static GLESHost *select_context' + selector + check
with tempfile.TemporaryDirectory(prefix='it-gles-context-') as tmp:
    c=Path(tmp)/'check.c';exe=Path(tmp)/'check';c.write_text(code)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True).split()
    subprocess.run(['clang','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe),*flags,'-framework','OpenGL'],check=True)
    subprocess.run([str(exe)],check=True)
