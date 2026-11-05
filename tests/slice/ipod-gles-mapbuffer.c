/* glMapBuffer and glUnmapBuffer: the host copies a buffer into the memory the guest shim gives it, whatever the access,
 * and back at the unmap unless read-only; glGetBufferParameteriv answers the shim's size query and the mapped state.
 * Answered 0, as before, Asphalt 8 memcpy'd into a null pointer.
 *
 * SLICE hw/arm/gles-host.c range case GLES_SLOT_MAP_BUFFER: | case GLES_SLOT_GET_INTEGERV:
 * CFLAGS -I$ROOT/include
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GL_INVALID_OPERATION 0x502
#define GL_INVALID_ENUM 0x500
#define GL_OUT_OF_MEMORY 0x505
#define GLES_SLOT_MAP_BUFFER 649
#define GLES_SLOT_UNMAP_BUFFER 650
#define GLES_SLOT_GET_BUFFER_PARAMETERIV 651
typedef int CPUState;
typedef uint64_t vaddr;
typedef struct {
    void *data;
    size_t size;
    uint32_t usage, map_va, map_size, map_access;
} GLESBuffer;
static GLESBuffer *bound;
static GLESBuffer *gles_buffer_bound(uint32_t target) { return target == 0x8892 ? bound : NULL; }
static int rejected, refused;
static int64_t gles_reject(int e) { rejected = e; return 0; }
static bool gles_refuse(const char *what) { refused++; return false; }
static bool gles_guest_fault_pending(void) { return false; }
#define GUEST 0x10000u
static uint8_t guest[256];
static bool guest_fails;
int gles_guest_rw(CPUState *cpu, vaddr addr, void *data, size_t n, bool write)
{
    if (guest_fails || addr < GUEST || addr + n > GUEST + sizeof(guest)) return -1;
    if (write) memcpy(guest + (addr - GUEST), data, n);
    else memcpy(data, guest + (addr - GUEST), n);
    return 0;
}
static int64_t call(uint32_t slot, uint32_t argc, const uint32_t *a)
{
    CPUState *cpu = NULL;
    switch (slot) {
#include "slice.h"
    default: abort();
    }
    return 0;
}
#define CALL(slot, ...) call(slot, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__})
enum { ARRAY = 0x8892, WRITE_ONLY = 0x88B9, READ_ONLY = 0x88B8 };

int main(void)
{
    static uint8_t bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    GLESBuffer b = { .data = bytes, .size = sizeof(bytes), .usage = 0x88E8 };

    /* Nothing bound or an empty buffer: the error, and a null pointer. */
    assert(!CALL(GLES_SLOT_MAP_BUFFER, ARRAY, WRITE_ONLY, GUEST) && rejected == GL_INVALID_OPERATION);
    bound = &b;

    /* The shim's size query. */
    assert(!CALL(GLES_SLOT_GET_BUFFER_PARAMETERIV, ARRAY, 0x8764, GUEST) && *(uint32_t *)guest == 8);
    assert(!CALL(GLES_SLOT_GET_BUFFER_PARAMETERIV, ARRAY, 0x8765, GUEST) && *(uint32_t *)guest == 0x88E8);
    assert(!CALL(GLES_SLOT_GET_BUFFER_PARAMETERIV, ARRAY, 0x88BC, GUEST) && *(uint32_t *)guest == 0);
    rejected = 0;
    assert(!CALL(GLES_SLOT_GET_BUFFER_PARAMETERIV, ARRAY, 0x1234, GUEST) && rejected == GL_INVALID_ENUM);

    /* A guest package from before the shim's half: refused, not a crash. */
    assert(!CALL(GLES_SLOT_MAP_BUFFER, ARRAY, WRITE_ONLY) && refused == 1 && rejected == GL_OUT_OF_MEMORY);

    /* Write-only still copies in, so a mapping that fills part of the buffer keeps the rest. */
    memset(guest, 0, sizeof(guest));
    assert(CALL(GLES_SLOT_MAP_BUFFER, ARRAY, WRITE_ONLY, GUEST) == GUEST);
    assert(!memcmp(guest, bytes, 8));
    assert(CALL(GLES_SLOT_MAP_BUFFER, ARRAY, WRITE_ONLY, GUEST + 64) == GUEST);   /* mapped: the same pointer */
    assert(!CALL(GLES_SLOT_GET_BUFFER_PARAMETERIV, ARRAY, 0x88BC, GUEST + 32) && *(uint32_t *)(guest + 32) == 1);
    guest[0] = 0x55;
    assert(CALL(GLES_SLOT_UNMAP_BUFFER, ARRAY) == 1);
    assert(bytes[0] == 0x55 && bytes[1] == 2 && !b.map_va);
    rejected = 0;
    assert(!CALL(GLES_SLOT_UNMAP_BUFFER, ARRAY) && rejected == GL_INVALID_OPERATION);   /* not mapped */

    /* Read-only: nothing comes back. */
    assert(CALL(GLES_SLOT_MAP_BUFFER, ARRAY, READ_ONLY, GUEST) == GUEST);
    guest[1] = 0x66;
    assert(CALL(GLES_SLOT_UNMAP_BUFFER, ARRAY) == 1 && bytes[1] == 2);

    /* A guest fault: no pointer on the map; on the unmap, -1 to be reissued, the mapping kept. */
    guest_fails = true;
    assert(!CALL(GLES_SLOT_MAP_BUFFER, ARRAY, WRITE_ONLY, GUEST) && !b.map_va);
    guest_fails = false;
    assert(CALL(GLES_SLOT_MAP_BUFFER, ARRAY, WRITE_ONLY, GUEST) == GUEST);
    guest_fails = true;
    assert(CALL(GLES_SLOT_UNMAP_BUFFER, ARRAY) == -1 && b.map_va);
    guest_fails = false;
    assert(CALL(GLES_SLOT_UNMAP_BUFFER, ARRAY) == 1);

    /* Storage changed under a mapping (glBufferData ends it): an unmap's copy can't overrun the new size. */
    assert(CALL(GLES_SLOT_MAP_BUFFER, ARRAY, WRITE_ONLY, GUEST) == GUEST);
    b.size = 4;
    assert(CALL(GLES_SLOT_UNMAP_BUFFER, ARRAY) == 1 && bytes[4] == 5);
    puts("PASS: glMapBuffer copies in whatever the access, glUnmapBuffer copies back unless read-only, faults reissue");
    return 0;
}
