/* The host's snapshot field codec preserves explicit APIs; old snapshots remain unknown.
 *
 * A snapshot file is input like any other: a restored context takes no index, size, layout or count the live
 * bridge would not have accepted (a drawable past 2048 squared, an array layout gl*Pointer refuses, a client
 * texture unit or a pixel-store alignment out of range, a matrix stack or unit count past what the capture
 * bounds), and the load fails instead.
 *
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host-snapshot.c.inc range typedef struct { GByteArray | /* ---- per-context GL state
 * SLICE hw/arm/gles-host.c define GLES_MAX_BUFFER_BYTES
 * SLICE hw/arm/gles-host.c define GLES_FIXED
 * SLICE hw/arm/gles-host.c fn gles_type_size gles_array_layout_ok gles_palette_reset
 * SLICE hw/arm/gles-host-snapshot.c.inc range /* ---- per-context GL state | /* Every matrix of the stack
 * SLICE hw/arm/gles-host-snapshot.c.inc fn snap_gl_ok
 * SLICE hw/arm/gles-host-snapshot.c.inc fn snap_put_map snap_get_map snap_buffer snap_put_array snap_get_array snap_alignment_ok snap_put_host snap_get_host snap_put_buffers snap_get_buffers
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
#include "ipod-gles.h"
#include "slice.h"
static void check_buffers(void) {
    uint8_t bytes[16]={1,2,3};
    GLESBuffer b={.name=7,.size=16,.data=bytes,.usage=GL_DYNAMIC_DRAW,
        .map_va=0x10000,.map_size=8,.map_access=2,.map_offset=4,.map_range=true};
    GHashTable *src=g_hash_table_new(g_direct_hash,g_direct_equal);
    GHashTable *dst=g_hash_table_new(g_direct_hash,g_direct_equal);
    g_hash_table_insert(src,GUINT_TO_POINTER(7),&b);
    SnapW w={g_byte_array_new()}; snap_put_buffers(&w,src);
    SnapR r={w.b->data,w.b->len,0,false}; snap_get_buffers(&r,dst,5);
    GLESBuffer *got=g_hash_table_lookup(dst,GUINT_TO_POINTER(7));
    assert(!r.bad && r.off==r.n && got && got->usage==GL_DYNAMIC_DRAW);
    assert(got->map_range && got->map_offset==4 && got->map_size==8 && got->map_va==0x10000);
    assert(!memcmp(got->data,bytes,16)); gles_buffer_bytes-=got->size;g_free(got->data);g_free(got);
    g_hash_table_remove_all(dst);
    r=(SnapR){w.b->data,w.b->len-24,0,false};snap_get_buffers(&r,dst,4);
    got=g_hash_table_lookup(dst,GUINT_TO_POINTER(7));
    assert(!r.bad && r.off==r.n && got && !got->map_va && !got->map_range);
    gles_buffer_bytes-=got->size;g_free(got->data);g_free(got);
    g_hash_table_destroy(src);g_hash_table_destroy(dst);g_byte_array_free(w.b,true);
}
static void check(uint32_t api) {
    GLESHost original={.api_version=api,.inited=true,.fbo=17,.tex=31,.drawable_width=1024,.drawable_height=768,.program=91,.drawable_renderbuffer=73,.clip_enabled=0xa5};
    original.vertex.ptr=0x123400;original.unpack_alignment=4;
    original.palette_on=original.palette_mode=true;original.palette_current=10;original.palette[10][13]=2.5f;
    original.matrix_index=(GLESArray){.enabled=1,.size=1,.type=GL_UNSIGNED_BYTE,.stride=0x1c,.ptr=0x10,.client_state=GL_MATRIX_INDEX_ARRAY_OES};
    SnapW w={g_byte_array_new()};snap_put_host(&w,&original);
    GLESHost restored={0};SnapR r={w.b->data,w.b->len,0,false};
    snap_get_host(&r,&restored,8);
    assert(!r.bad && r.off==r.n && restored.api_version==api);
    assert(restored.fbo==17 && restored.program==91 && restored.vertex.ptr==0x123400 && restored.drawable_width==1024 && restored.drawable_renderbuffer==73 && restored.clip_enabled==0xa5);
    assert(restored.palette_on && restored.palette_mode && restored.palette_current==10 && restored.palette[10][13]==2.5f);
    assert(restored.matrix_index.enabled && restored.matrix_index.stride==0x1c && restored.matrix_index.ptr==0x10);
    g_hash_table_destroy(restored.fbo_drawable);
    /* v7 has no OES_matrix_palette: a new context's (disabled, identity) */
    size_t v7=w.b->len-(2*7*4+3*4+4+sizeof original.palette);
    memset(&restored,0,sizeof restored);r=(SnapR){w.b->data,v7,0,false};
    snap_get_host(&r,&restored,7);
    assert(!r.bad && r.off==r.n && !restored.palette_on && !restored.palette_mode && restored.palette[10][13]==0 && restored.palette[10][15]==1);
    assert(restored.weight.client_state==GL_WEIGHT_ARRAY_OES && !restored.matrix_index.enabled);
    g_hash_table_destroy(restored.fbo_drawable);
    /* v5 has shared-layer storage, but no clip-enable mask. */
    GByteArray *v5=g_byte_array_new();g_byte_array_append(v5,w.b->data,36);
    g_byte_array_append(v5,w.b->data+40,v7-40);
    memset(&restored,0,sizeof restored);r=(SnapR){v5->data,v5->len,0,false};
    snap_get_host(&r,&restored,5);assert(!r.bad&&r.off==r.n&&restored.drawable_renderbuffer==73&&!restored.clip_enabled);
    g_hash_table_destroy(restored.fbo_drawable);g_byte_array_free(v5,true);
    /* v4 has no shared-layer word and its PVRTC metadata ends before immutable_levels. */
    GByteArray *old=g_byte_array_new();
    g_byte_array_append(old,w.b->data,32);
    g_byte_array_append(old,w.b->data+40,v7-40-4);
    uint32_t old_pvrtc=offsetof(GLESPVRTC,immutable_levels);
    memcpy(old->data+old->len-old_pvrtc-4,&old_pvrtc,4);
    memset(&restored,0,sizeof restored);r=(SnapR){old->data,old->len,0,false};
    snap_get_host(&r,&restored,4);
    assert(!r.bad && r.off==r.n && restored.api_version==api && !restored.drawable_renderbuffer);
    g_hash_table_destroy(restored.fbo_drawable);
    memset(&restored,0,sizeof restored);r=(SnapR){old->data+4,old->len-4,0,false};
    snap_get_host(&r,&restored,3);
    assert(!r.bad && r.off==r.n && restored.api_version==0 && restored.program==91 && restored.fbo==17);
    g_hash_table_destroy(restored.fbo_drawable);g_byte_array_free(old,true);
    uint32_t invalid=3;memcpy(w.b->data,&invalid,4);
    memset(&restored,0,sizeof restored);r=(SnapR){w.b->data,w.b->len,0,false};
    snap_get_host(&r,&restored,6);assert(r.bad);
    g_hash_table_destroy(restored.fbo_drawable);g_byte_array_free(w.b,true);
}
/* One field of a good saved context changed: the load fails. */
static bool restores(void (*change)(GLESHost *)) {
    GLESHost h={.inited=true,.drawable_width=1024,.drawable_height=768,.unpack_alignment=4};
    h.vertex.client_state=GL_VERTEX_ARRAY;h.vertex.size=3;h.vertex.type=GL_FLOAT;h.vertex.stride=12;
    change(&h);
    SnapW w={g_byte_array_new()};snap_put_host(&w,&h);
    GLESHost restored={0};SnapR r={w.b->data,w.b->len,0,false};
    snap_get_host(&r,&restored,8);
    g_hash_table_destroy(restored.fbo_drawable);g_byte_array_free(w.b,true);
    return !r.bad;
}
static void same(GLESHost *h) {}
static void drawable(GLESHost *h) { h->drawable_width=4096; }
static void no_drawable(GLESHost *h) { h->drawable_height=0; }
static void stride(GLESHost *h) { h->vertex.stride=0x80000000u; }
static void components(GLESHost *h) { h->attr[3].size=0x40000001; h->attr[3].type=0x140C; }
static void client_state(GLESHost *h) { h->color.client_state=GL_VERTEX_ARRAY; }
static void unit(GLESHost *h) { h->client_active_unit=GLES_MAX_TEXUNITS; }
static void alignment(GLESHost *h) { h->pack_alignment=3; }
static void palette_index(GLESHost *h) { h->palette_current=GLES_MAX_PALETTE; }
static void index_type(GLESHost *h) { h->matrix_index=(GLESArray){.size=1,.type=GL_FLOAT}; }
static void weight_units(GLESHost *h) { h->weight=(GLESArray){.size=GLES_MAX_VERTEX_UNITS+1,.type=GL_FLOAT}; }
static void check_gl(void) {
    GLESCtxGL s={.units=2,.mv_depth=1,.proj_depth=1};
    s.unit[0].mat_depth=s.unit[1].mat_depth=1;
    assert(snap_gl_ok(&s));
    s.mv_depth=SNAP_MV_DEPTH+1;assert(!snap_gl_ok(&s));s.mv_depth=1;
    s.proj_depth=0;assert(!snap_gl_ok(&s));s.proj_depth=1;
    s.unit[1].mat_depth=SNAP_AUX_DEPTH+1;assert(!snap_gl_ok(&s));s.unit[1].mat_depth=1;
    s.units=GLES_MAX_TEXUNITS+1;assert(!snap_gl_ok(&s));
}
int main(void) {
    assert(restores(same));
    assert(!restores(drawable) && !restores(no_drawable) && !restores(stride) && !restores(components));
    assert(!restores(client_state) && !restores(unit) && !restores(alignment));
    assert(!restores(palette_index) && !restores(index_type) && !restores(weight_units));
    check_gl();check_buffers();
    check(0);check(1);check(2);puts("PASS: real snapshot field codec preserves API1/2, accepts v3 unknown, rejects corrupt API and out-of-range state"); }
