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

## Legacy fallback

Current Light Touch uses the baked guest agent's bounded `orientation` RPC.
Only an image without the agent uses this streamed SSH helper. No injection is
needed. The source remains a reference for the 3.1.3 orientation semantics.

## Building

    ./build.sh          # needs ../armv6-toolchain

## Verification

The native agent replacement is covered by `tests/ipod/test_agent_guest.py
--orientation`: a disposable landscape Harness reports landscape and Home
returns to portrait. The older streaming helper is retained for legacy images.
