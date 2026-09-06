#ifndef IPOD_TOUCH_NOR_SPI_H
#define IPOD_TOUCH_NOR_SPI_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/ssi/ssi.h"
#include "hw/hw.h"

#define TYPE_IPOD_TOUCH_NOR_SPI                "ipodtouch.norspi"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchNORSPIState, IPOD_TOUCH_NOR_SPI)

#define NOR_FLASH_SIZE (1u << 20)
#define NOR_PAGE_SIZE 256
#define NOR_ERASE_32K 0x52
#define NOR_ERASE_64K 0xd8

#define NOR_WRITE_TO_STATUS_REG 0x1
#define NOR_WRITE_DATA_CMD 0x2
#define NOR_READ_DATA_CMD  0x3
#define NOR_DISABLE_WRITE  0x4
#define NOR_GET_STATUS_CMD 0x5
#define NOR_ENABLE_WRITE   0x6
#define NOR_ERASE_BLOCK    0x20
#define NOR_GET_JEDECID    0x9F

typedef struct IPodTouchNORSPIState {
    SSIPeripheral ssidev;
    char *nor_path;
    const char *boot_args;
    uint32_t cur_cmd;
    uint8_t nor_data[NOR_FLASH_SIZE];
    uint32_t nor_size;
    uint8_t write_enabled;
    uint32_t nor_read_ind;
    uint8_t address_bytes, page_offset, stream_offset, status;
    uint16_t data_count;
    uint8_t page[NOR_PAGE_SIZE];
    bool command_active;
    bool nor_initialized;
} IPodTouchNORSPIState;

#endif