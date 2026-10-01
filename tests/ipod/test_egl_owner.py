#!/usr/bin/env python3
"""Run actual EGL ownership callbacks with ASan/UBSan host-service doubles.

By default test both repository frontends. --source selects one source, including
an unmodified baseline, which must compile and fail the lifetime assertion.
Only GL dispatch and GC destruction are doubled; the attach, release, destroy,
and thread-local current-context functions are extracted from production source.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

HOST_DOUBLES = r"""
  #include <stdlib.h>
  #include <assert.h>
  #include <string.h>
  #include <stdio.h>
  #include <pthread.h>
  typedef unsigned EGLint;
  typedef int EGLBoolean;
  typedef struct { unsigned host; unsigned *batch,batch_len; } GuestGC;
  #define FE_DISPLAY ((void*)1)
  #define FE_NCONFIGS 3
#define GLES2X_DISPLAY FE_DISPLAY
#define GLES2X_WINDOW_ORDER FE_WINDOW_ORDER
#define gles2x_cur_t fe_cur_t
#define gles2x_cur fe_cur
#define gles2x_set_current fe_set_current
#define gles2x_key fe_key
#define gles2x_once fe_once
#define gles2x_key_init fe_key_init
static const struct {unsigned ifmt;} gles2x_configs[3]={{0x8058},{0x8058},{0x8058}};
  #define EGL_BAD_DISPLAY 0x3008
  #define EGL_BAD_CONTEXT 0x3006
  #define EGL_BAD_SURFACE 0x300d
  #define EGL_PIXMAP_BIT 2
  #define EGL_WINDOW_BIT 4
  #define FE_WINDOW_ORDER 0x80000000u
  static int egl_error;
  static EGLBoolean egl_fail(EGLint e){egl_error=e;return 0;}
  typedef struct {GuestGC *gc;void *owner;int is_egl;void *draw,*read;} fe_cur_t;
  static pthread_key_t fe_key;
  static pthread_once_t fe_once=PTHREAD_ONCE_INIT;
  static unsigned releases,deletes,binds;
  static void batch_flush(GuestGC *gc){assert(gc->host);}
  static void fe_key_init(void){pthread_key_create(&fe_key,free);}
  static void mock_gl(GuestGC *gc,const char *name)
  { assert(gc->host==0x80000002||gc->host==0x80000004);
   if(!strcmp(name,"glDeleteFramebuffers")||!strcmp(name,"glDeleteTextures")) releases++; }
  #define GL(gc,name,...) mock_gl(gc,#name)
  static int GLESDestroyGC(void *g){GuestGC *gc=g;assert(gc->host);deletes++;free(gc);return 0;}
  static int fe_bind_layer(GuestGC *gc,void *layer){assert(gc->host);(void)layer;binds++;return 1;}
  static int GLESBindView(GuestGC *gc,void *layer,void *format,void *unused)
{(void)format;(void)unused;return fe_bind_layer(gc,layer);}
static int GLESBindCoreSurface(GuestGC *gc,unsigned target,void *s){assert(gc->host);(void)target;(void)s;return 1;}
  static void refused(const char*a,const char*b,unsigned c){(void)a;(void)b;(void)c;abort();}
  static void w(const char*s){(void)s;}
  static void wd(unsigned n){(void)n;}
  static void fe_ca_path(const char*s){(void)s;}
"""

CONTEXT_HELPERS = r"""
static egl_ctx_t *context(unsigned handle) {
 egl_ctx_t *c=calloc(1,sizeof *c);c->magic=EGL_CTX_MAGIC;c->gc=calloc(1,sizeof *c->gc);c->gc->host=handle;return c;
}
static egl_surf_t *surface(unsigned kind) {
 egl_surf_t *s=calloc(1,sizeof *s);s->magic=EGL_SURF_MAGIC;s->kind=kind;s->native=(void*)1;s->width=320;s->height=480;return s;
}
static void attach(egl_ctx_t*c,egl_surf_t*s) {
"""

OWNERSHIP_CASES = r"""
 s->tex=10;s->fbo=11;
}
int main(void) {
 egl_ctx_t *c=context(0x80000002);egl_surf_t *s[3];
 for(unsigned i=0;i<3;i++){s[i]=surface(EGL_PIXMAP_BIT);attach(c,s[i]);}
 fe_set_current(c->gc,c,1,s[0],s[0]);
 assert(eglDestroyContext(FE_DISPLAY,c));assert(fe_cur(0)->gc==0);
 for(unsigned i=0;i<3;i++)assert(eglDestroySurface(FE_DISPLAY,s[i]));
 assert(releases==6&&deletes==1);
 // Surfaces destroyed first remove owner links, leaving context valid.
 c=context(0x80000002);egl_surf_t *one=surface(EGL_PIXMAP_BIT);attach(c,one);
 assert(eglDestroySurface(FE_DISPLAY,one));assert(eglDestroyContext(FE_DISPLAY,c));
 assert(releases==8&&deletes==2);
 // A surface moved to B must not be invalidated by A's destruction.
 egl_ctx_t *a=context(0x80000002),*b=context(0x80000004);
 one=surface(EGL_PIXMAP_BIT);attach(a,one);attach(b,one);
 fe_set_current(b->gc,b,1,one,one);
 assert(eglDestroyContext(FE_DISPLAY,a));assert(one->gc==b->gc);
 assert(fe_cur(0)->gc==b->gc&&fe_cur(0)->owner==b);
 assert(eglDestroyContext(FE_DISPLAY,b));assert(eglDestroySurface(FE_DISPLAY,one));
 assert(releases==12&&deletes==4);
 // Context-first window teardown detaches while its GC lives.
 c=context(0x80000002);one=surface(EGL_WINDOW_BIT);attach(c,one);
 assert(eglDestroyContext(FE_DISPLAY,c));assert(eglDestroySurface(FE_DISPLAY,one));
 assert(binds==2&&deletes==5);
 // A surviving surface can allocate fresh names in a new live context.
 c=context(0x80000002);b=context(0x80000004);
 one=surface(EGL_PIXMAP_BIT);attach(c,one);
 assert(eglDestroyContext(FE_DISPLAY,c));assert(one->gc==0);
 attach(b,one);assert(one->gc==b->gc);
 assert(eglDestroyContext(FE_DISPLAY,b));assert(eglDestroySurface(FE_DISPLAY,one));
 assert(releases==16&&deletes==7);
 puts("actual EGL destroy/attach/release_names:3buffers/contextfirst/surfacefirst/rebind/window PASS");
}
"""

def extract_function(source, signature):
    start = source.index(signature + "\n{")
    cursor = source.index("{", start) + 1
    depth = 1
    while depth:
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
        cursor += 1
    return source[start:cursor] + "\n"


def fixture(source):
    tracked_owner = "typedef struct egl_surf egl_surf_t;" in source
    legacy = "gles2x_cur_t" in source
    declaration = (
        "typedef struct egl_surf egl_surf_t;" if tracked_owner else
        "typedef struct { unsigned magic; GuestGC *gc; void *sg; int config; } egl_ctx_t;"
    )
    start = source.index(declaration)
    end = source.index("static EGLint egl_config_attrib", start)
    current_type = "gles2x_cur_t" if legacy else "fe_cur_t"
    current_getter = "gles2x_cur" if legacy else "fe_cur"
    current_setter = "gles2x_set_current" if legacy else "fe_set_current"
    signatures = [
        f"static {current_type} *{current_getter}(int create)",
        f"static void {current_setter}(GuestGC *gc, void *owner, int is_egl, void *draw, void *read)",
    ]
    if not legacy:
        signatures.append("static void fe_destroy_gc(GuestGC *gc)")
    signatures.append("static void egl_release_names(egl_surf_t *s)")
    if tracked_owner:
        signatures.append("static void egl_surface_owner(egl_ctx_t *owner, egl_surf_t *s)")
    signatures.extend([
        "static int egl_attach(egl_ctx_t *owner, egl_surf_t *s)" if tracked_owner else
        "static int egl_attach(GuestGC *gc, egl_surf_t *s)",
        "EGLBoolean eglDestroyContext(void *dpy, void *ctx)",
        "EGLBoolean eglDestroySurface(void *dpy, void *surface)",
    ])
    callbacks = "".join(extract_function(source, name) for name in signatures)
    attachment = "assert(egl_attach(c, s));" if tracked_owner else "assert(egl_attach(c->gc, s));"
    return HOST_DOUBLES + source[start:end] + callbacks + CONTEXT_HELPERS + attachment + OWNERSHIP_CASES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="single frontend override, including baseline")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    sources = [args.source] if args.source else [
        root / "contrib/gles-public/opengles.c",
        root / "contrib/it-gles/gles1x.c",
    ]
    compiler = os.environ.get("CC", "clang")
    if not shutil.which(compiler):
        raise SystemExit("Required sanitizer compiler unavailable: " + compiler)
    for source_path in sources:
        with tempfile.TemporaryDirectory(prefix="egl-owner-test-") as scratch:
            directory = Path(scratch)
            source = directory / "fixture.c"
            executable = directory / "fixture"
            source.write_text(fixture(source_path.read_text()))
            subprocess.run([
                compiler, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
                "-fsanitize=address,undefined", str(source), "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)
            print("PASS", source_path)


if __name__ == "__main__":
    main()
