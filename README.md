# QEMU-iOS

Built with use from agentic coding products.

This is a fork of [devos50/qemu-ios](https://github.com/devos50/qemu-ios) that emulates two legacy
Apple devices well enough to run their stock iOS to the home screen, install and run App Store apps of
the era (including OpenGL ES games through a host GL bridge), talk to `libimobiledevice` over an
emulated USB link, and persist guest writes across reboots -- and a third, the iPod touch 1G, as far as
its home screen:

| Machine | Device | SoC | Models | Firmwares reached |
|---|---|---|---|---|
| `-M iPod-Touch` | iPod touch 2G (n72ap) | S5L8720 | `hw/arm/ipod_touch_2g.c`, `hw/arm/ipod_touch_*.c` | iOS 3.1.3 (7E18), 4.2.1 (8C148); 2.1.1 (5F138) with the host setting the clock |
| `-M ipad1` | iPad 1 (k48ap) | S5L8930 (A4) | `hw/arm/ipad1.c`, `hw/arm/s5l8930_*.c`, `include/hw/arm/s5l8930.h` | iOS 3.2 (7B367), 3.2.2 (7B500), 4.2.1 (8C148), through the real iBoot chain |
| `-M iPod-Touch-1G` | iPod touch 1G (n45ap) | S5L8900 | `hw/arm/ipod_touch_1g.c`, `hw/arm/s5l8900_*.c`, the `ipod_touch_*.c` models with `s5l8900`/variant properties; `docs/ipod1g/README.md` | iPhone OS 1.1 (3A101a) to the home screen with touch, through the real bootrom and iBoot-204 (devos50's public n45ap assets; milestone 0, no app or USB work yet) |

Shared between the boards: the host GL executor (`hw/arm/gles-host*.c`), the guest-service hypercalls
(`hw/arm/guest-services.c`, `guest-gles.c`, `guest-pasteboard.c`, `guest-package.c`) and the typed guest
agent RPC (`hw/arm/ipod-agent.c`).

The fork is one third of Light Touch. The native macOS app is the sibling
[LightTouchMac](https://github.com/samhenrigold/LightTouchMac) repository; it links this emulator as
`libqemu-arm.dylib`, ships the guest tools built from `contrib/`, and its Swift preparer (`firmwarekit`)
is checked against the Python pipeline here. The third part is the
[usbmuxd fork](https://github.com/samhenrigold/usbmuxd) (branch `qemu-zlp`) that carries the emulated
USB device to libimobiledevice; `docs/tcp-usb-protocol.md` is the wire contract between the two.

**Project status lives in LightTouchMac's `docs/STATUS.md`** (branch `multidevice`): what is done,
what is running, what is left, and how each line was checked. This README is only the map of this
repository.

## What is where

| Path | What |
|---|---|
| `hw/arm/`, `include/hw/arm/` | The three machines and their peripherals, plus the shared host pieces above |
| `contrib/it-*` | Guest helpers for the iPod (armv6, built with `contrib/armv6-toolchain`): `it-agent` (the guest agent), `it-gles` (MBX GL shim), `it-boot` (guest-package loader), `it-pasteboard`, `it-media`, `it-webproxy`, `it-keybag`, `it-seal`, … each with its own README |
| `contrib/ipad1-gles`, `contrib/ipad1-guest`, `contrib/appsync` | iPad-side helpers: the GLI shim for ES 1.1/2.0; `ipad1-guest/build.sh` builds `it_pbd` (pasteboard bridge) and `it_ethlink` (raises the USB Ethernet link) for armv7 from the shared sources; the AppSync interposer dylib |
| `contrib/guest-package` | `mkpkg.py` and `VERSION`: the versioned guest-tools package format the loader installs at boot |
| `contrib/macos-app` | `make-dylib-macos.sh` (the app's dylib), `entitlements.plist` (the app's helper entitlements), `nandpack.py` |
| `contrib/run-ipod-touch.sh` | Stand-alone windowed launcher for the iPod (expects images under `~/Developer/qemu-ios-files`) |
| `imgtools/` | The Python pipeline: `device.py create MANIFEST OUT` builds a device from a stock IPSW, its keys and a seed (`ipad1_device.py` for k48ap, `ipod2g_device.py` for n72ap), plus NAND/HFS/img3 tools and older one-offs. Today this is the test oracle for LightTouchMac's `firmwarekit`; retiring it to research and test drivers is decided (LightTouchMac `docs/sweep/PLAN.md`, S1/C5) |
| `manifests/` | One declared-inputs manifest per build (`ipad1-7B367/7B500/8C148`, `ipod2g-5F138/7E18/8C148`) |
| `tests/ipod/`, `tests/ipad1/`, `tests/guest-package/` | The gates below |
| `scripts/ccninja`, `scripts/configure-patched-ffmpeg` | Build helpers |
| `docs/` | Ours are the `.md` files; the `.rst`/`.txt` tree is upstream QEMU's. See "Documentation" |

Firmware, images and the private work area (`~/Developer/qemu-ios-files`) are never in the repository.

## Build

Configure against the patched FFmpeg, not Homebrew's: the iPod H.264 bridge needs
`contrib/ffmpeg/h264-chunk-er.patch`, and a tree linked against stock FFmpeg fails
`tests/ipod/test_h264_snapshot.py` ("slice decode failed") while looking identical. The helper defaults
`FFMPEG_PREFIX` to the prefix `~/Developer/qemu-ios/build-native14` was built with.

```sh
mkdir build && cd build
../scripts/configure-patched-ffmpeg          # meson configure: arm-softmmu, headless (no SDL)
../scripts/ccninja qemu-system-arm           # ninja through ccache, one cache for every worktree
```

`scripts/ccninja` puts Homebrew's ccache masquerade directory on `PATH` (the configured compiler is bare
`cc`) and sets `CCACHE_BASEDIR=~/Developer`, so a fresh worktree builds in seconds at a warm cache.

The app's library is a relink of the same objects: `contrib/macos-app/make-dylib-macos.sh BUILD_DIR`
writes `BUILD_DIR/libqemu-arm.dylib`, exporting only the `qemu_ios_*` ABI. LightTouchMac's
`Configuration/Shared.xcconfig` points `QEMU_IOS_DIR`/`QEMU_BUILD_DIR` at a checkout and such a build
directory. The multi-device app needs the dylib from this `ipad1` line (the iPod-only `ipod_touch_2g`
branch lacks the iPad exports).

For a windowed stand-alone iPod run (`contrib/run-ipod-touch.sh`) the older configure line still works,
at the cost of the FFmpeg caveat above:

```sh
../configure --enable-sdl --target-list=arm-softmmu --disable-capstone --disable-pie --disable-slirp --disable-fuse \
    --extra-cflags=-I/opt/homebrew/opt/openssl@3/include --extra-ldflags='-L/opt/homebrew/opt/openssl@3/lib -lcrypto'
ninja -C build qemu-system-arm
```

(`--disable-slirp` costs the Wi-Fi checks; see `RUNNING.md`, upstream's, for other platforms.)

## Gates

One command, three tiers:

```sh
tests/gate.sh --quick    # host only, about a minute: every host-side unit check, in parallel
tests/gate.sh --full     # quick + the iPod and iPad regression suites (default tiers), one after the other
tests/gate.sh --fresh    # full + both fresh-device.sh: run when imgtools/, manifests/ or contrib/ change
```

One line per check (PASS, FAIL, SKIP with the reason, XFAIL for a check the script lists as known failing
on today's tree, XPASS once it passes again); non-zero exit only on FAIL; every log under the printed
directory. Unit checks that launch the emulator, or take a NAND or a movie on the command line, are SKIP in
every tier and are run by hand. The suites keep their own input defaults, except that the iPod suite runs with
`--stage-gles-shim` (the gate judges this tree's host and guest shim together, not the shipping image's older
baked shim); `QEMU=` overrides the emulator (default `build/qemu-system-arm`). The table below is what each
tier is made of.

Every headless boot passes `-audio driver=none`. The harnesses pick their own ports, write only their
own overlays, and signal only processes they started.

| Gate | What it covers | Inputs |
|---|---|---|
| `tests/ipod/run-regression.sh` (= `tests/ipod/regress.py`) | The iPod suite. Default tier: `boot, fsck, persist, appinstall, applaunch, gles, agent, audio`; `--with-apps` adds `afc, usbtcp, wifi, respring, restart`; `--quick` is boot + AFC; `--checks a,b`; `--check-prereqs` lists what is missing and runs nothing. Each check guards a bug that shipped; the docstring in `regress.py` says which | `build/qemu-system-arm`, a NAND with `it_agent` (`~/Developer/qemu-ios-files/nand-current`), the usbmuxd fork and libimobiledevice tools for USB checks, `contrib/it-harness/build/Harness.ipa` for app checks |
| `tests/ipad1/regress.py` | The iPad suite, built on the iPod harness: `boot, usbmux, afc, persist, wifi, net, audio`; opt-in `net-usb`, `shadow`; `--device DIR` boots a `device.py` output through its own iBoot, NOR and keys | golden-pristine or a device directory, the usbmuxd fork |
| `tests/ipod/fresh-device.sh [MANIFEST] [OUT]`, `tests/ipad1/fresh-device.sh [MANIFEST] [OUT] [-- create options]` | A new device from declared inputs only (`imgtools/device.py create`), then the regression tier (iPod) or two boots on one overlay with a clean power-off between them (iPad). Run these when `imgtools/`, `manifests/` or `contrib/` change | The manifest's IPSW and keys; an activation hook is Sam's and is passed in, never committed |
| `python3 tests/ipod/test_*.py`, `tests/ipad1/test_*.py`, `tests/guest-package/test_*.py` | Host-only unit checks, one file at a time (about 120 under `tests/ipod`); the GLES boundary and guest-package checks compile the real C under ASan/UBSan | No emulator for most; a few `*_guest.py` boot one |
| `tests/ipad1/boot-smoke.py`, `restore-smoke.py`, `iboot-check.py`, `app-compat.py`, `audio-check.py`, `mic-check.py`, `snapshot-check.py` | Single-purpose iPad drivers: how far a boot got, a stock restore ramdisk through SecureROM and emulated DFU/recovery USB, a boot through iBoot, the app-compatibility pass, audio out and in, a live snapshot round trip | Per docstring |

The known-failing list at the top of `tests/gate.sh` names each unit check whose C slice or harness mock has
fallen behind the tree, with the reason; delete a line there once its check passes again.

## Documentation

| Where | What |
|---|---|
| `docs/capabilities.md` | The iPod board: what works, where it stops, how to verify |
| `docs/ipad1/README.md` | The iPad board: Sam's principles (vanilla guest, IPSW-agnostic guest changes, Wi-Fi default), the definition of done, the audio and Bluetooth notes, and the index of `docs/ipad1/` (`iboot.md`, `wifi.md`, `usb-keyboard.md`, `ios4.md`, `location.md`, `guest-services.md`, `app-compat*.md`, `addresses-7B500.md`) |
| `docs/ipod/` | `from-ipsw.md` (the manifest pipeline for the iPod), the 7E18 hardware notes (`pmu-7e18.md`, `ipod-pke.md`, `ipod-clcd-irqs.md`, `ipod-native-halt.md`, `nor-transactions.md`), `ipod-touch-2g-setup.md` (the stand-alone launcher walkthrough), and `nand-current-new-verification.md` (the staged iPod image, kept until the image swap at the main merge) |
| `docs/configuration.md` | Typed machine properties and their `IT_*` aliases |
| `docs/tcp-usb-protocol.md` | The tcp_usb wire protocol shared with the usbmuxd fork |
| `docs/networking.md`, `docs/stock-service-protocol.md`, `docs/app-ledger.md` | Guest networking and the built-in proxy; the Weather/Stocks gateway; the iPod app ledger |
| `contrib/*/README.md`, `imgtools/README-appsync.md` | Per-tool notes |
| `docs/research/` | Research that preceded the code (the A4 gap/reference reports, the 7B500 userland and GL notes, the Bluetooth keyboard scoping, the iPod media investigation, the run-on-an-iPhone study). Each file starts with what superseded it |
| `docs/archive/` | Superseded plans and dated logs: the iPad milestone plan, the Sept-5 and plan-progress trackers, the packaging assessment, the August app survey, the iPod backport and guest-services plans |

`docs/ipad1/*.tsv` and `docs/ipod/*.tsv` are GL dispatch tables, build inputs consumed by LightTouchMac's
`scripts/build-guest-tools.sh` (moving them out of `docs/` is on another track). `docs/ipad1/screens/`
holds the iPad evidence screenshots; put new evidence in `qemu-ios-files` instead.

## Upstream

QEMU-iOS started as devos50's emulator for the iPod touch 1G and 2G. Its write-ups are still the best
introduction to the S5L8720 peripherals:
[running the iPod touch 1G](https://devos50.github.io/blog/2022/ipod-touch-qemu-pt2/) and
[the reverse-engineering process](https://devos50.github.io/blog/2022/ipod-touch-qemu/). `RUNNING.md`
is upstream's iPod touch 2G guide, kept with a note on what still applies.
