/*
 * The iPhone 3GS's GPS receiver behind its baseband (include/hw/misc/ios_baseband_gps.h,
 * docs/baseband/gps.md).
 *
 * locationd 4.2.1 (8C148a, CLGpsController82 and CLHelper::ParsePositionEstimate at 0x2d724) reads
 * "+XLSR: <shape>,..." from parameter 0: the shape as an integer (0 point + uncertainty circle,
 * 1 + ellipse, 2 point + altitude + uncertainty ellipsoid, 3 ellipsoid arc, 4 point), then latitude
 * and longitude as NMEA-style "ddmm.mmmm",N|S and "dddmm.mmmm",E|W. Shape 2 goes on with the altitude
 * (signed meters above the ellipsoid: locationd subtracts its own geoid separation), the semi-major and
 * semi-minor uncertainty codes, the orientation, the altitude uncertainty code and the confidence (%),
 * and every shape ends with the speed in knots and the course in degrees (empty = unknown). The codes
 * are TS 23.032's: r = 10 * (1.1^k - 1) m horizontally, 45 * (1.025^k - 1) m vertically, and locationd
 * divides the horizontal radius by confidence / 100.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "hw/misc/ios_baseband_gps.h"

#define GPS_CONFIDENCE 68                  /* percent: one sigma, what a receiver reports */
#define GPS_FIRST_FIX_MS 1000              /* hot start: the first report after a session starts */

/* TS 23.032 uncertainty code for radius r (meters), k = 0..127. */
static int unc_code(double r, double c, double x)
{
    double k = r > 0 ? log(r / c + 1) / log(1 + x) : 0;

    return k < 0 ? 0 : k > 127 ? 127 : (int)lround(k);
}

/* NMEA ddmm.mmmmmm (deg_digits = 2) or dddmm.mmmmmm (3) of |deg|. */
static void nmea_coord(double deg, int deg_digits, char *out, size_t size)
{
    double a = fabs(deg);
    int d = (int)a;
    double m = (a - d) * 60;

    if (m >= 59.9999995) {                 /* would print as 60.000000 */
        d++;
        m = 0;
    }
    snprintf(out, size, "%0*d%09.6f", deg_digits, d, m);
}

int ios_bb_gps_format(const IosBbGps *g, char *out, size_t size)
{
    char lat[24], lon[24], course[16] = "";
    int major = unc_code(g->accuracy * GPS_CONFIDENCE / 100, 10, 0.1);

    nmea_coord(g->lat, 2, lat, sizeof(lat));
    nmea_coord(g->lon, 3, lon, sizeof(lon));
    if (g->course >= 0) {
        snprintf(course, sizeof(course), "%.1f", g->course);
    }
    return snprintf(out, size, "2,%s,%c,%s,%c,%ld,%d,%d,0,%d,%d,%.2f,%s",
                    lat, g->lat < 0 ? 'S' : 'N', lon, g->lon < 0 ? 'W' : 'E', lround(g->alt),
                    major, major, unc_code(g->accuracy * 1.5 * GPS_CONFIDENCE / 100, 45, 0.025),
                    GPS_CONFIDENCE, g->speed * 3.6 / 1.852, course);
}

bool ios_bb_gps_set(IosBbGps *g, const char *spec)
{
    double v[6] = { 0, 0, 0, 0, -1, 5 };
    int n = 0;
    const char *p = spec;

    if (!*spec) {
        g->fix = false;
        return true;
    }
    for (;;) {
        char *end;

        if (n == 6) {
            return false;                  /* more than six fields */
        }
        v[n++] = strtod(p, &end);
        if (end == p || !isfinite(v[n - 1]) || (*end && *end != ',')) {
            return false;
        }
        if (!*end) {
            break;
        }
        p = end + 1;
    }
    if (n < 2 || fabs(v[0]) > 90 || fabs(v[1]) > 180 || fabs(v[2]) > 100000 ||
        v[3] < 0 || v[3] > 1000 || v[4] >= 360 || v[5] <= 0 || v[5] > 100000) {
        return false;
    }
    g->lat = v[0];
    g->lon = v[1];
    g->alt = v[2];
    g->speed = v[3];
    g->course = v[4] < 0 ? -1 : v[4];
    g->accuracy = v[5];
    g->fix = true;
    return true;
}

void ios_bb_gps_get(const IosBbGps *g, char *out, size_t size)
{
    if (!g->fix) {
        out[0] = 0;
        return;
    }
    snprintf(out, size, "%.7f,%.7f,%g,%g,%g,%g", g->lat, g->lon, g->alt, g->speed, g->course,
             g->accuracy);
}

void ios_bb_gps_reset(IosBbGps *g)
{
    g->ch = -1;
    g->interval_ms = 0;
    g->due_ms = 0;
}

bool ios_bb_gps_command(IosBbGps *g, int64_t now_ms, int ch, const char *cmd)
{
    if (!g->present) {
        return false;
    }
    if (strcmp(cmd, "xlsrstop") == 0) {
        ios_bb_gps_reset(g);
        return true;
    }
    if (strncmp(cmd, "xlsr=", 5) == 0) {
        /* +XLSR=<mode>,,,,<interval s>: CLGpsController82 asks for GpsFixInterval / 1000. */
        const char *f = cmd + 5;
        int interval = 0;

        for (int i = 0; i < 4 && f; i++) {
            f = strchr(f, ',');
            f = f ? f + 1 : NULL;
        }
        if (f) {
            interval = atoi(f);
        }
        g->ch = ch;
        g->interval_ms = (interval > 0 ? interval : 1) * 1000;
        g->due_ms = now_ms + GPS_FIRST_FIX_MS;
        return true;
    }
    return false;
}

int ios_bb_gps_poll(IosBbGps *g, int64_t now_ms, char *line, size_t size)
{
    char fix[160];

    if (g->ch < 0 || !g->due_ms || now_ms < g->due_ms) {
        return -1;
    }
    g->due_ms = now_ms + g->interval_ms;
    if (!g->fix) {
        return -1;                         /* searching: no position to report */
    }
    ios_bb_gps_format(g, fix, sizeof(fix));
    snprintf(line, size, "\r\n+XLSR: %s\r\n", fix);
    return g->ch;
}

int64_t ios_bb_gps_next_due(const IosBbGps *g)
{
    return g->ch >= 0 ? g->due_ms : 0;
}
