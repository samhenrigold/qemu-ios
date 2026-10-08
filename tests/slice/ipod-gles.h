/* The host GL bridge's own platform block, constants, types, GLESHost and refusals, cut from hw/arm/gles-host.c,
 * so a check compiles against the real layout. A check that includes this carries these directives:
 *
 *   SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 *   SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 *   CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 *   PKG glib-2.0
 *
 * and supplies gles_guest_rw (declared by gles.h) with its own fake guest memory, plus
 * whatever other stubs the functions it cuts need. A check that exercises gles-debug's paint defines
 * GLES_TEST_REAL_DEBUG and cuts the real ones.
 */
#define GL_SILENCE_DEPRECATION
#include <glib.h>
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct CPUState CPUState;
typedef uint64_t vaddr;
typedef uint64_t hwaddr;
typedef uint64_t ram_addr_t;
#define QEMU_BUILD_BUG_ON(x) _Static_assert(!(x), #x)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define trace_event_get_state_backends(id) 0
#define TRACE_PRINTF(fn, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#include "hw/arm/guest-services/gles.h"
#include "gles-prelude.h"
static GLESHost gh_legacy;
static GLESHost *gh_current = &gh_legacy;
#define gh (*gh_current)
bool gles_guest_fault_pending(void) { return false; }
#include "gles-refusals.h"
#ifndef GLES_TEST_REAL_DEBUG
static void gles_debug_mark(void) {}
static void gles_debug_texture(GLenum target) {}
#endif
