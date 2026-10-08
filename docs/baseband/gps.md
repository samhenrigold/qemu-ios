# Location hardware per board, and the 3GS's GPS receiver (issue 40)

Static reading of the DeviceTrees, kernelcaches and locationd (4.2.1 8C148/8C148a, 6.1.6 10B500, 7.1.2 11D257),
confirmed by booting the N88 with `IOS_BB_TRACE=2`. Addresses are 8C148a locationd's (`__TEXT` at 0x1000), notes for
the next reader; nothing keys on them.

## Who has what

| Board | Location hardware | How locationd reaches it | Modeled |
|---|---|---|---|
| N88 iPhone 3GS | GPS receiver inside the X-Gold baseband; the DT has no `gps` node | `CLGpsController82`: AT `+XL...` commands on the modem's `cl1` DLCI (DLCI 7 on 4.2.1 and 6.1.6); the modem computes the fix and reports it as `+XLSR:` | yes: `hw/misc/ios_baseband_gps.c` |
| N90 iPhone 4 | Broadcom BCM4750 on `uart4` (DT `gps,bcm4750`, `function-gps_reset` GPIO 0x306, `function-gps_standby` 0x307; kext AppleEmbeddedGPS / AppleBCM4750) | `CLGpsController90`: Broadcom's GLL host library linked into locationd (`/dev/tty.gps`), plus control-plane assistance over the modem (`+XLGCPL`, `+XLRMT`) | no (below) |
| K48 iPad | The DT carries the same `gps,bcm4750` node for the 3G model; the Wi-Fi iPad this machine is has no chip | as the N90 (`handleK48CountInRequest`) | no |
| M68 iPhone, N45/N72/N18/N81 iPods | none | Wi-Fi (and on the iPhone cell) lookups against Skyhook and Apple's location service | no |

locationd picks the controller from the hardware model (`N82`, `N88` -> 82; `N90`, `K48M` -> 90; 0xbedf8,
"Firing up GPS HW version"). The iPhone 3G (N82) would take the same path as the 3GS.

## The 3GS: `+XLSR`

What CLGpsController82 sends once the modem is up (`onBasebandOpened`, 0xc3306), each answered with a bare OK:
`+xlsrstop`, `+xlginfo=0`, `+xlgcpl=1`, `+xlgtest=80`, `+xlgloglev=0,0`, `+xlgmode=1`, `+xlgtest=79`,
`+xlgtest=70`, `+xlrmt=1`, `+xlgnmea=0`. A client asking for location starts a periodic session
(`startLocation`, 0xc0790):

```
at+xlsrstop
at+xlgtest=66 / =108 / =210
at+xlsr=2,,,,1            <- mode 2, interval GpsFixInterval / 1000 s
at+xlginfo=2
```

and stops it with `at+xlsrstop`. The modem then reports a fix every interval as an unsolicited line on the same
DLCI, which `handleXlsr` (0xc2144) hands to `CLHelper::ParsePositionEstimate` (0x2d724) from parameter 0:

| param | meaning |
|---|---|
| 0 | shape (3GPP TS 23.032): 0 point + uncertainty circle, 1 + ellipse, 2 point + altitude + uncertainty ellipsoid, 3 ellipsoid arc, 4 point; anything else is refused |
| 1, 2 | latitude, NMEA style: `ddmm.mmmm`, `N`/`S` |
| 3, 4 | longitude: `dddmm.mmmm`, `E`/`W` |
| 5... | per shape. Shape 2: altitude (m above the WGS84 ellipsoid; locationd subtracts its own geoid separation, 0xd5930), semi-major and semi-minor uncertainty codes, orientation, altitude uncertainty code, confidence (%) |
| last two | speed in knots (x 1.852 / 3.6), course in degrees; either may be empty |

The uncertainty codes are TS 23.032's: r = 10 (1.1^k - 1) m horizontally (0x2d538), 45 (1.025^k - 1) m
vertically (0x2d4e4); with a confidence of 1-99 % locationd divides r by confidence / 100. The model sends
shape 2 at 68 % confidence:

```
+XLSR: 2,3720.094000,N,12200.540000,W,30,3,3,0,4,68,0.00,
```

The fix is the modem's `gps-fix` property ("lat,lon[,alt[,speed[,course[,accuracy]]]]", "" for none, every modem
takes it); only a modem built with `gps=on` (the N88's board data, `bb_gps`) answers `+XLSR` with it. Without a fix a
session runs and reports nothing, as a receiver indoors. The status JSON the app polls carries `gps` and `gps-fix`.

Checked: on n88ap-8C148a CoreLocation in the guest (contrib/it-location's probe) reports the set position, speed and
course, and Maps' Locate Me puts the blue dot at Apple Park; test-ios-baseband's GPS section reads the reports back as
locationd does.

## The iPhone 4: why not

The BCM4750 is a measurement-engine-only part. Everything above the RF correlators runs on the AP inside locationd:
Broadcom's GLL (GPSBCM4750-58: `glmeaseng` with its ASIC packet sender/router, acquisition, tracking DSP and Viterbi
decoding; `glposeng` with the Kalman position engine, ephemeris, almanac and LTO assistance). The chip speaks a
proprietary binary packet protocol (`glme_cmdpckt` / `glme_rsppacket`, an ECPU patch download at start) and returns
raw measurements. A model would have to synthesize a satellite constellation's correlator-level measurements in that
undocumented format, consistent with the position, the time and the ephemeris GLL holds, for a host library that
validates them (`fixSanityChecking`, a watchdog that resets an unresponsive GLL). That is reverse engineering of a
scale this issue does not cover; nothing smaller gives GLL a fix.

Other routes, and why they are not the hardware:

- **Wi-Fi and cell location**: locationd posts the BSSIDs it sees (and the serving cell) to
  `https://iphone-services.apple.com/clls/wloc` (4.x); that endpoint still answered on 2026-10-08 (HTTP 400 to an
  empty POST). It would only place the phone where the scanned access points really are, so a chosen position would
  need the Wi-Fi model to advertise real BSSIDs from that place, and the guest's TLS to reach Apple. Untested.
- **A location accessory** (iAP's location lingo, `CLAccessoryLocationProvider` takes NMEA from one) needs Apple's
  accessory authentication, which an emulator cannot sign.
- **locationd's own NMEA provider** (`NmeaDeviceName`) is a developer default inside the guest, which is the guest
  behavior the models avoid.
