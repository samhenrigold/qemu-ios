#!/usr/bin/env python3
"""Compile the actual GLES boundary handlers with checked host-GL stand-ins.

No guest/assets/context required. ASan guards allocations while the stand-ins
write the host API's full result (glGet has no destination-capacity argument).
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
src = (ROOT / 'hw/arm/gles-host.c').read_text()


def function(name):
    start = src.rfind('\nstatic ', 0, src.index(name + '(')) + 1
    opening = src.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    return src[start:end]


def cases(first, following):
    return src[src.index('    case ' + first + ':'):src.index('    case ' + following + ':')]


code = r'''
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define GLES_MAX_TEX_BYTES ((size_t)4096 * 4096 * 4)
#define g_realloc realloc
#define GLES_SLOT_GET_INTEGERV 1
#define GLES_SLOT_GET_FLOATV 2
#define GLES_SLOT_PIXEL_STOREI 3
#define GLES_SLOT_READ_PIXELS 4
#define GLES_SLOT_GET_ERROR 5
typedef int CPUState;
typedef struct { uint32_t name; } GLESBuffer;
static struct {
    const GLESBuffer *array_buffer, *element_buffer;
    struct { const GLESBuffer *vbo; } vertex, normal, color, texcoord[8];
    unsigned client_active_unit;
    uint32_t pack_alignment, unpack_alignment, bound_framebuffer, bound_renderbuffer;
    GLenum error;
    uint8_t *txbuf;
    size_t txbuf_size;
} gh;
static unsigned host_queries, host_reads, host_pack = 4;
static unsigned char guest[256];
static size_t copied;
static void host_integer(GLenum pname, GLint *v) {
    host_queries++;
    /* A host-specific query longer than any guest result. Must never arrive. */
    unsigned count = pname == GL_COMPRESSED_TEXTURE_FORMATS ? 64 :
                     pname == GL_MODELVIEW_MATRIX ? 16 : 1;
    for (unsigned i = 0; i < count; i++) v[i] = 100 + i;
}
static void host_float(GLenum pname, GLfloat *v) {
    host_queries++;
    unsigned count = pname == GL_COMPRESSED_TEXTURE_FORMATS ? 64 :
                     pname == GL_MODELVIEW_MATRIX ? 16 : 1;
    for (unsigned i = 0; i < count; i++) v[i] = 100 + i;
}
static void host_store(GLenum pname, GLint align) {
    assert(pname == GL_PACK_ALIGNMENT);
    assert(align == 1 || align == 2 || align == 4 || align == 8);
    host_pack = align;
}
static void host_read(GLint x, GLint y, GLsizei w, GLsizei h,
                      GLenum format, GLenum type, void *dst) {
    host_reads++;
    size_t bpp = format == GL_RGB ? 3 : 4;
    if (type == GL_FLOAT) bpp *= sizeof(float);
    size_t stride = (w * bpp + host_pack - 1) & ~(host_pack - 1);
    for (int row = 0; row < h; row++) memset((char *)dst + row * stride, 0x42, w * bpp);
}
static int cpu_memory_rw_debug(CPUState *cpu, uint32_t addr, uint8_t *data, size_t n, int write) {
    assert(n <= sizeof(guest));
    assert(write);
    memcpy(guest, data, n); copied = n;
    return 0;
}
#define glGetIntegerv host_integer
#define glGetFloatv host_float
#define glPixelStorei host_store
#define glReadPixels host_read
#define glGetError() GL_NO_ERROR
'''
start = src.index('static const GLint gles_compressed_formats[]')
code += src[start:src.index('};', start) + 2] + '\n'
for name in ['gles_texel_bytes', 'gles_unpack', 'gles_image_bytes', 'gles_reject', 'gles_query_count']:
    code += function(name) + '\n'
code += 'static int64_t dispatch(unsigned slot, const uint32_t *a) { CPUState *cpu = NULL; switch(slot) {\n'
code += cases('GLES_SLOT_GET_INTEGERV', 'GLES_SLOT_MATRIX_MODE')
code += cases('GLES_SLOT_PIXEL_STOREI', 'GLES_SLOT_SCISSOR')
code += cases('GLES_SLOT_READ_PIXELS', 'GLES_SLOT_FOGF')
start = src.index('    case GLES_SLOT_GET_ERROR:')
code += src[start:src.index('\n    }', start) + 6]
code += 'default: abort(); } }\n'
code += r'''
int main(void) {
    assert(gles_texel_bytes(GL_RGBA, GL_FLOAT) == 0);
    assert(gles_texel_bytes(GL_RGB, GL_UNSIGNED_SHORT_4_4_4_4) == 0);
    assert(gles_texel_bytes(GL_RGB, GL_UNSIGNED_SHORT_5_6_5) == 2);
    assert(gles_image_bytes(3, 2, 3, 4) == 21);
    assert(gles_image_bytes(3, 2, 3, 1) == 18);
    assert(gles_image_bytes(UINT32_MAX, UINT32_MAX, 4, 8) == SIZE_MAX);
    assert(gles_image_bytes(4096, 4096, 4, 4) == GLES_MAX_TEX_BYTES);
    assert(gles_image_bytes(1, 1, 4, 3) == SIZE_MAX);
    uint32_t a[9] = {GL_UNPACK_ALIGNMENT, 1};
    assert(dispatch(GLES_SLOT_PIXEL_STOREI, a) == 0);
    a[1] = 3;
    assert(dispatch(GLES_SLOT_PIXEL_STOREI, a) == -1);
    assert(gh.unpack_alignment == 1);
    assert(dispatch(GLES_SLOT_GET_ERROR, a) == GL_INVALID_VALUE);
    assert(dispatch(GLES_SLOT_GET_ERROR, a) == GL_NO_ERROR);
    uint32_t read[] = {0, 0, 3, 2, GL_RGB, GL_UNSIGNED_BYTE, 1};
    assert(dispatch(GLES_SLOT_READ_PIXELS, read) == 0);
    assert(copied == 21 && host_pack == 4 && host_reads == 1);
    assert(guest[9] == 0 && guest[10] == 0 && guest[11] == 0);
    a[0] = GL_PACK_ALIGNMENT; a[1] = 1;
    assert(dispatch(GLES_SLOT_PIXEL_STOREI, a) == 0);
    assert(dispatch(GLES_SLOT_READ_PIXELS, read) == 0);
    assert(copied == 18 && host_pack == 1);
    read[5] = GL_FLOAT;
    assert(dispatch(GLES_SLOT_READ_PIXELS, read) == -1 && host_reads == 2);
    dispatch(GLES_SLOT_GET_ERROR, a);
    read[5] = GL_UNSIGNED_BYTE; read[2] = read[3] = UINT32_MAX;
    assert(dispatch(GLES_SLOT_READ_PIXELS, read) == -1 && host_reads == 2);
    dispatch(GLES_SLOT_GET_ERROR, a);
    a[0] = GL_COMPRESSED_TEXTURE_FORMATS; a[1] = 1;
    assert(dispatch(GLES_SLOT_GET_INTEGERV, a) == 0);
    assert(copied == sizeof(gles_compressed_formats) && host_queries == 0);
    assert(memcmp(guest, gles_compressed_formats, copied) == 0);
    assert(dispatch(GLES_SLOT_GET_FLOATV, a) == 0 && host_queries == 0);
    GLfloat f; memcpy(&f, guest, sizeof(f)); assert(f == 0x8C00);
    a[0] = GL_NUM_COMPRESSED_TEXTURE_FORMATS;
    assert(dispatch(GLES_SLOT_GET_INTEGERV, a) == 0);
    GLint i; memcpy(&i, guest, sizeof(i)); assert(i == 14);
    a[0] = GL_MODELVIEW_MATRIX;
    assert(dispatch(GLES_SLOT_GET_INTEGERV, a) == 0 && copied == 64);
    memcpy(&i, guest + 60, sizeof(i)); assert(i == 115);
    assert(dispatch(GLES_SLOT_GET_FLOATV, a) == 0 && copied == 64);
    memcpy(&f, guest + 60, sizeof(f)); assert(f == 115);
    unsigned before = host_queries;
    a[0] = 0xdeadbeef;
    assert(dispatch(GLES_SLOT_GET_FLOATV, a) == -1 && host_queries == before);
    assert(gles_query_count(GL_CURRENT_NORMAL) == 3);
    assert(gles_query_count(GL_COLOR_WRITEMASK) == 4);
    assert(gles_query_count(GL_POINT_DISTANCE_ATTENUATION) == 3);
    assert(gles_query_count(GL_GENERATE_MIPMAP_HINT) == 1);
    GLESBuffer buffer = { .name = 123 };
    gh.array_buffer = &buffer;
    a[0] = GL_ARRAY_BUFFER_BINDING;
    assert(dispatch(GLES_SLOT_GET_INTEGERV, a) == 0);
    memcpy(&i, guest, sizeof(i)); assert(i == 123);
    free(gh.txbuf);
    puts("PASS: GLES pixel bounds, independent alignment, query cardinality and rejected host queries");
}
'''
with tempfile.TemporaryDirectory(prefix='gles-boundaries-') as tmp:
    c = Path(tmp) / 'check.c'
    c.write_text(code)
    exe = Path(tmp) / 'check'
    subprocess.run(['clang', '-std=c11', '-fsanitize=address,undefined', '-g', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
