/* The H2FMI page geometry a guest's set_config or a snapshot can set: nand_read_page fills page + spare bytes, and
 * s5l8930_h2fmi.c's buffers hold FMI_MAX_PAGE (H2FMI_BUF), so the sum is bounded, not each part alone.
 *
 * SLICE:state hw/arm/s5l8930_iop.c range struct S5L8930IOPState { | \n};
 * SLICE hw/arm/s5l8930_iop.c define FMI_STATUS_OK|FMI_STATUS_PARAM|FMI_META_BYTES|FMI_MAX_PAGE|CMD_GET|ARG
 * SLICE hw/arm/s5l8930_iop.c fn fmi_set_config iop_post_load
 * CFLAGS -Wall -Werror -Wno-unused-function
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define NAND_BUSES 2
#define NAND_CES 8
#define IOP_VIC_COUNT 4
#define IOP_VIC_REGS 0x1000
#define IOP_MAX_ENDPOINTS 8
typedef int SysBusDevice, MemoryRegion, DeviceState, QEMUTimer, RunState, BlockBackend, VMChangeStateEntry;
#define LOG_GUEST_ERROR 1
#define LOG_UNIMP 2
#define qemu_log_mask(...) ((void)0)
#define qemu_loglevel_mask(m) 0
#define qemu_log(...) ((void)0)
#define DPRINTF(...) ((void)0)
#define error_report(...) ((void)0)
static uint32_t ldl_le_p(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
typedef struct S5L8930IOPState S5L8930IOPState;
#include "state.h"
};
#include "slice.h"

static uint32_t config(S5L8930IOPState *s, uint32_t page, uint32_t spare)
{
    uint8_t cmd[0x80] = { 0 };
    uint32_t words[][2] = { { 0x10, 0 }, { 0x1c, 128 }, { 0x24, page }, { 0x28, spare } };
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        memcpy(cmd + words[i][0], &words[i][1], 4);
    }
    return fmi_set_config(s, 0, cmd);
}

int main(void)
{
    S5L8930IOPState s = { 0 };
    /* 16 KiB PPN pages with their spare fill the buffer exactly; one byte more is refused */
    assert(config(&s, 0x4000, 0x4800) == FMI_STATUS_OK);
    assert(s.bytes_per_page[0] == 0x4000 && s.bytes_per_spare[0] == 0x4800);
    assert(config(&s, 0x4000, 0x4801) == FMI_STATUS_PARAM);
    /* each part fits alone, the sum (0x11000 bytes into 0x8800) does not */
    assert(config(&s, FMI_MAX_PAGE, FMI_MAX_PAGE) == FMI_STATUS_PARAM);
    assert(config(&s, FMI_MAX_PAGE + 1, 0) == FMI_STATUS_PARAM);
    assert(s.bytes_per_page[0] == 0x4000 && s.bytes_per_spare[0] == 0x4800);   /* a refused config changes nothing */

    /* a snapshot is held to the same bound */
    for (int bus = 0; bus < NAND_BUSES; bus++) {
        s.bytes_per_page[bus] = 0x800;
        s.bytes_per_spare[bus] = 0x40;
    }
    assert(iop_post_load(&s, 0) == 0);
    s.bytes_per_page[1] = FMI_MAX_PAGE;
    s.bytes_per_spare[1] = FMI_MAX_PAGE;
    assert(iop_post_load(&s, 0) == -EINVAL);
    puts("PASS: H2FMI page + spare is bounded by the page buffer in set_config and post-load");
    return 0;
}
