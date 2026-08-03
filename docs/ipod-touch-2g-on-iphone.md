# Running this emulator on an iPhone

Feasibility study, 2026-08-03. **Verdict: viable.** No code was written for it;
this is the record of what was measured so nobody has to re-derive it.

The appeal is obvious — an iPod touch 2G emulated on a phone, driven by touch,
is the device in something close to its original form. The question was never
whether it would be nice, but whether it would be fast enough and whether iOS
would allow it.

## The performance question, measured rather than argued

QEMU's TCG needs to write executable memory, which iOS forbids for ordinary
apps. So the number that decides everything is: how slow is QEMU *without* JIT?

Rather than quote someone else's benchmark, an interpreter-only QEMU was built
from this tree (`--enable-tcg-interpreter`) and the same boot measured through
it. Host was an M4 Max under load 7-15 from other work, so these are
conservative. All three runs reach a pixel-identical 3.1.3 lock screen.

|                       | JIT run 1 | JIT run 2 | interpreter |
|-----------------------|----------:|----------:|------------:|
| boot, wall clock      |    39.0 s |    24.7 s |     115.9 s |
| boot, CPU-seconds     |     30.89 |     19.37 |      101.58 |
| idle, % of one core   |      4.98 |      4.43 |        24.8 |
| driven, % of one core |      10.9 |      6.49 |        15.0 |

**Measured penalty for losing JIT: 3.3-5.2x.** Boot CPU-seconds is the
defensible comparison; the interpreter's idle figure is contaminated by
post-boot settling work spilling into the measurement window. This is plain TCI
— UTM SE ships TCTI, which is faster — so **~4x is an upper bound**. It agrees
with UTM's own published 3.5-6.7x range.

A previous study found a browser/WASM port ~24x too slow and abandoned it.
**That number does not carry over**: it stacked a browser, WASM, and no native
code generation. Native ARM64 interpretation is 4x.

Scaling to a phone: an A19 Pro is ~0.90 of an M4 Max core single-threaded, and
sustained thermal derate is ~0.75, so call it **0.68 M4-Max-core-equivalents**.

- **With JIT:** boot 29-45 s, interactive 10-16% of one core — 6-10x headroom.
- **Without JIT:** boot ~2m50s, interactive 22% of one core.

Both are usable. And this tree already has working save/restore (`migrate file:`
plus `-incoming file:`, 86 MB in ~1.1 s), so an app could ship a post-boot
snapshot and skip the boot entirely.

## JIT: available, but on a leash

- **StikDebug** works on iOS 17.4 through 26.x. `get-task-allow` plus
  `ptrace(PT_TRACE_ME)` sets `CS_DEBUGGED`, which lifts the dynamic-codesigning
  gate on `PROT_EXEC`. Needs a device-specific pairing file made once on a
  computer, and **must be re-armed on every launch**.
- **TrollStore is dead** — the CoreTrust bug was patched in iOS 17.0.1.
- The EU browser-engine entitlement does not apply; it is for browser engines,
  with a paid account and Apple approval.

**This is the real blocker** — not speed, and not whether JIT exists. It is that
JIT depends on third-party tooling, re-armed every launch, on top of a 7-day
free certificate ($99/year buys a year), and that arrangement tends to break
with each iOS release. It is survivable only because the fallback is 4x rather
than 40x.

## Graphics: iOS is a better host than macOS

Checked against the shipping binary rather than the documentation. In Xcode's
device support for an iPhone 17, `OpenGLES.framework` still exports **457 GL
entry points, including the full fixed-function set** — `glMatrixMode`,
`glOrthof`, `glTexEnvf`, `glEnableClientState`, `glLightfv`, and so on — and the
iOS 27 SDK ships matching ES1 headers with an arm64e `.tbd`.

The guest API is GLES 1.1. So on iOS there is **no Metal translation layer and
no fixed-function emulation needed** — a closer match than the Mac's OpenGL 2.1
compatibility profile, which is what `hw/arm/gles-host.c` targets today.

That file is the only macOS-specific one in `hw/arm/`. Its CGL portion is ~40
lines (`CGLChoosePixelFormat`/`CGLCreateContext`/`CGLSetCurrentContext` becoming
`EAGLContext`), plus `*EXT` to `*OES` FBO suffixes.

## Sensors

| | State | Effort |
|---|---|---|
| **Touch** | `ipod_touch_lcd_mouse_event()` already takes normalised coordinates and calls `ipod_touch_multitouch_on_touch/_release/_motion` | Near-free; a UIKit handler calls those three |
| **Multitouch** | **Does not exist.** One `FingerData`, scalar `touch_x`/`touch_y`, `// TODO we assume one finger for now`. Pinch and rotate do not work. | Contained but real — the wire header already has `numFingers`/`fingerDataLen` |
| **Accelerometer** | Modelled LIS302DL. `ACCEL_1G = 0x40`, ±2 g in a signed byte. **X is inverted versus UIKit; Y and Z are not.** | Near-free: `x = -a.x*0x40, y = a.y*0x40, z = a.z*0x40` |
| **Audio out** | Host path proven, but the guest never starts the transfer | Blocked on an open bug |
| **Microphone** | **Unbuilt** — no `AUD_open_in`, `SWVoiceIn` or `AUD_read` anywhere in `hw/arm/`, and neither codec model has mic registers | A whole new capture path |

One accelerometer gotcha: `lis302dl_apply_orientation()` also calls
`it_display_set_orientation()` to rotate the host window. On a phone that should
be suppressed — the physical device is already rotating.

## The rebase

This fork is QEMU 8.2.0; UTM's iOS host QEMU is 10.0.2, so a port means rebasing
onto `utmapp/qemu`. That is less alarming than it sounds.
`git diff --shortstat v8.2.0 HEAD` is 170 files, 29,657 insertions and **214
deletions** — almost purely additive. Outside our own files, the only upstream
code touched is `hw/dma/pl080.c`, `hw/char/exynos4210_uart.c`, one line of
`ui/sdl2.c` and one of `include/qemu/osdep.h`.

**Nothing in `tcg/`, `accel/` or `target/arm/`** — so both the rebase and
swapping the TCG backend are low-risk.

## Smallest experiment that would settle it

The expensive half is already done: the interpreter multiplier for this exact
workload. What remains is device-side — rebase the machine onto utmapp/qemu
10.0.2, build a minimal iOS app with the NAND/NOR/bootrom in the bundle, blit
the `DisplaySurface` into a `UIView`, and time a boot with and without JIT.

Roughly 1-2 weeks for someone who has shipped an iOS build before. The risk sits
in the rebase, not the app.
