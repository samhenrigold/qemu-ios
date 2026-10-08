/* Exercise the guest CA drawable lifecycle with real shim functions.
 *
 * SLICE:views contrib/it-gles/mbxshim.c range /*\n * ONE OF THESE PER GC | static void *iosurf;
 * SLICE:views contrib/it-gles/mbxshim.c range static int (*p_IOSurfaceLock) | static unsigned long (*p_IOSurfaceGetTypeID)
 * SLICE:bind contrib/it-gles/mbxshim.c range typedef int (*ca_bind_fn) | /* A mapped IOSurface
 * SLICE:bind contrib/it-gles/mbxshim.c range static int GLESBindView( | /* 7E18 0x1d020
 * CFLAGS -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-comment
 */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#define GLES_OP_DELETE_CONTEXT 0x1007
#define GLES_OP_DRAWABLE_STORAGE 0x1008
#define CA_FOURCC_BGRA 0x42475241
#define A(...) (const unsigned[]){__VA_ARGS__}
typedef struct { unsigned host; } GuestGC;
static unsigned deleted;
static long long qc(unsigned op,void *gc,unsigned argc,const unsigned *args)
{ if(op==GLES_OP_DRAWABLE_STORAGE) { assert(argc==2 && args[0] && args[1]);return 0; }
  assert(op==GLES_OP_DELETE_CONTEXT);deleted++;return 0; }
static void iosurface_init(void) {}
static void w(const char *s) {}
static void wx(unsigned long x) {}
static void wd(unsigned x) {}
static void refused(const char *what, const char *name, unsigned num) {}
#include "views.h"
typedef struct { void *surface; unsigned locks, unlocks, references; } SurfaceRecord;
static SurfaceRecord records[16];
static void *fail_lock;
static int fail_capture;
static SurfaceRecord *record(void *surface)
{
    for(unsigned i=0;i<16;i++) {
        if(records[i].surface==surface)return &records[i];
        if(!records[i].surface) { records[i].surface=surface;return &records[i]; }
    }
    assert(!"surface registry exhausted");return NULL;
}
static int lock_surface(void *surface,unsigned flags,unsigned *seed)
{
    assert(flags==3 && !seed);
    if(surface==fail_lock)return 1;
    record(surface)->locks++;return 0;
}
static int unlock_surface(void *surface,unsigned flags,unsigned *seed)
{
    SurfaceRecord *r=record(surface);
    assert(flags==3 && !seed && r->locks>r->unlocks && r->references);
    r->unlocks++;return 0;
}
static const void *retain_surface(const void *surface)
{ record((void *)surface)->references++;return surface; }
static void release_surface(const void *surface)
{ SurfaceRecord *r=record((void *)surface);assert(r->references);r->references--; }
static int surface_capture(ca_view_t *v, void *surface)
{
    unsigned *geometry=surface;
    assert(v && v->gc);
    if(surface_is_core) {
        SurfaceRecord *r=record(surface);
        assert(r->locks>r->unlocks && r->references);
    }
    if(fail_capture)return 0;
    v->ref=surface;v->base=0x1000;v->stride=geometry[0]*4;
    v->width=geometry[0];v->height=geometry[1];return 1;
}
#include "bind.h"
typedef struct {
    void *vt[5];
    void **block;
    void *current;
    unsigned geometry[2];
    unsigned binds,unbinds,presents,created;
    int refuse,empty;
} Drawable;
static int bind_drawable(void *p,unsigned format,void **block)
{
    Drawable *d=p;d->binds++;
    assert(format==CA_FOURCC_BGRA);
    /* This is CA's observed refusal when the previous binding is retained. */
    if(d->block || d->refuse)return 0;
    assert(ca_view_for_block(block));
    d->block=block;d->created=0;return 1;
}
static int unbind_drawable(void *p)
{
    Drawable *d=p;assert(d->block);d->unbinds++;
    if(d->created) {
        ca_view_t *v=ca_view_for_block(d->block);
        assert(v && v->ref==d->current);
        assert(((int (*)(void *,void *))d->block[2])(d->block,d->current));
        assert(!v->ref && !v->base);
    }
    d->block=NULL;return 1;
}
static void *next_drawable(void *p)
{
    Drawable *d=p;assert(d->block);
    if(d->empty)return NULL;
    if(!d->created) {
        assert(((int (*)(void *,void *))d->block[1])(d->block,d->geometry));
        d->created=1;d->current=d->geometry;
    }
    return d->geometry;
}
static int present_drawable(void *p,unsigned n)
{ Drawable *d=p;assert(d->block && n==1);d->presents++;return 1; }
static Drawable drawable(unsigned width,unsigned height)
{
    Drawable d={.vt={NULL,bind_drawable,unbind_drawable,next_drawable,present_drawable},
                .geometry={width,height}};
    return d;
}
int main(void)
{
    GuestGC *one=calloc(1,sizeof(*one)), *two=calloc(1,sizeof(*two));
    one->host=1;two->host=2;
    Drawable portrait=drawable(320,480), other=drawable(320,480), refused=drawable(480,320);
    assert(GLESBindView(one,&portrait,(void *)0x8058,NULL));
    assert(GLESBindView(two,&other,(void *)0x8058,NULL));
    ca_view_t *a=ca_view_for_gc(one,0),*b=ca_view_for_gc(two,0);
    assert(a!=b && a->width==320 && a->height==480);
    /* Diner's layoutSubviews destroys/recreates storage with new dimensions. */
    portrait.geometry[0]=480;portrait.geometry[1]=320;
    assert(GLESBindView(one,&portrait,(void *)0x8058,NULL));
    assert(portrait.binds==2 && portrait.unbinds==1 && portrait.presents==1);
    assert(a->width==480 && a->height==320 && a->stride==1920);
    assert(b->drawable==&other && other.binds==1 && !other.unbinds);
    /* No extra present when the previous frame was already submitted. */
    a->need_buffer=1;
    assert(GLESBindView(one,&portrait,(void *)0x8058,NULL));
    assert(portrait.presents==1 && portrait.unbinds==2);
    /* A rejected replacement must not keep freed surface metadata. */
    refused.refuse=1;
    assert(!GLESBindView(one,&refused,(void *)0x8058,NULL));
    assert(!ca_view_for_gc(one,0) && !a->base && !a->ref);
    assert(!refused.unbinds && !other.unbinds);
    /* Accepted binding with no usable surface must be released. */
    refused.refuse=0;refused.empty=1;
    assert(!GLESBindView(one,&refused,(void *)0x8058,NULL));
    assert(refused.unbinds==1 && !ca_view_for_gc(one,0));
    assert(GLESBindView(one,NULL,NULL,NULL));
    assert(GLESBindView(two,NULL,NULL,NULL));
    assert(other.unbinds==1 && !ca_view_for_gc(two,0));
    assert(GLESBindView(one,&portrait,(void *)0x8058,NULL));
    GLESDestroyGC(one);GLESDestroyGC(two);
    assert(!portrait.block && deleted==2);
    for(unsigned i=0;i<CA_MAX_VIEWS;i++)assert(!ca_views[i].gc);
    /* CoreSurface callbacks own mapping and retain until destroyBuffer.
     * These calls execute production acquire/release, not a mapping mock. */
    surface_is_core=1;
    p_IOSurfaceLock=lock_surface;p_IOSurfaceUnlock=unlock_surface;
    p_surface_retain=retain_surface;p_surface_release=release_surface;
    GuestGC *three=calloc(1,sizeof(*three));three->host=3;
    Drawable core=drawable(320,480);
    assert(GLESBindView(three,&core,(void *)0x8058,NULL));
    ca_view_t *c=ca_view_for_gc(three,0);
    SurfaceRecord *cr=record(core.geometry);
    assert(cr->locks==1 && cr->unlocks==0 && cr->references==1);
    /* Repeated creation of one callback/surface does not double-acquire. */
    assert(ca_create_buffer(c->block,core.geometry));
    assert(cr->locks==1 && cr->references==1);
    unsigned rotated[2]={480,320};
    assert(ca_create_buffer(c->block,rotated));
    core.current=rotated;
    SurfaceRecord *rr=record(rotated);
    assert(rr->locks==1 && rr->references==1);
    ca_destroy_buffer(c->block,core.geometry);
    assert(cr->unlocks==1 && cr->references==0);
    assert(c->ref==rotated); /* destroying old buffer preserves current one */
    unsigned rejected[2]={320,480};
    fail_lock=rejected;
    assert(!ca_create_buffer(c->block,rejected));
    assert(!record(rejected)->references && !record(rejected)->locks);
    fail_lock=NULL;fail_capture=1;
    assert(!ca_create_buffer(c->block,rejected));
    assert(record(rejected)->locks==1 && record(rejected)->unlocks==1);
    assert(!record(rejected)->references && c->ref==rotated);
    fail_capture=0;
    p_IOSurfaceUnlock=NULL;
    assert(!ca_create_buffer(c->block,rejected));
    assert(record(rejected)->locks==1); /* unavailable API refuses before lock */
    p_IOSurfaceUnlock=unlock_surface;
    GuestGC *four=calloc(1,sizeof(*four));four->host=4;
    Drawable shared=drawable(320,480);
    assert(GLESBindView(four,&shared,(void *)0x8058,NULL));
    ca_view_t *e=ca_view_for_gc(four,0);
    assert(ca_create_buffer(e->block,rotated));
    ca_destroy_buffer(e->block,shared.geometry);
    shared.current=rotated;
    assert(rr->locks==2 && rr->references==2);
    GLESDestroyGC(three);
    assert(rr->unlocks==1 && rr->references==1 && e->ref==rotated);
    GLESDestroyGC(four);
    assert(rr->unlocks==2 && !rr->references && !ca_surface_locks);
    for(unsigned i=0;i<16;i++) {
        assert(records[i].locks==records[i].unlocks);
        assert(!records[i].references);
    }
    assert(deleted==4);
    puts("CA drawable lifecycle and CoreSurface lock ownership: passed");
    return 0;
}
