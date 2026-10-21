/*
 * The GL bridge on ANGLE (gles-host-angle.c.inc): gles-host.c built a second time, beside the CGL one, its entry
 * points renamed gles_angle_*. gles-host.c's own entry points forward to these when a boot picks ANGLE
 * (-M ...,gl-renderer=angle) and ANGLE opens.
 *
 * Copyright (c) 2026 the qemu-ios contributors.
 */
#define GLES_HOST_ANGLE 1
#define gles_host_call gles_angle_host_call
#define gles_host_stats gles_angle_host_stats
#define gles_host_set_allowed gles_angle_host_set_allowed
#define gles_host_reset gles_angle_host_reset
#define gles_host_context_count gles_angle_host_context_count
#define gles_host_refuse gles_angle_host_refuse
#define gles_host_rejects gles_angle_host_rejects
#define gles_host_set_debug gles_angle_host_set_debug
#define gles_host_debug gles_angle_host_debug
#define gles_host_ram_gen gles_angle_host_ram_gen
#include "gles-host.c"

bool gles_angle_host_available(void);
bool gles_angle_host_available(void)
{
    return gles_angle_load();
}
