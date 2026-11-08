/* The sensor-info report (0xD3) carries bcdVersion big-endian, as the Z1 model sends it and as the drivers
 * read it: the real k48's AppleMultitouchZ2SPI shows 0x0146 and the real n90's AppleMultitouchN1SPI 0x0079,
 * where the model's little-endian halfword showed 0x4601 and 0x7900.
 *
 * SLICE include/hw/arm/ipod_touch_multitouch.h define MT_REPORT_|MT_ENDIANNESS
 * SLICE include/hw/arm/ipod_touch_multitouch.h typedef MTSensorProfile
 * SLICE hw/arm/ipod_touch_multitouch.c fn prepare_short_control_response
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LOG_GUEST_ERROR 0
#define qemu_log_mask(...) ((void)0)
typedef struct IPodTouchMultitouchState {
    uint8_t *out_buffer;
    const struct MTSensorProfile *profile;
} IPodTouchMultitouchState;
#include "slice.h"

int main(void)
{
    static const uint16_t versions[] = { 0x146, 0x79 };
    uint8_t out[16];

    for (unsigned i = 0; i < 2; i++) {
        MTSensorProfile p = { .rows = 40, .cols = 50, .bcd_version = versions[i] };
        IPodTouchMultitouchState s = { .out_buffer = out, .profile = &p };

        prepare_short_control_response(&s, MT_REPORT_SENSOR_INFO);
        /* the report starts at out[3]: endianness, rows, columns, bcdVersion */
        assert(out[3] == MT_ENDIANNESS && out[4] == 40 && out[5] == 50);
        assert((out[6] << 8 | out[7]) == versions[i]);
    }
    puts("PASS: the sensor-info report carries bcdVersion big-endian (0x0146, 0x0079)");
    return 0;
}
