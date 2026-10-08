/* Actual CoreSurface drawable lifetime: mapping precedes getters; one owner per view/surface.
 *
 * SLICE:lifetime contrib/it-gles/mbxshim.c fn ca_view_for_block
 * SLICE:lifetime contrib/it-gles/mbxshim.c range typedef struct ca_surface_lock { | static int ca_create_buffer
 * SLICE:lifetime contrib/it-gles/mbxshim.c fn surface_capture ca_create_buffer ca_destroy_buffer ca_detach_view
 * CFLAGS -Wall -Wextra -Wno-unused-variable
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef struct { void *gc,*drawable,*block[8],*ref; unsigned base,stride,width,height,format; unsigned char need_buffer; } ca_view_t;
#define CA_MAX_VIEWS 4
#define CA_FOURCC_565L 1
#define CA_FOURCC_555L 2
static ca_view_t ca_views[CA_MAX_VIEWS];
static int surface_is_core=1, locks,unlocks,retains,releases,refusals,lock_error,unlock_error,allocation_error;
static int synchronous_destroy;
typedef struct { int mapped, valid, held; } Surface;
static void w(const char*x){(void)x;}
static void wd(unsigned x){(void)x;}
static void wx(unsigned long x){(void)x;}
static char *fourcc_text(unsigned x,char*b){(void)x;strcpy(b,"????");return b;}
static void refused(const char*a,const char*b,unsigned x){(void)a;(void)b;(void)x;refusals++;}
static void iosurface_init(void){}
static int lock_fn(void*p,unsigned flags,unsigned*seed){Surface*s=p;(void)seed;assert(flags==3);locks++;if(lock_error)return 1;s->mapped=1;s->held++;return 0;}
static int unlock_fn(void*p,unsigned flags,unsigned*seed){Surface*s=p;(void)seed;assert(flags==3);assert(s->held>0);s->held--;unlocks++;return unlock_error;}
static const void *retain_fn(const void*p){retains++;return p;}
static void release_fn(const void*p){(void)p;releases++;}
static void *base_fn(void*p){Surface*s=p;return s->mapped&&s->valid?(void*)0x1000:0;}
static unsigned width_fn(void*p){(void)p;return 320;}
static unsigned height_fn(void*p){(void)p;return 480;}
static unsigned stride_fn(void*p){(void)p;return 1280;}
static unsigned format_fn(void*p){(void)p;return 3;}
static void *(*p_IOSurfaceGetBaseAddress)(void*)=base_fn;
static unsigned (*p_IOSurfaceGetBytesPerRow)(void*)=stride_fn;
static unsigned (*p_IOSurfaceGetWidth)(void*)=width_fn;
static unsigned (*p_IOSurfaceGetHeight)(void*)=height_fn;
static unsigned (*p_IOSurfaceGetPixelFormat)(void*)=format_fn;
static int (*p_IOSurfaceLock)(void*,unsigned,unsigned*)=lock_fn;
static int (*p_IOSurfaceUnlock)(void*,unsigned,unsigned*)=unlock_fn;
static const void *(*p_surface_retain)(const void*)=retain_fn;
static void (*p_surface_release)(const void*)=release_fn;
static void *test_calloc(size_t n,size_t z){return allocation_error?0:calloc(n,z);}
#define calloc test_calloc
typedef int (*ca_present_fn)(void*,int);
typedef void (*ca_unbind_fn)(void*);
#include "lifetime.h"
static void unbind(void *d){(void)d;if(synchronous_destroy)ca_destroy_buffer(ca_views[0].block,ca_views[0].ref);assert(ca_views[0].gc);}
int main(void){
 ca_view_t *a=&ca_views[0],*b=&ca_views[1];Surface one={0,1,0},two={0,1,0},bad={0,0,0};
 a->gc=(void*)1;b->gc=(void*)2;
 assert(ca_create_buffer(a->block,&one));assert(one.held==1&&locks==1&&retains==1);
 assert(ca_create_buffer(a->block,&one));assert(locks==1);
 assert(ca_create_buffer(a->block,&two));assert(locks==2);
 assert(ca_create_buffer(b->block,&one));assert(one.held==2);
 ca_destroy_buffer(a->block,&one);assert(one.held==1&&a->ref==&two&&b->ref==&one);
 ca_destroy_buffer(a->block,&one);assert(unlocks==1);
 lock_error=1;int old=refusals;assert(!ca_create_buffer(a->block,&bad));assert(refusals==old+1&&bad.held==0);lock_error=0;
 old=refusals;assert(!ca_create_buffer(a->block,&bad));assert(refusals==old+1&&bad.held==0);
 allocation_error=1;int before=locks;assert(!ca_create_buffer(a->block,&bad));assert(locks==before);allocation_error=0;
 void *vt[5]={0,0,(void*)unbind,0,0};a->drawable=vt;a->need_buffer=1;synchronous_destroy=1;
 ca_detach_view(a);assert(!a->gc&&two.held==0);
 ca_detach_view(b);assert(one.held==0&&retains==releases&&ca_surface_locks==0);
 a->gc=(void*)1;surface_is_core=0;one.mapped=1;before=locks;assert(ca_create_buffer(a->block,&one));ca_detach_view(a);assert(locks==before);
 surface_is_core=1;a->gc=(void*)1;p_IOSurfaceLock=0;assert(!ca_create_buffer(a->block,&one));assert(!ca_surface_locks);p_IOSurfaceLock=lock_fn;
 assert(ca_create_buffer(a->block,&one));unlock_error=1;old=refusals;ca_destroy_buffer(a->block,&one);assert(refusals==old+1&&one.held==0&&ca_surface_locks==0&&retains==releases);
 puts("PASS actual create/capture/destroy/detach: lock ordering, rotation, layer ownership, failures, synchronous teardown, IOSurface unchanged");
}
