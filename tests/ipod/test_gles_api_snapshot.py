#!/usr/bin/env python3
"""Actual host field codec preserves explicit APIs; old snapshots remain unknown."""
from gles_harness import root, PRELUDE, build_and_run
snapshot = (root/'hw/arm/gles-host-snapshot.c.inc').read_text()
def function(name):
    start = snapshot.rfind('\nstatic ', 0, snapshot.index(name+'(')) + 1
    opening = snapshot.index('{', start)
    depth, end = 1, opening+1
    while depth:
        depth += (snapshot[end]=='{')-(snapshot[end]=='}')
        end += 1
    return snapshot[start:end]
codec = snapshot[snapshot.index('typedef struct { GByteArray'):snapshot.index('/* ---- per-context GL state')]
codec += ''.join(function(n) for n in ('snap_put_map','snap_get_map','snap_buffer','snap_put_array','snap_get_array','snap_put_host','snap_get_host'))
check = r'''
static void check(uint32_t api) {
    GLESHost original={.api_version=api,.inited=true,.fbo=17,.tex=31,.drawable_width=1024,.drawable_height=768,.program=91};
    original.vertex.ptr=0x123400;original.unpack_alignment=4;
    SnapW w={g_byte_array_new()};snap_put_host(&w,&original);
    GLESHost restored={0};SnapR r={w.b->data,w.b->len,0,false};
    snap_get_host(&r,&restored,4);
    assert(!r.bad && r.off==r.n && restored.api_version==api);
    assert(restored.fbo==17 && restored.program==91 && restored.vertex.ptr==0x123400 && restored.drawable_width==1024);
    g_hash_table_destroy(restored.fbo_drawable);
    /* Version 3 fields begin at inited, without the API word. */
    memset(&restored,0,sizeof restored);r=(SnapR){w.b->data+4,w.b->len-4,0,false};
    snap_get_host(&r,&restored,3);
    assert(!r.bad && r.off==r.n && restored.api_version==0 && restored.program==91 && restored.fbo==17);
    g_hash_table_destroy(restored.fbo_drawable);
    uint32_t invalid=3;memcpy(w.b->data,&invalid,4);
    memset(&restored,0,sizeof restored);r=(SnapR){w.b->data,w.b->len,0,false};
    snap_get_host(&r,&restored,4);assert(r.bad);
    g_hash_table_destroy(restored.fbo_drawable);g_byte_array_free(w.b,true);
}
int main(void) { check(0);check(1);check(2);puts("PASS: real snapshot field codec preserves API1/2, accepts v3 unknown and rejects corrupt API"); }
'''
build_and_run(PRELUDE+codec+check,'it-gles-api-snapshot-')
