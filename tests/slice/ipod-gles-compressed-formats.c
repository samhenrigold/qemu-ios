/* glGet's compressed texture formats are the context's API's, as the SGX driver lists them: ES 2.0 has no
 * OES_compressed_paletted_texture, so its context lists the four PVRTC formats alone (NUM_COMPRESSED_TEXTURE_FORMATS
 * 4), and ES 1.1 adds the ten paletted ones (14). Every context used to list all 14, so an ES 2.0 app sized its
 * buffer by a count the device never gives. The front end's limit table (gles_gpus.h) says the same, and the
 * drawable cap is the largest GPU's renderbuffer limit, the SGX 543MP3's 4096.
 *
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host.c range static const GLint gles_compressed_formats[] | /* glGet has no capacity argument.
 * SLICE hw/arm/gles-host.c fn gles_query_count
 * SLICE:gpus contrib/it-gles/gles_gpus.h range enum { FE_MBX | /* Already on the MBX in 2.2.1
 * SLICE:gpus contrib/it-gles/gles_gpus.h range static const struct { unsigned short pname | /* glGetIntegerv, glGetFloatv
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
#include "ipod-gles.h"
int gles_guest_rw(CPUState *cpu, vaddr a, void *p, size_t n, bool write) { return -1; }
#include "slice.h"
#include "gpus.h"

int main(void)
{
    float v[2] = { 14, 0 };
    gh.api_version = 1;
    assert(gles_query_count(GL_COMPRESSED_TEXTURE_FORMATS) == 14 && gles_compressed_count() == 14);
    gh.api_version = 2;
    assert(gles_query_count(GL_COMPRESSED_TEXTURE_FORMATS) == 4 && gles_compressed_count() == 4);
    for (unsigned i = 0; i < 4; i++) assert(gles_compressed_formats[i] >= 0x8C00 && gles_compressed_formats[i] <= 0x8C03);
    assert(fe_limit(FE_SGX2, 0x86A2, v, 1) == 1 && v[0] == 4);
    v[0] = 14; assert(fe_limit(FE_SGX1, 0x86A2, v, 1) == 1 && v[0] == 14);
    v[0] = 16384; assert(fe_limit(FE_543_2, 0x84E8, v, 1) == 1 && v[0] == GLES_FB_MAX);
    puts("PASS: ES 2.0 lists the PVRTC formats alone; the drawable cap is the 543's renderbuffer limit");
    return 0;
}
