/* Native context isolation and sharegroup lifetime, from the real bridge and the shim's sharegroup and context
 * calls: GL state stays per context, textures and deleted buffers are shared within a group, independent groups
 * stay apart, and destruction, reboot and API negotiation leave nothing behind. Snapshot save and restore are out of
 * scope; only their registration hook is stubbed.
 *
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE:policy hw/arm/gles-host.c range static int gles_live_contexts; | static GLESHost gh_legacy;
 * SLICE:ops hw/arm/gles-host.c fn gles_buffer_destroy gles_buffer_bind gles_buffer_intern gles_buffer_forget
 * SLICE:ops hw/arm/gles-host.c range static GHashTable *gles_contexts, *gles_groups; | #ifdef GLES_HOST_ANGLE\nstatic void gles_snapshot_register
 * SLICE:ops2 hw/arm/gles-host.c range /* A guest reboot cannot send destruction calls | #endif\n\nvoid gles_host_reset
 * SLICE:ops2 hw/arm/gles-host.c fn gles_host_reset
 * SLICE:guestgc contrib/it-gles/mbxshim.c range typedef struct {\n | \nextern void *calloc
 * SLICE:sharegroup contrib/it-gles/mbxshim.c range static int GLESCreateSharegroup( | /*\n * The one that matters.
 * SLICE:sharegroup contrib/it-gles/mbxshim.c define GLES_N_SLOTS|GLES_N_HAND
 * SLICE:gc contrib/it-gles/mbxshim.c range static int GLESCreateGCWithAPI( | static int GLESDestroyGC(void *gc);
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
// SLICE:create hw/arm/gles-host.c range /* CGL */ | /*\n * Every request used to call
#include "ipod-gles.h"
typedef char Error;
static bool migration_busy;
#define qatomic_read g_atomic_int_get
#define qatomic_inc g_atomic_int_inc
#define qatomic_dec(p) g_atomic_int_add(p,-1)
#define qatomic_set g_atomic_int_set
static void error_setg(Error **e,const char *message) { *e=g_strdup(message); }
static void error_free(Error *e) { g_free(e); }
static int migrate_add_blocker_internal(Error **e,Error **error) {
 if(!migration_busy)return 0;
 g_clear_pointer(e,g_free);*error=g_strdup("busy");return -1;
}
static void migrate_del_blocker(Error **e) { g_clear_pointer(e,g_free); }
#include "policy.h"
/* The CGL backend's context create. */
#include "create.h"
#include "ops.h"
static void gles_snapshot_register(void) {}
#include "ops2.h"
#include "guestgc.h"
#define A(...) ((unsigned[]){__VA_ARGS__})
static bool older_host, reject_api;
static long long qc(unsigned slot, void *gc, unsigned argc, const unsigned *args)
{ if(reject_api && slot==GLES_OP_NEW_CONTEXT && argc==2)return -2; if(older_host && slot==GLES_OP_NEW_CONTEXT && argc==2)return -1; return gles_context_operation(slot,gc?((GuestGC*)gc)->host:0,argc,args); }
#include "sharegroup.h"
static void w(const char *s) {}
static void gles_hand_table(void **t) {}
static unsigned gles_fill(void **a, unsigned n, void *const *b) { return 0; }
#include "gc.h"
static GLESHost *select_context(unsigned id)
{
    GLESHost *s=g_hash_table_lookup(gles_contexts,GUINT_TO_POINTER(id));assert(s);
    gh_current=s;
    if(!s->cgl) assert(gles_platform_context_create());
    assert(!CGLSetCurrentContext(s->cgl));
    return s;
}
int main(void)
{
    void *guest_group=NULL;
    assert(!GLESCreateSharegroup(NULL));
    assert(GLESCreateSharegroup(&guest_group)==1 && guest_group);
    uint32_t group=((GuestGC*)guest_group)->host;
    uint32_t one=gles_context_operation(GLES_OP_NEW_CONTEXT,0,1,&group);
    uint32_t explicit_args[]={group,2};
    uint32_t two=gles_context_operation(GLES_OP_NEW_CONTEXT,0,2,explicit_args);
    explicit_args[1]=3;
    assert(gles_context_operation(GLES_OP_NEW_CONTEXT,0,2,explicit_args)==-1);
    explicit_args[1]=0;
    assert(gles_context_operation(GLES_OP_NEW_CONTEXT,0,2,explicit_args)==-1);
    assert(one!=two && one>=0x80000000);
    /* CGL state is saved with the snapshot (acc5e8e9d7): no migration blocker. */
    assert(gles_host_context_count()==2 && !gles_save_blocker);
    GLESHost *a=select_context(one);
    glEnable(GL_BLEND);a->vertex.enabled=1;
    GLuint texture;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    uint8_t pixel[]={7,29,61,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    glFinish();
    GLESHost *b=select_context(two);
    assert(a->api_version==0 && b->api_version==2);
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
    assert(!gles_host_context_count() && !gles_save_blocker);
    isolated=gles_context_operation(GLES_OP_NEW_SHAREGROUP,0,0,NULL);
    migration_busy=true;
    assert(gles_context_operation(GLES_OP_NEW_CONTEXT,0,1,&isolated)>=0x80000000);
    assert(gles_host_context_count()==1 && !gles_save_blocker);
    migration_busy=false;
    gles_host_reset();
    assert(GLESCreateSharegroup(&guest_group));
    void *known=NULL;
    assert(GLESCreateGCWithAPI(guest_group,NULL,NULL,&known,2));
    assert(((GuestGC*)known)->api==2);
    assert(select_context(((GuestGC*)known)->host)->api_version==2);
    assert(!gles_context_operation(GLES_OP_DELETE_CONTEXT,((GuestGC*)known)->host,0,NULL));free(known);
    older_host=true;
    assert(GLESCreateGCWithAPI(guest_group,NULL,NULL,&known,1));
    assert(((GuestGC*)known)->api==1);
    assert(select_context(((GuestGC*)known)->host)->api_version==0);
    assert(!gles_context_operation(GLES_OP_DELETE_CONTEXT,((GuestGC*)known)->host,0,NULL));free(known);
    assert(!GLESCreateGCWithAPI(guest_group,NULL,NULL,&known,3));
    older_host=false;reject_api=true;
    assert(!GLESCreateGCWithAPI(guest_group,NULL,NULL,&known,2));
    assert(!gles_host_context_count());
    assert(!GLESDestroySharegroup(guest_group));
    g_hash_table_destroy(gles_contexts);g_hash_table_destroy(gles_groups);
    puts("PASS: native GL state isolation, shared textures and deleted buffers, independent groups and destruction order");
}
