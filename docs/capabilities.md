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

## Forward TCP to the device

`iproxy` works, so any device-side TCP listener is reachable from the host:

```sh
iproxy 2222:62078          # host port 2222 -> lockdownd on the device
```

Measured 2026-07-31 with a raw client on a plain socket (deliberately not
`libimobiledevice`, so the forwarded path is what is being measured): a full
lockdown session including the TLS handshake, `StartService`, then 8 MiB pulled
back over AFC with a matching SHA-256 at six different chunk sizes. Peak
~8.6 MB/s, ~5 ms round-trip, and eight concurrent forwarded connections with no
stalls or cross-talk. Chunk sizes that are *not* multiples of the 512-byte max
packet (65535, 1000) were included on purpose - that is the residue case the
bulk-IN bug used to mishandle - and came back byte-identical.

Small chunks are round-trip-bound, not bandwidth-bound: 8 MiB at 1000 bytes per
operation is 8390 round trips and takes 47 s, against 0.93 s at 65535.

One trap if you write your own client instead of using `libimobiledevice`:
**lockdownd here predates RFC 5746**, so OpenSSL 3 refuses the handshake with
`UNSAFE_LEGACY_RENEGOTIATION_DISABLED` (or a bare `UNEXPECTED_EOF_WHILE_READING`
if you let it offer TLS 1.2/1.3). It needs TLSv1 pinned as both minimum and
maximum, `SECLEVEL=0` ciphers, and `OP_LEGACY_SERVER_CONNECT`. That is a
host-toolchain problem, not an emulator one.

SSH over USB would work on this transport; what is missing is a listener on port
22, and the work there is cross-building dropbear for armv6.

## Run real App Store apps

Third-party apps from 2008 run, fully interactive - but only when injected into
`/Applications` in the NAND image offline. `ideviceinstaller install` now
completes over USB, and the icon appears on SpringBoard, yet the app still will
not launch by any route available from the host; see the known-limits entry for
the two mutually exclusive gates that cause this. Offline injection is the only
way to get a running app. installd rebuilds its cache every boot by scanning, so
a bundle only has to be present.

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
carries, and debugserver ends its session after a failed attach (`vAttach;<pid>`
returns `E05`).

The USB link no longer wedges under repeated short-lived connections. It used to,
which was a symptom of the bulk-IN residue bug; with that fixed, ~1000 sessions -
including 200 failed attaches torn down with an abrupt RST, over 3198 distinct
mux channels - ran clean, and `idevice_id`/`ideviceinfo`/a SHA-256-verified AFC
read all passed afterwards. (This old debugserver does not implement
`vAttachName`, `qHostInfo`, `qProcessInfo` or `qSupported` - they return the empty
packet - so `vAttach;<pidhex>` is the attach form.)

## Drive it headlessly

```sh
$Q -M iPod-Touch,... -qmp tcp:127.0.0.1:4530,server=on,wait=off
python3 contrib/ipod-touch-qmp.py 4530 shot out.ppm
python3 contrib/ipod-touch-qmp.py 4530 tap 275 243
python3 contrib/ipod-touch-qmp.py 4530 swipe 160 400 160 100
python3 contrib/ipod-touch-qmp.py 4530 button home
```

The hardware buttons live behind the host **Command** modifier, so that plain
keys stay free for text entry: home is Command+Shift+H, power Command+L, volume
Command+- / Command+=. Use `button home` rather than `key h` - `key h` now types
an `h` into whatever has focus and silently does nothing else, which is an easy
way to lose an hour wondering why taps are landing in the wrong place. They are
GPIO levels the guest samples rather than edges, so the press needs a hold time;
`button` supplies one.

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
the CDC control channel - publishes an `IO80211Interface` with the right MAC,
runs a real scan (the network `qemu-ios` appears in Settings with signal bars),
and **joins it through its own association state machine** - `Link Up on en0`,
DHCP lease, ARP and TCP on the wire. `IPOD_WIFI_FAKE_LINK` is no longer needed.

**Traffic passes, and Safari renders a page.** With the network stack enabled
offline (`imgtools/setup_networking.py`: SC preferences + mDNSResponder +
known-network seed), the device auto-joins at boot and loads a page - by IP, and
by hostname for locally-resolvable names, with the DNS query on the wire. The one
remaining gap is remote/internet names: `SCNetworkReachabilityCreateWithName`
judges a name that needs a unicast DNS round-trip unreachable *before* issuing
the query, so those fail with no packet sent. See `docs/networking.md`.

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
- **OpenGL ES apps do not render.** The MBX GPU is not emulated. With
  `mbx-irq=on` the MMU handshake is acknowledged so they no longer peg the CPU
  (the verified 0x1020 fix) - but that option is **off by default**, so in the
  stock configuration a GLES app still spins ~21M reads on the MMU register, and
  either way it still hangs without rendering. Making `mbx-irq` default on is a
  pending decision (it needs a boot test that the 2D path is unaffected).
- **Debugger attach fails** (see above).
- **WiFi remote-name DNS is the last gap** - scan, association, DHCP and IP/local-name browsing all work; only names needing a unicast DNS query fail (an SCNetworkReachability gate). See the WiFi section.
- **`ideviceinstaller` installs cannot produce a launchable app.** The install
  itself works - it used to die at `PackageExtractionFailed` because device->host
  reads were corrupt, and now a genuine signed `.ipa` reaches `Install:
  Complete`, is listed by `ideviceinstaller list`, and its icon appears on
  SpringBoard without a reboot. It still will not run, and there is no way around
  it from the host, because the two gates are mutually exclusive:
  - A **signed** App Store `.ipa` is FairPlay-encrypted (`cryptid 1`). It
    installs, then dies at its first `__TEXT` page: `AMFI: Invalid signature but
    permitting execution` followed by `CODESIGNING: vm_fault_enter(...)
    *** INVALID PAGE ***`, and the icon bounces.
  - A **decrypted** bundle (`cryptid 0`) never gets that far: installd rejects it
    at `VerifyingApplication (40%)` with `ApplicationVerificationFailed`.
    `amfi_allow_any_signature=1` disables signature enforcement at *exec*, not
    the verification installd runs at *install*, and decryption breaks the
    CodeDirectory hashes.

  Separately, `/var/mobile/Applications` still does not work, re-verified after
  the USB bulk-IN fix by a same-boot A/B with two decrypted, known-good apps:
  Obama '08 in `/Applications` launches, while the identical-vintage Guangzhou
  under `/var/mobile/Applications/<UUID>` gives "The application ... cannot be
  opened". That boot logs no codesigning event at all for it - the binary is
  never exec'd - so this is launch bookkeeping in userspace, a different
  mechanism from the two gates above.

  **Inject into `/Applications` offline. That is the only route that runs.**
- **You cannot build new 2.x binaries.** `ld` refuses armv6 and armv7 output
  uses load commands 2.1's dyld cannot parse. Decrypted period apps are the
  only practical source of software.
