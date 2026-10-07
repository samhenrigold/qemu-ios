# QEMU-iOS

Built with use from agentic coding products.

QEMU-iOS emulates legacy Apple devices well enough to run their stock firmware: from the boot ROM or a
kernel bundle to the home screen, with touch, buttons, display, audio, Wi-Fi, USB (usbmux/AFC through
libimobiledevice), persistent storage, a fake cellular network on the iPhones, and OpenGL ES through a
host GL bridge. It is the emulator inside [Light Touch](https://github.com/samhenrigold/LightTouchMac).

| Machine (`-M`) | Device | SoC | Board docs |
|---|---|---|---|
| `iPod-Touch-1G` | iPod touch (1st gen, n45ap) | S5L8900 | `docs/ipod1g/README.md` |
| `iPhone-2G` | iPhone (m68ap) | S5L8900 | `docs/m68/README.md` |
| `iPod-Touch` | iPod touch (2nd gen, n72ap) | S5L8720 | `docs/capabilities.md`, `docs/ipod/` |
| `n18` | iPod touch (3rd gen, n18ap) | S5L8922 | `docs/n18/README.md` |
| `n88` | iPhone 3GS (n88ap) | S5L8920 | `docs/n88/README.md` |
| `iPod-Touch-4G` | iPod touch (4th gen, n81ap) | A4 (S5L8930) | `docs/n81/README.md` |
| `iPhone-4` | iPhone 4 GSM (n90ap) | A4 (S5L8930) | `docs/n90/README.md` |
| `ipad1` | iPad (k48ap) | A4 (S5L8930) | `docs/ipad1/README.md` |

Which firmware builds each board runs, and how well, is tracked in Light Touch's firmware catalog
(`LightTouchMac/Resources/firmware-catalog.json`): every build is prepared from Apple's stock IPSW.

Shared between the boards: the host GL executor (`hw/arm/gles-host*.c`), the guest-service hypercalls
(`hw/arm/guest-services.c`, `guest-gles.c`, `guest-pasteboard.c`, `guest-package.c`), the typed guest
agent RPC (`hw/arm/ipod-agent.c`) and the baseband model (`hw/misc/ios_baseband*.c`, `docs/baseband/`).

Light Touch is three repositories: this emulator; the macOS app
[LightTouchMac](https://github.com/samhenrigold/LightTouchMac), which links it as `libqemu-arm.dylib`
and ships the guest tools built from `contrib/`; and a [usbmuxd fork](https://github.com/samhenrigold/usbmuxd)
(branch `idle-poll`) that carries the emulated USB device to libimobiledevice (`docs/tcp-usb-protocol.md`
is the wire contract).

## What is where

| Path | What |
|---|---|
| `hw/arm/`, `include/hw/arm/` | The three machines and their peripherals, plus the shared host pieces above |
| `contrib/it-*` | Guest helpers for the iPod (armv6, built with `contrib/armv6-toolchain`): `it-agent` (the guest agent), `it-gles` (MBX GL shim), `it-boot` (guest-package loader), `it-pasteboard`, `it-media`, `it-keybag`, `it-seal`, … each with its own README |
| `contrib/gles-public`, `contrib/ipad1-guest`, `contrib/appsync` | the GL front end (one OpenGLES.framework replacement for every 2.x-5.x firmware, iPad and iPod; `docs/ipad1/gles-public-seam.md`); iPad-side helpers: `ipad1-guest/build.sh` builds `it_pbd` (pasteboard bridge) and `it_ethlink` (raises the USB Ethernet link) for armv7 from the shared sources; the AppSync interposer dylib |
| `contrib/guest-package` | `mkpkg.py` and `VERSION`: the versioned guest-tools package format the loader installs at boot |
| `contrib/macos-app` | `make-dylib-macos.sh` (the app's dylib), `entitlements.plist` (the app's helper entitlements), `nandpack.py` |
| `contrib/run-ipod-touch.sh` | Stand-alone windowed launcher for the iPod (expects images under `~/Developer/qemu-ios-files`) |
| `imgtools/` | NAND/HFS/img3 tools and older one-offs. Devices are made by LightTouchMac's Swift FirmwareKit: `firmwarekit create --catalog CATALOG --id BOARD-BUILD --ipsw IPSW --out OUT` (CATALOG: LightTouchMac's `LightTouchMac/Resources/firmware-catalog.json`; k48ap and 4.x n72ap also take `--helper` with the LightTouchDevice executable). `device.py create` and the `*_device.py` names only translate their inputs and call it (`research/python-preparer/README.md`) |
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
directory.

**The contract with LightTouchMac** is `contrib/export-guest-artifacts.sh OUT [BUILD_DIR]`: it builds every
guest component from a copy of `contrib/*` (through `contrib/guest-package/build.sh`, so the checkout is
never written) and stages what the app consumes — `guest-tools/` (the iPod set), `ipad-guest-tools/` (the
flat set `firmwarekit` reads: helpers, AppSync, the two GL engines with `gles-names.h`, the `.itpack`s
at `contrib/guest-package/VERSION`'s serial), `macos-app/entitlements.plist`, `include/`, and with a build
directory `dylib/libqemu-arm.dylib` — with `manifest.json` (source commit, branch, dirty flag, sha256 of
every input and every staged file). LightTouchMac pins this repository by commit in
`build-support/sources.json`; its `scripts/build-guest-tools.sh` is a thin caller of the export, and its
release build validates the staged tree against the manifest. Bump the pin when the guest tools, the
helper ABI or the entitlements change.

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
| `tests/ipad1/regress.py` | The iPad suite, built on the iPod harness: `boot, usbmux, afc, persist, wifi, net, audio`; opt-in `net-usb`, `shadow`; `--device DIR` boots a `firmwarekit create` output through its own iBoot, NOR and keys | golden-pristine or a device directory, the usbmuxd fork |
| `tests/fresh-device.sh BOARD-BUILD IPSW OUT [create options]`; `tests/ipod/fresh-device.sh IPSW OUT`, `tests/ipad1/fresh-device.sh IPSW OUT` (`ENTRY`, default n72ap-7E18 / k48ap-7B500) | A new device from the catalog entry (`firmwarekit create`), then the board's default checks (`boot,fsck,persist` on n72ap, `boot,persist` on k48ap, `boot` on n45ap). Run these when `contrib/` changes | The IPSW; `FIRMWAREKIT`, `FIRMWAREKIT_CATALOG`; `--helper` among the create options for k48ap and 4.x n72ap |
| `python3 tests/ipod/test_*.py`, `tests/ipad1/test_*.py`, `tests/guest-package/test_*.py` | Host-only unit checks, one file at a time (about 120 under `tests/ipod`); the GLES boundary and guest-package checks compile the real C under ASan/UBSan | No emulator for most; a few `*_guest.py` boot one |
| `tests/ipad1/boot-smoke.py`, `restore-smoke.py`, `iboot-check.py`, `app-compat.py`, `audio-check.py`, `mic-check.py`, `snapshot-check.py` | Single-purpose iPad drivers: how far a boot got, a stock restore ramdisk through SecureROM and emulated DFU/recovery USB, a boot through iBoot, the app-compatibility pass, audio out and in, a live snapshot round trip | Per docstring |

The known-failing list at the top of `tests/gate.sh` names each unit check whose C slice or harness mock has
fallen behind the tree, with the reason; delete a line there once its check passes again.

The export for the app (`contrib/export-guest-artifacts.sh`, "Build" above) is gated on the LightTouchMac side:
its `scripts/build-release.py --stage guest` runs the export from the commit pinned in `build-support/sources.json`
and validates the staged tree against `manifest.json`; a pin bump is a LightTouchMac commit.

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

`docs/ipad1/*.tsv` and `docs/ipod/*.tsv` are GL dispatch tables, build inputs `contrib/export-guest-artifacts.sh`
stages for LightTouchMac (moving them out of `docs/` is on another track). `docs/ipad1/screens/`
holds the iPad evidence screenshots; put new evidence in `qemu-ios-files` instead.

## Upstream and licence

QEMU-iOS is built on [QEMU](https://www.qemu.org) and grew out of devos50's
[qemu-ios](https://github.com/devos50/qemu-ios), the emulator for the iPod touch 1G and 2G. devos50's
write-ups are still the best introduction to the S5L8720 peripherals:
[running the iPod touch 1G](https://devos50.github.io/blog/2022/ipod-touch-qemu-pt2/) and
[the reverse-engineering process](https://devos50.github.io/blog/2022/ipod-touch-qemu/). `RUNNING.md`
is upstream's iPod touch 2G guide, kept with a note on what still applies.

Like QEMU, this repository is licensed under the GNU General Public License version 2 (`COPYING`); some
files carry other compatible licences in their headers (`LICENSE` explains QEMU's licensing). No Apple
firmware is part of the repository.
