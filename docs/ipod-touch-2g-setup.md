# Running the emulated iPod touch 2G

This fork emulates an iPod touch 2G (S5L8720) well enough to boot iOS 3.1.3
(build 7E18) to a usable home screen, install and launch decrypted apps over
USB, reach the network, and take a debugger.

## Getting the images

The images are attached to the
[release](https://github.com/samhenrigold/qemu-ios/releases/tag/ipod-touch-2g-2026.08),
as upstream does for 2.1.1. Verify against `SHA256SUMS`:

| Asset | What it is |
|---|---|
| `bootrom_240_4` | S5L8720 bootrom, unchanged from upstream |
| `nor_7E18.bin` | 3.1.3 NOR, SHSH wrapped for the emulated device's UID |
| `nand-canonical.tar.gz` | Clean 3.1.3 image. The regression harness's baseline. |
| `nand-appsync3.tar.gz` | Same, plus the patches that let decrypted apps install **and launch** |

Both NAND images are **derived**, not dumps: built from
`iPod2,1_3.1.3_7E18_Restore.ipsw` by `build_nand.py`, with the volume grown so
the guest has space to write and `/private/etc/fstab` rewritten to a single
read-write root. `build_nand.py` reproduces them from an IPSW you supply.

`nand-appsync3` is **deliberately weakened**: four patch sites (installd x2,
`MISValidateSignature` inside `dyld_shared_cache_armv6`, and SpringBoard's
`applicationSignatureState`), and it boots with the code-signing gates open.
That is what lets decrypted apps launch, `lldb` attach, and unsigned bundles
`dlopen`. Only the first three patches are proven necessary on 3.1.3; the
SpringBoard one was verified on 2.1.1. Do not treat it as a faithful device.

## Building the images yourself

Everything below is what `build_nand.py` and `build_nor.py` do, and what you
need if you want to rebuild rather than download.

| Piece | Where it comes from | Can it be synthesised? |
|---|---|---|
| **Bootrom** | Dumped from real hardware | **No.** There is no way around this. |
| **NOR image** | Dumped from a real device | Partly — `build_nor.py` repacks one, but needs an existing NOR as `--base` for its SysCfg and nvram. |
| **iOS 3.1.3 root filesystem** | `iPod2,1_3.1.3_7E18_Restore.ipsw` | Yes, via `build_nand.py`. |
| **Kernelcache** | Same IPSW | Yes — copied in still encrypted; the emulated AES engine decrypts it in-guest, so you need no key for it. |
| **NAND template** | An existing page directory | **No** — see the caveat below. |

The IPSW is the easy part; it is a public Apple file and its SHA-1 is widely
published.

## The NAND template caveat

`build_nand.py` builds the filesystem pages from an IPSW rootfs, but a NAND
image is not only a filesystem. It also carries the FTL's own bookkeeping pages,
the NAND driver signature and a GPT at device LBA 0-2 — flash metadata that
describes the storage, not its contents. Those pages are copied verbatim from
`--template`, an existing page directory.

So `build_nand.py` re-skins an existing image with a different iOS; it does not
conjure one from nothing. Generating that metadata from first principles is a
real, unsolved piece of work in this tree.

## Building the images

Root filesystem to NAND pages:

    imgtools/build_nand.py \
        --rootfs   <rootfs.dmg>  --key <rootfs dmg key> \
        --kernelcache kernelcache.release.s5l8720x \
        --template <an existing page directory> \
        --out      nand-mine

It decrypts the rootfs DMG, grows the volume so the guest has space to write,
rewrites `/private/etc/fstab` to a single read-write root (the synthetic image
is one volume, with no separate `/private/var` partition), copies the
kernelcache to the path iBoot asks FMSS for, runs `fsck_hfs -n` and refuses to
continue if it is unhappy, then lays the volume out across four chip-selects
using the closed-form formula in `ftlmap.py`.

NOR:

    imgtools/build_nor.py --base <a NOR dump> \
        --all-flash <IPSW>/all_flash.n72ap.production \
        --out nor-mine

One thing matters here and is easy to miss. A 3.x iBoot does not verify the SHSH
of a flash-resident image as it finds it: it first *unwraps* it, decrypting the
signature in place with AES-CBC under a key derived from the device UID. A real
restore writes images with their SHSH already wrapped for that specific device.
The IPSW's `all_flash` copies carry a *plaintext* SHSH, so pasting them in
unmodified means iBoot decrypts a signature that was never encrypted, gets
garbage, and rejects the image — silently, in the case of the boot logo. So
`build_nor.py` wraps each SHSH under the UID key of the device the NOR is for.
That is what makes iBoot draw the Apple logo and load the device tree by itself,
with no patching of iBoot at all.

2.x iBoot has no such step — every SHSH in a stock 2.1.1 NOR dump is plaintext
and verifies raw — so pass `--no-wrap-shsh` when building a 2.x image.

Verify a NOR by walking it the way LLB would:

    imgtools/build_nor.py --verify nor-mine

## Building the emulator

    ./configure --enable-sdl --target-list=arm-softmmu \
                --disable-capstone --disable-pie --disable-fuse \
                --extra-cflags=-I/opt/homebrew/opt/openssl@3/include \
                --extra-ldflags="-L/opt/homebrew/opt/openssl@3/lib -lcrypto"
    ninja -C build qemu-system-arm

Do not pass `--disable-slirp` if you want the network to work.

## Driving it

Hardware buttons are behind Command, because plain letters go to the guest
keyboard: **Home** is `Cmd+Shift+H`, **power** `Cmd+L`, **volume**
`Cmd+-` / `Cmd+=`, **rotate** `Cmd+Left` / `Cmd+Right`.

`imgtools/itdrive.py` drives the UI over QMP (taps, swipes, screendumps) and
`tests/ipod/run-regression.sh` is a regression harness whose every check
corresponds to a bug that shipped in this tree.

Note the device idle-sleeps during the boot wait and the digitizer does not
return on its own: **press Home to wake before touching anything**, or touch
appears completely dead.

## What works

Boot to a usable home screen; reboot; touch, multitouch (pinch-to-zoom) and the
on-screen keyboard; WiFi (Safari renders pages); installing and launching
decrypted third-party apps over USB; real OpenGL ES games; sound; the host
clipboard writing into the guest pasteboard; root ssh over USB; `lldb` attaching
to guest processes, reading registers and hitting breakpoints; save and restore
of a running machine via `migrate file:` / `-incoming file:`.

## What does not

**3D is partial.** A game's world, camera and menus render and animate, and
frames reach CoreAnimation through the real IOSurface present path. But at least
one app reaches a state where the panel keeps compositing a stale frame: the
missing geometry is provably in the GL colour buffer and provably absent from
the panel, with CA accepting every frame it is handed. Pacing on 3.1.3 also
judders while the guest sits at 3.5% CPU.

**Audio is imperfect.** Output is sample-exact against the source and correctly
paced, but a clip can replay a fragment of itself about 0.37 s later — one
period of the guest's 16-page DMA ring. Ringtones go through the AMC AAC engine
rather than raw PCM and have not been looked at.

Encrypted App Store binaries cannot run — only decrypted ones.

## A note on demo mode

This emulator has always presented itself to iOS as a tethered demo unit,
because 3.1.3's presence test for Apple's demo card is nothing more than "did
the I2C write to address 0x29 succeed", and this tree's I2C never NAKs. That is
why the device never auto-locks, never dims and hides its charging UI.
`IT_I2C_NAK=1` makes absent I2C slaves behave like absent slaves, which is more
faithful — and turns those behaviours back on.
