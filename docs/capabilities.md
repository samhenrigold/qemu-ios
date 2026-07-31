# What the emulator can do

An iPod touch 2G (n72ap) running iPhone OS 2.1.1, build 5F138. This is the
practical list: what works, the exact command, and where each thing stops.

Everything below assumes:

```sh
Q=./build/qemu-system-arm            # or build/arm-softmmu/qemu-system-arm
F=/Users/shg/Developer/qemu-ios-files
```

`$F/nand` is the golden image and is **read-only** - never point a writable
option at it. The variant images listed under [NAND images](#nand-images) are
copies with things already injected.

## Boot to the home screen

```sh
$Q -M iPod-Touch,bootrom=$F/bootrom_240_4,nand=$F/nand,nor=$F/nor_n72ap.bin \
   -serial mon:stdio -cpu max -m 2G -display sdl
```

The home screen is up around 100 seconds in. Keys in the SDL window: `H` home,
`P` power, `=` / `-` volume. Click and drag for touch.

The device survives idle sleep now - it used to panic in
`AppleMPVDDriver::setPowerStateGated` about ninety seconds in, which put a hard
ceiling on every session. The screen still does not come back from sleep, so
press nothing and screenshot before it sleeps, or expect black.

## Machine options

| Option | Default | What it does |
| --- | --- | --- |
| `bootrom=` | - | S5L8720 bootrom binary. Required. |
| `nor=` | - | NOR image. Required. |
| `nand=` | - | NAND page directory (`cs0`..`cs3`). Required. |
| `nandrw=` | off | Copy-on-write overlay directory. Guest writes land here and persist across reboots. |
| `boot-args=` | - | Replaces `boot-args` in the NOR's nvram, in memory only. `io=0x37` turns on IOKit matching logs. |
| `usb-attached=` | on | Report a USB cable to the PMU. iOS parks the whole USB stack without it. |
| `usb-tcp-addr=` | - | `host:port` of a USB-over-TCP bridge. Empty disables the link. |
| `usb-patch-mux-gate=` | off | Patch the kernel so the USB stack goes on bus. Build-specific (5F138). |
| `wifi=` | off | Present a BCM4325 on the SDIO bus. |
| `mbx-irq=` | off | Raise a completion interrupt for the unemulated MBX GPU so an app that submits work does not wait forever. Cannot make anything render. |

## Talk to it as a real iOS device

The emulated USB device controller speaks a TCP transport
(`docs/tcp-usb-protocol.md`) to a fork of usbmuxd, after which the standard
`libimobiledevice` tools work.

```sh
# host bridge
cd /Users/shg/Developer/usbmuxd-qemu
USBMUXD_QEMU_ADDR=127.0.0.1:1330 USBMUXD_QEMU_DELAY=12 \
  ./usbmuxd/src/usbmuxd -f -v -S 127.0.0.1:27130 -P NONE -C ./run/mine

# emulator
$Q -M iPod-Touch,bootrom=$F/bootrom_240_4,nand=$F/nand,nor=$F/nor_n72ap.bin,\
usb-attached=on,usb-patch-mux-gate=on,usb-tcp-addr=127.0.0.1:1330 \
   -serial mon:stdio -cpu max -m 2G -display sdl

# host tools
export USBMUXD_SOCKET_ADDRESS=127.0.0.1:27130
idevice_id -l          # UDID appears about 18s after the guest programs the core
ideviceinfo            # DeviceClass iPod, ProductType iPod2,1, 2.1.1 / 5F138
idevicepair pair
```

Poll `idevice_id -l` rather than sleeping a fixed time.

## Run real App Store apps

Third-party apps from 2008 run, fully interactive. They are injected into
`/Applications` in the NAND image offline - not through `ideviceinstaller`.
installd rebuilds its cache every boot by scanning, so a bundle only has to be
present.

```sh
python3 imgtools/editimg.py --nand $F/nand-mine --script inject.sh
```

Two things decide whether a given app works:

- **It must be decrypted.** An encrypted binary fails its code-directory hash at
  the first `__TEXT` page. Clutch-decrypted archives (`cryptid 0`) clear it
  completely. Code signing itself is not a wall - the boot args carry
  `amfi_allow_any_signature=1`.
- **`DTSDKName` must start `iphoneos2.`, not `MinimumOSVersion`.** Plenty of
  apps claim a 2.0 deployment target but were built with a 3.x or 4.x SDK, and
  those silently fail to launch.

`/var/mobile/Applications` does **not** work - bundles there appear and refuse
to open, proven by an A/B with the identical bundle in one boot. Use
`/Applications`.

Verified running: Obama '08, Guangzhou Metro. OpenGL ES apps still hang.

## Persist guest writes across reboots

```sh
$Q -M iPod-Touch,...,nandrw=/path/to/overlay ...
```

Off by default, in which case writes are discarded as before. With an overlay,
state written on one boot - SpringBoard prefs, the installation cache,
TrustStore, `/var/run` - is still there several boots later.

**Flush before you stop the emulator, or new files will not survive:**

```sh
contrib/it-nand-flush.sh /path/to/overlay
```

The guest writes file *data* to flash promptly, but HFS+ keeps catalog updates
in memory and nothing forces them out on an idle device - measured, not one page
reaches flash in the three minutes after an `afcclient put`. Kill QEMU at that
point and every data block is on disk while the directory entry is not, so the
file simply does not exist on the next boot. That is what made guest writes look
like they were being discarded.

The script asks lockdownd to enter recovery, which tears the filesystem down on
the way out; the flush is the point, and the "Failed to enter recovery mode" it
prints is expected (no working reset path). Call it last - the device is
unusable afterwards. Verified end to end: `afcclient put` a 64 KB file, flush,
restart on the same overlay, and `afcclient get` returns it with a matching
SHA-256.

One wrinkle: a boot following an unclean shutdown runs `fsck_hfs`, repairs,
prints `MACH Reboot` and stops, because the machine has no working reset path.
The next boot comes up normally, so boots alternate repair/normal.

## Edit the guest filesystem from the host

The NAND block layout is a closed-form formula, so the whole volume can be
reassembled, mounted on macOS, edited and written back:

```sh
python3 imgtools/editimg.py --nand $F/nand-mine --script my-edits.sh
```

The script runs with `$MNT` pointing at a mounted, writable copy of the guest
filesystem. Nothing is written back unless `fsck_hfs` passes afterwards, and the
tool refuses the golden image.

This is how the Core Animation fix, the apps, `/Developer` and dyld insertions
all got in.

## The Core Animation flip fix

Stocks and Weather froze the whole UI on their "i" flip. Setting
`CA_AUTO_ENABLE_OGL=0` (and `LK_AUTO_ENABLE_OGL=0`) in SpringBoard's launchd
plist inside the image fixes it - the flips render and complete. Already baked
into the `nand-*` images below.

It only redirects Core Animation. Apps that call OpenGL ES directly are
unaffected.

## On-device debugserver

`/Developer` on the rootfs is an empty mount point, and lockdownd reads
`/Developer/Library/Lockdown/ServiceAgents/*.plist` on its own - so copying the
2.1 DeveloperDiskImage's `usr` and `Library` trees straight in replaces the
whole `MobileImageMounter` handshake.

```sh
$Q -M iPod-Touch,...,nand=$F/nand-dev,usb-attached=on,usb-patch-mux-gate=on,\
usb-tcp-addr=127.0.0.1:1330 ...
idevicedebugserverproxy 1234
```

debugserver answers GDB-remote over that port: `qC` returns `$QC0#c4`.
**Attaching does not work yet** - the likely blocker is that `task_for_pid`
needs a `get-task-allow` entitlement that no stock or App-Store-signed binary
carries. Also: debugserver ends its session after a failed attach, and many
short-lived connections wedge the USB link until you reboot.

## Drive it headlessly

```sh
$Q -M iPod-Touch,... -qmp tcp:127.0.0.1:4530,server=on,wait=off
python3 contrib/ipod-touch-qmp.py 4530 shot out.ppm
python3 contrib/ipod-touch-qmp.py 4530 tap 275 243
python3 contrib/ipod-touch-qmp.py 4530 swipe 160 400 160 100
python3 contrib/ipod-touch-qmp.py 4530 key h
```

Screendumps usually come out with a maximum sample value of 1, so a straight
PPM-to-PNG conversion looks solid black. Normalise to 0..255 before viewing.
`imgtools/itdrive.py` does the same job with more around it.

## Dump the USB descriptors

```sh
python3 contrib/ipod-touch-usbdesc.py 1330 105
```

Walks every configuration the guest exposes. This is what ruled out USB
ethernet - see `docs/networking.md`.

## WiFi

```sh
$Q -M iPod-Touch,...,wifi=on ...
```

Off by default. With it on, the BCM4325 driver initialises completely against
the emulated dongle - firmware download, mailbox handshake, SDPCM framing and
the CDC control channel - and publishes an `IO80211Interface` with the right MAC
address. iOS agrees: Settings' Wi-Fi row reads "Not Connected" instead of a
greyed-out "No Wi-Fi", and the Wi-Fi Networks pane opens and scans.

**No traffic passes.** The network list stays empty: the driver asks for a scan
every fifteen seconds and waits for a completion event that the model does not
send yet. See `docs/networking.md` for exactly what remains.

Do not combine `wifi=on` with `boot-args=io=0x37`: IOKit matching logs make the
firmware download about 250x slower, and stretched that far the driver's own
deep sleep timer fires mid-`start()` and panics the guest.

Set `IPOD_SDIO_TRACE=1` in the environment for a full register trace.

## NAND images

All under `$F`. The golden `nand` is never written.

| Image | Contents |
| --- | --- |
| `nand` | Golden. Read-only. |
| `nand-imgtools-1` | Core Animation flip fix only. |
| `nand-apps-final` | Flip fix + Obama '08, Guangzhou Metro, Echo!. |
| `nand-dev` | Flip fix + `/Developer` (debugserver). |
| `nand-dylib` | Flip fix + a `DYLD_INSERT_LIBRARIES` proof. |

## Known limits

- **The display does not wake from idle sleep.** The system stays alive, the
  screen stays black. Same root cause as the missing self-reboot: the kernel
  wedges in a clock/power-controller spin at `PC=0xc05c7f94` and never finishes
  the power-down transition.
- **No self-reboot.** The watchdog device is modelled, but the guest never
  reaches the handler that would write it, for the reason above.
- **OpenGL ES apps do not render.** The MBX GPU is not emulated; the MMU
  handshake is acknowledged so they no longer peg the CPU, but they still hang.
- **Debugger attach fails** (see above).
- **WiFi passes no traffic** (see above).
- **`ideviceinstaller` installs do not launch** - they land in
  `/var/mobile/Applications`, which does not work. Inject into `/Applications`.
- **You cannot build new 2.x binaries.** `ld` refuses armv6 and armv7 output
  uses load commands 2.1's dyld cannot parse. Decrypted period apps are the
  only practical source of software.
