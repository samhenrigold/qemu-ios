/*
 * Feasibility-spike probe (wasm spike): with SPIKE_PROBE=<seconds> set, prints
 * once a wall second to stderr the elapsed time, the instruction count (with
 * -icount) and the screen's mean luma, and quits after <seconds> (0: never),
 * writing the screen to SPIKE_SHOT (a PPM) first if that is set (every second with
 * SPIKE_SHOT_EVERY=1, for the browser page's canvas); SPIKE_CONT=1 runs a
 * machine that loaded paused. Inert unset.
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "exec/icount.h"
#include "system/runstate.h"
#include "hw/arm/spike-probe.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static QEMUTimer *probe_timer;
static int64_t probe_start;
static double probe_quit_at;

static double probe_screen(const char *ppm)
{
    QemuConsole *con = qemu_console_lookup_by_index(0);
    DisplaySurface *s;
    double sum = 0;

    if (!con) {
        return -1;
    }
    graphic_hw_update(con);
    s = qemu_console_surface(con);
    if (!s || surface_bits_per_pixel(s) != 32) {
        return -1;
    }
    int w = surface_width(s), h = surface_height(s), stride = surface_stride(s);
    uint8_t *data = surface_data(s);
    FILE *f = ppm ? fopen(ppm, "wb") : NULL;
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
    }
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(data + y * stride);
        for (int x = 0; x < w; x++) {
            uint8_t rgb[3] = { row[x] >> 16, row[x] >> 8, row[x] };
            sum += rgb[0] + rgb[1] + rgb[2];
            if (f) {
                fwrite(rgb, 1, 3, f);
            }
        }
    }
    if (f) {
        fclose(f);
    }
    return sum / (3.0 * 255 * w * h);
}

static void probe_tick(void *opaque)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    double t = (now - probe_start) / 1000.0;
    bool quit = probe_quit_at > 0 && t >= probe_quit_at;
    double luma = probe_screen(quit || getenv("SPIKE_SHOT_EVERY") ? getenv("SPIKE_SHOT") : NULL);

    fprintf(stderr, "spike t=%.2f icount=%" PRId64 " vt=%.3f luma=%.3f\n", t,
            icount_enabled() ? icount_get_raw() : (int64_t)-1,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, luma);
    /* A state saved paused (as the helper's Stop saves) loads paused: SPIKE_CONT runs it. */
    if (getenv("SPIKE_CONT") && runstate_check(RUN_STATE_PAUSED)) {
        vm_start();
    }
    if (quit) {
#ifdef __EMSCRIPTEN__
        emscripten_force_exit(0); /* exit() from a pthread leaves Node waiting */
#endif
        exit(0); /* -no-shutdown would turn a shutdown request into a pause */
    }
    timer_mod(probe_timer, now + 1000);
}

void spike_probe_init(void)
{
    const char *env = getenv("SPIKE_PROBE");

    if (!env) {
        return;
    }
    probe_quit_at = atof(env);
    probe_start = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    probe_timer = timer_new_ms(QEMU_CLOCK_REALTIME, probe_tick, NULL);
    timer_mod(probe_timer, probe_start + 1000);
}
