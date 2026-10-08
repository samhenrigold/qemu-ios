/*
 * The iPhone 3GS's GPS receiver, as its baseband presents it.
 *
 * The 3GS has no GPS chip of its own on the AP side: the receiver hangs off the X-Gold baseband, which
 * computes the fix itself and reports it over AT. locationd's CLGpsController82 (the N82/N88 path; the
 * iPhone 4 and the 3G iPad drive a BCM4750 on a UART instead) opens the modem's "cl1" DLCI, starts a
 * periodic session with +XLSR=2,,,,<interval s> and stops it with +XLSRSTOP; each fix comes back as a
 * "+XLSR:" line that CLHelper::ParsePositionEstimate reads as a 3GPP TS 23.032 shape
 * (docs/baseband/gps.md). Everything else it sends (+XLGCPL, +XLGMODE, +XLGTEST, +XLRMT, +XLGINFO...)
 * only needs an OK.
 *
 * No QEMU objects in here: ios_baseband_core.c calls it, test-ios-baseband drives it directly.
 */
#ifndef HW_MISC_IOS_BASEBAND_GPS_H
#define HW_MISC_IOS_BASEBAND_GPS_H

typedef struct IosBbGps {
    /* Controls (the board and the app): kept across a baseband reset. */
    bool present;              /* the board has the receiver (N88) */
    bool fix;                  /* a position is set; without one a session reports nothing */
    double lat, lon;           /* degrees, WGS84 */
    double alt;                /* meters above the WGS84 ellipsoid */
    double speed;              /* m/s */
    double course;             /* degrees from true north, < 0 unknown */
    double accuracy;           /* meters, horizontal */

    /* The running +XLSR session. */
    int ch;                    /* DLCI that started it, -1 = none */
    int interval_ms;
    int64_t due_ms;            /* next report */
} IosBbGps;

/* "lat,lon[,alt[,speed[,course[,accuracy]]]]" sets a fix; "" clears it. False (unchanged) on a bad spec. */
bool ios_bb_gps_set(IosBbGps *g, const char *spec);
/* The fix as ios_bb_gps_set takes it ("" without one). */
void ios_bb_gps_get(const IosBbGps *g, char *out, size_t size);
/* A baseband power cycle: the session ends, the controls stay. */
void ios_bb_gps_reset(IosBbGps *g);
/* cmd: an AT command after "+", lower case. True if it was the receiver's (the caller answers OK). */
bool ios_bb_gps_command(IosBbGps *g, int64_t now_ms, int ch, const char *cmd);
/* A report due at now_ms: writes the "\r\n+XLSR: ...\r\n" line and returns its DLCI, else -1. */
int ios_bb_gps_poll(IosBbGps *g, int64_t now_ms, char *line, size_t size);
/* When the next report is due, 0 if none. */
int64_t ios_bb_gps_next_due(const IosBbGps *g);
/* The fix as one "+XLSR:" line's parameters (shape 2, point with altitude and uncertainty ellipsoid). */
int ios_bb_gps_format(const IosBbGps *g, char *out, size_t size);

#endif
