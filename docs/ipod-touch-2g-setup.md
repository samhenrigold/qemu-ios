# Running the emulated iPod touch 2G

This fork emulates an iPod touch 2G (S5L8720) running iOS 3.1.3 (build 7E18):
it boots to a usable home screen, runs decrypted apps and OpenGL ES games,
plays sound, reaches the network, and takes a debugger.

## 1. Get the images

Download from the
[release](https://github.com/samhenrigold/qemu-ios/releases/tag/ipod-touch-2g-2026.08)
and verify against `SHA256SUMS`.

| Asset | What it is |
|---|---|
| `bootrom_240_4` | S5L8720 bootrom, unchanged from upstream |
| `nor_7E18.bin` | 3.1.3 NOR, SHSH wrapped for the emulated device's UID |
| `iBoot.bin` | 7E18 iBoot, loaded directly — 3.1.3 does not boot without it |
| `nand-canonical.tar.gz` | Clean 3.1.3 image; the regression harness's baseline |
| `nand-appsync3.tar.gz` | Same, plus the patches that let decrypted apps install **and launch** |

Unpack a NAND tarball anywhere; the directory you point QEMU at is the one
containing `cs0`–`cs3`.

`nand-appsync3` boots with its code-signing gates open, which is what lets
decrypted apps launch, `lldb` attach, and unsigned bundles `dlopen`. It is not
a faithful device — use `nand-canonical` if you care about that.

Neither NAND image is a dump. Both are built from
`iPod2,1_3.1.3_7E18_Restore.ipsw` (a public Apple file), with the volume grown
so the guest has room to write. See §4 to build your own.

## 2. Run it

Unpack the release next to each other in one directory and point the runner at
it:

    IPOD_FILES=~/ipod-touch-files contrib/run-ipod-touch.sh          # keeps state across runs
    IPOD_FILES=~/ipod-touch-files contrib/run-ipod-touch.sh --net    # ...with WiFi
    IPOD_FILES=~/ipod-touch-files contrib/run-ipod-touch.sh --sound  # ...with audio
    contrib/run-ipod-touch.sh --help                                 # the full list

Use the runner rather than calling QEMU directly. 3.1.3 needs half a dozen
environment settings to boot at all — the 7E18 iBoot loaded directly, the
watchdog left unarmed, the TV-out gates answered, the panel backlight forced,
and the two kernel boot args that open the code-signing gate — and the failure
when one is missing is a hang at the Apple logo with nothing in the log. The
runner sets them and documents why each one is there.

Writes go to a copy-on-write overlay, so the base image is never modified and
`--fresh` always gets you back to a clean device.

Two things to know:

- **Do not pass `-cpu max`.** Older instructions recommend it. It makes the
  guest appear to burn a full core forever, because it turns XNU's CP15 wait-
  for-interrupt into a no-op — the emulator then spins where the real device
  would idle. The default `arm1176` is correct and faster in practice.
- **The device idle-sleeps during the boot wait**, and the digitizer does not
  come back on its own. Press Home before touching anything, or touch will look
  completely dead.

Hardware buttons live behind Command, because plain letters go to the guest's
own keyboard:

| Key | Button |
|---|---|
| `Cmd+Shift+H` | Home |
| `Cmd+L` | Power / lock |
| `Cmd+-` / `Cmd+=` | Volume down / up |
| `Cmd+←` / `Cmd+→` | Rotate a quarter turn |

All of them are also in the **Device** menu, along with **Install App…** (the
same handler a dropped `.ipa` takes) and **Open Terminal**, which opens a root
shell on the guest over USB.

`imgtools/itdrive.py` drives the same things over QMP — taps, swipes,
screendumps — and `tests/ipod/run-regression.sh` is a regression harness whose
every check corresponds to a bug that shipped in this tree.

## 3. Build the emulator

    ./configure --enable-sdl --target-list=arm-softmmu \
                --disable-capstone --disable-pie --disable-fuse \
                --extra-cflags=-I/opt/homebrew/opt/openssl@3/include \
                --extra-ldflags="-L/opt/homebrew/opt/openssl@3/lib -lcrypto"
    ninja -C build qemu-system-arm

Do not pass `--disable-slirp` if you want the network to work.

## 4. Build the images yourself

Two pieces cannot be generated from scratch — the **bootrom** and a **NAND page
directory** to use as a template — but you do not need a device to get them:
both are in the release above, and the bootrom doubles as its own download.
Everything else `imgtools` builds for you.

The template is needed because a NAND image is not only a filesystem — it also
carries the FTL's bookkeeping pages, the NAND driver signature and a GPT at
device LBA 0–2. Those describe the storage rather than its contents, and are
copied verbatim. So `build_nand.py` re-skins an existing image with a different
iOS; generating that metadata from first principles is unsolved here.

Root filesystem to NAND pages:

    imgtools/build_nand.py \
        --rootfs <rootfs.dmg> --key <rootfs dmg key> \
        --kernelcache kernelcache.release.s5l8720x \
        --template <an existing page directory> \
        --out nand-mine

It decrypts the rootfs, grows the volume, rewrites `/private/etc/fstab` to a
single read-write root, copies the kernelcache where iBoot asks FMSS for it,
runs `fsck_hfs -n` and refuses to continue if that is unhappy, then lays the
volume across four chip-selects. The kernelcache is copied in *still
encrypted* — the emulated AES engine decrypts it in-guest, so you need no key
for it.

NOR:

    imgtools/build_nor.py --base <a NOR dump> \
        --all-flash <IPSW>/all_flash.n72ap.production --out nor-mine
    imgtools/build_nor.py --verify nor-mine

The `--base` NOR supplies SysCfg and nvram, which are per-device.

One thing here is easy to miss. A 3.x iBoot does not verify a flash-resident
image's SHSH where it finds it — it first *unwraps* it, decrypting the
signature under a key derived from the device UID. The IPSW's `all_flash`
copies carry a **plaintext** SHSH, so pasting them in unmodified means iBoot
decrypts a signature that was never encrypted, gets garbage, and rejects the
image — silently, in the boot logo's case. `build_nor.py` wraps each SHSH for
the target device, which is what makes iBoot draw the Apple logo and load the
device tree with no patching at all. 2.x iBoot has no such step: pass
`--no-wrap-shsh` for a 2.x image.

## 5. What works, and what doesn't

**Works.** Boot and reboot; touch, multitouch (pinch-to-zoom) and the on-screen
keyboard; WiFi, with Safari rendering real pages; installing and launching
decrypted third-party apps over USB; real OpenGL ES games; sound; the host
clipboard writing into the guest pasteboard; root ssh over USB; `lldb` on guest
processes; save and restore of a running machine via `migrate file:` /
`-incoming file:`.

**3D is partial.** A game's world, camera and menus render and animate, and
frames reach CoreAnimation through the real IOSurface present path. But at
least one app reaches a state where the panel keeps compositing a stale frame:
the missing geometry is provably in the GL colour buffer and provably absent
from the panel, with CA accepting every frame it is handed. Pacing also
judders, while the guest sits at 3.5% CPU.

**Audio is imperfect.** Output is sample-exact against the source and correctly
paced, but a clip can replay a fragment of itself about 0.37 s later — one
period of the guest's 16-page DMA ring. Ringtones go through the AMC AAC engine
rather than raw PCM and have not been looked at.

**Encrypted App Store binaries cannot run** — only decrypted ones.

**The device never auto-locks, dims, or idle-sleeps** on a stock run. Set
`IT_I2C_NAK=1` to get those behaviours back.
