/*
 * The GL bridge's entry points where ANGLE is the only renderer (Windows, where there is no CGL): each forwards to
 * gles-host-angle.c's gles_angle_* build of gles-host.c, as gles-host.c's own do on a Mac boot that picked ANGLE.
 * Without ANGLE's libraries every call is refused, as gles-host-stub.c refuses them, and the guest draws in software.
 *
 * Copyright (c) 2026 the qemu-ios contributors.
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "qom/object.h"
#include "qapi/error.h"
#include "hw/arm/guest-services/gles.h"

int64_t gles_angle_host_call(CPUState *cpu, uint32_t slot, uint32_t ctx, uint32_t argc, const uint32_t *args);
void gles_angle_host_stats(uint64_t *draws, uint64_t *presents);
void gles_angle_host_set_allowed(bool allowed);
void gles_angle_host_reset(void);
int gles_angle_host_context_count(void);
bool gles_angle_host_refuse(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
char *gles_angle_host_rejects(void);
void gles_angle_host_set_debug(bool on);
bool gles_angle_host_debug(void);
uint64_t gles_angle_host_ram_gen(uint64_t addr, uint64_t len);
bool gles_angle_host_available(void);
void gles_host_set_allowed(bool allowed);

static bool angle(void)
{
    static int available = -1;
    if (available < 0) {
        available = gles_angle_host_available();
    }
    return available;
}

int64_t gles_host_call(CPUState *cpu, uint32_t slot, uint32_t ctx, uint32_t argc, const uint32_t *a)
{
    return angle() ? gles_angle_host_call(cpu, slot, ctx, argc, a) : -1;
}

void gles_host_stats(uint64_t *draws, uint64_t *presents)
{
    if (angle()) {
        gles_angle_host_stats(draws, presents);
    } else {
        *draws = *presents = 0;
    }
}

void gles_host_set_allowed(bool allowed)
{
    if (angle()) {
        gles_angle_host_set_allowed(allowed);
    }
}

void gles_host_reset(void)
{
    if (angle()) {
        gles_angle_host_reset();
    }
}

int gles_host_context_count(void) { return angle() ? gles_angle_host_context_count() : 0; }

bool gles_host_refuse(const char *fmt, ...)
{
    if (!angle()) {
        return false;
    }
    va_list ap;
    va_start(ap, fmt);
    g_autofree char *why = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    return gles_angle_host_refuse("%s", why);
}

char *gles_host_rejects(void) { return angle() ? gles_angle_host_rejects() : g_strdup(""); }

void gles_host_set_debug(bool on)
{
    if (angle()) {
        gles_angle_host_set_debug(on);
    }
}

bool gles_host_debug(void) { return angle() && gles_angle_host_debug(); }

uint64_t gles_host_ram_gen(uint64_t addr, uint64_t len) { return angle() ? gles_angle_host_ram_gen(addr, len) : 0; }

/* gl-renderer takes the Mac's names; here either is ANGLE, the only renderer. */
static char *gles_get_renderer(Object *obj, Error **errp) { return g_strdup("angle"); }

static void gles_set_renderer(Object *obj, const char *value, Error **errp)
{
    if (strcmp(value, "angle") && strcmp(value, "cgl")) {
        error_setg(errp, "gl-renderer is cgl or angle, not '%s'", value);
    }
}

void gles_host_class_init(ObjectClass *oc)
{
    object_class_property_add_str(oc, "gl-renderer", gles_get_renderer, gles_set_renderer);
    object_class_property_set_description(oc, "gl-renderer", "The GL bridge's host renderer: ANGLE, the only one here");
}
