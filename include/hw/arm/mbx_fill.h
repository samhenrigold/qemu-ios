/* MIT; Copyright (c) 2026 j0shua-SYSON; adapted S5LBox state/bus subset.
 * QEMU adapter owns reset/migration and observer-aware writes. */
#ifndef HW_ARM_MBX_FILL_H
#define HW_ARM_MBX_FILL_H
#include <stdbool.h>
#include <stdint.h>
typedef struct MBXFillState {
    uint32_t roots[8];
    uint32_t ring[0x10000 / 4];
    uint32_t pending_offset;
    uint32_t pending_count;
    uint32_t pending_mask;
} MBXFillState;
typedef struct MBXFillBus {
    void *ctx;
    bool mmu_enabled; /* hardware control bit, derived by the QEMU adapter */
    uint8_t *(*host_ram)(void *ctx, uint32_t pa, uint32_t len);
    void (*write32)(void *ctx, uint32_t pa, uint32_t value);
} MBXFillBus;
typedef enum { MBX_FILL_IGNORED, MBX_FILL_DONE, MBX_FILL_REJECTED } MBXFillResult;
void mbx_fill_reset(MBXFillState *s);
MBXFillResult mbx_fill_write(MBXFillState *s, const MBXFillBus *bus,
                           uint32_t offset, uint32_t value, const char **why);
#endif
