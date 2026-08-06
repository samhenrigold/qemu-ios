# itorient — the guest tells the host which way the front app wants to be held

Open a landscape-only app on the emulated iPod and the host's on-screen device
should swing to landscape; press home and it should swing back. This is the
guest half of that. `itorient.c` is the design document — it has the full
argument, including the two routes that looked like they would work and don't.

The one-paragraph version: on 3.1.3 the host cannot see the front app's
orientation. `-[SpringBoard noteUIOrientationChanged:display:]` posts nothing,
the `com.apple.springboard.*Orientation` Darwin notifications come from the
accelerometer (which the host is faking anyway, so they tell it nothing it did
not already know), and `springboardservicesrelay` implements only `getIconState`
/ `setIconState` / `getIconPNGData` — so libimobiledevice's
`sbservices_get_interface_orientation` has nothing to talk to. What the guest
*does* have is SpringBoardServices' `SBGetUIOrientation(mach_port_t, int *)` MIG
stub onto SpringBoard's own server port, free to any process, exactly like
`sbdlicon` in ../it-instprogress. This polls it at 4 Hz and prints a line
whenever the answer changes.

## Output

One integer per line: `0`, `90`, `180` or `-90` — SpringBoard's degrees, which
are the angle the *content* is rotated by, not the angle the device is turned.
`LandscapeLeft` (home button on the right) is `90`. The host converts.

## Deployment: none

Nothing is baked into a NAND image and nothing is injected. LightTouchMac's
`EmulatorController` streams the ~50 KB binary into `/tmp/itorient` over the ssh
stdin of the session that then `exec`s it, so the same connection that installs
it is the one that runs it, and a guest that never got it simply never rotates
by itself.

## Building

    ./build.sh          # needs ../armv6-toolchain

## Status

Builds clean to a valid armv6 Mach-O; the MIG stub's `(port, int *)` signature
was read off the armv6 stub in the 3.1.3 dyld shared cache rather than guessed.
**Not yet run on a guest** — that needs the emulator up with a landscape-only
app to hand, which is a human at a window, not a headless check.
