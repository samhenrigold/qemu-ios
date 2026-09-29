# iPad 1 (K48AP / S5L8930 "A4") in QEMU-iOS

The `ipad1` machine boots stock iOS 3.2 (7B367), 3.2.2 (7B500) and 4.2.1 (8C148) to the home screen
through the real iBoot chain, with GPU drawing, Wi-Fi, a USB keyboard, USB restore and usbmux.
Project status is LightTouchMac `docs/STATUS.md`. The milestone log that got here (M1–M8, the research
corrections, the 7B500 address notes, the USB Ethernet kernel patch that was later deleted) is archived
in `../archive/ipad1-PLAN.md`; what follows are the parts of it that still govern the work.

## Documents

| File | What |
|---|---|
| `iboot.md` | Booting through iBoot-817.29: preparation, testing, the captured handoff comparison |
| `wifi.md` | The BCM4329 model behind the IOP SDIO ring; the default network |
| `usb-keyboard.md` | A USB keyboard on the host controller (CCK path) next to usbmux |
| `ios4.md` | The declared-inputs pipeline across a major version (8C148) |
| `location.md` | Location without GPS: the network location service answered by the proxy |
| `guest-services.md` | What guest code the iPad carries and why (AppSync dylib, `it_ethlink`, pasteboard) |
| `app-compat.md`, `app-compat-results.md` | The app-compatibility inventory and results (3.2.2 and 4.2.1) |
| `addresses-7B500.md` | Firmware addresses re-derived for 7B500 |
| `gli-dispatch-7B500.tsv`, `gli-dispatch-8C148.tsv` | GL dispatch layouts derived offline (`contrib/ipad1-gles/glitsv.py`): the reference the shim's runtime discovery was checked against, no longer a build input |
| `hw1-probes.log`, `screens/` | Register probes from the real unit; evidence screenshots (put new evidence in `qemu-ios-files`) |

The research that preceded the models (A4 SoC and board references, the kernel MMIO and IOP mailbox
contracts, the NAND stack, the SGX and GL notes, the keyboard/network study, the Bluetooth keyboard
scoping) is in `../research/`; each file says what superseded it. Gates: `../../tests/ipad1/regress.py`
and `../../tests/ipad1/fresh-device.sh` (see the repository README).

## Principle: vanilla guest (Sam, 2026-09-27)
The iPod needed guest services because its USB was limited. The iPad on 3.2.2 doesn't: prefer faithful
hardware models plus Apple's own services over USB (USB Ethernet, lockdown/AFC/installation_proxy,
syslog_relay, crash reports, DDI ScreenShotr). Guest code only where unavoidable: the GLEngine shim
(until an SGX model) and possibly a pasteboard helper. Keyboard target: a USB keyboard through the Camera
Connection Kit host path (stock USB HID); open question whether the kernel runs host (EHCI) and device
(usbmux) together, else a CCK plug/unplug mode switch. The IOHIDUserDevice daemon is only a fallback.

(Resolved since: host and device run together, `usb-keyboard.md`. Sam's 2026-09-28 decision that one
guest agent per architecture may suppress stock quirks that hurt the user, such as the USB "not
supported" alert, is recorded in LightTouchMac `docs/sweep/PLAN.md`.)

## Principle: IPSW-agnostic guest changes (Sam, 2026-09-27)
When this fans out to many iOS versions, no version may need hand work. So guest changes are only:
boot-args, or helpers/dylibs baked in by the image builder that find what they need by stable API or symbol
name at runtime (IOKit properties, dlsym/interposing). No byte patches at hand-found offsets. That rules out
the --usb-eth-link kernel patch long-term: it is being replaced by the it_ethlink helper (sets
LinkStatus=1 through IOKit, as USBEthernetSharing does). AppSync on the iPad is an injected dylib that
interposes MISValidateSignature, not an installd patch.

(The `--usb-eth-link` patch has since been deleted; `it_ethlink` is the baked helper, `guest-services.md`.)

## Network: Wi-Fi is the default, stock kernel (Sam, 2026-09-27)
The ipad1 machine has `wifi=on` by default: an emulated BCM4329 behind the IOP SDIO ring
(`wifi.md`), bridged to QEMU user networking (`type=user,id=wifi0` is created when no `wifi0`
netdev is given). Stock AppleBCMWLAN joins the open BSS "qemu-ios" on its own, DHCPs, and Safari and the
rest of the system use it, with no kernel patch and no guest helper. `wifi=off` opts out. USB Ethernet
remains as a secondary path: stock kernel plus the baked `it_ethlink` helper.

## Definition of done (Sam, 2026-09-27)

Complete fidelity, no stone unturned, before any other device:
- boots to SpringBoard; NAND persistence and overlays like the iPod machine
- full multitouch and gestures (multi-finger, pinch, rotate) through the real Zephyr2 path
- hardware keyboard via the iPad's own HID keyboard support (no simulated-touch typing)
- networking: USB Ethernet (present on 3.2.x: AppleUSBEthernetDevice) and, if feasible, Wi-Fi (BCM4329 over IOP SDIO)
- the guest services layer (agent, pasteboard, GLES bridge, status) ported to armv7/3.2.2
- rotation (accelerometer-driven and host-commanded), buttons, sensors, battery, audio
- USB/usbmux into LightTouchMac: app install, file access, the same features as the iPod
- GLES 1.1 + 2.0 hardware acceleration via a GLI shim; accelerated CoreAnimation
- LightTouchMac device profiles: the app runs the iPad with its own bezel, geometry and controls
- boot through real iBoot (M8)

STATUS.md says which of these are checked and how.

## App compatibility (Sam, 2026-09-27)
Test apps from Legacy Store (https://legacystore.app, Sam's own project) and the IPA collection in
~/Downloads/ios3: install each, launch, exercise touch/keyboard/rotation/GL, and record a compatibility
table (works / degraded / fails + cause) in `app-compat.md`. Failures feed back as bugs.

## Verification

Every milestone is checked against a real-iPad reference: serial logs, IORegistry dumps, screenshots.
`regress.py`-style harness per machine; the iPod machine's suite must stay green through every shared-model
refactor.

## Audio (2026-09-27)
Out works: CS42L61 + Mikey (i2c0 0x39; the codec waits for its 'mikey' function) -> AppleARMIISAudio ->
CDMA ch 0x1a (16 x 4 KiB IOAudio ring, streamed in 10 ms virtual-time steps) -> i2s0 FIFO ->
`-audio driver=wav|coreaudio`. Boot, lock, unlock and Notes keyboard clicks land in the WAV (correlation
0.86-0.92 against the rootfs files at 1.00x); `tests/ipad1/audio-check.py` checks boot/unlock/lock/unlock.
Rate: the I2S frame rate is read from PMGR NCO n (+4 = 64 * fs, written by the NCOFrequency function when
the device rate is set). The device stays at 44.1 kHz; every on-device sound is 11.025-44.1 kHz and the HAL
resamples to it. 48 kHz media: Safari (USB Ethernet, USB keyboard) playing a 48 kHz stereo 1 kHz tone WAV
from a host HTTP server (Range requests needed, or the player shows a crossed-out play icon) lands in the
44.1 kHz host WAV as 1000.00 Hz for 3.97 s of 4 s, i.e. right pitch and speed. A device-side switch to
48 kHz (an app setting the preferred hardware rate) is still unobserved.
Microphone (2026-09-27): i2s0 RX (+0x34 command, +0x38 FIFO) reads frames from a QEMU input voice (the
Mac's microphone under `-audio coreaudio`) or, with the test-only `-global
driver=s5l8930.i2s,property=tone-hz,value=N`, a synthetic stereo sine; CDMA ch 0x1b streams it into the
guest's ring with the same pacing as playback. `tests/ipad1/mic-check.py` builds `contrib/ipad1-mictest`
(an AudioQueue input recorder) into a scratch store and checks a 10 s recording in the guest: 1000.0 Hz from
a 1000 Hz tone (440.0 from 440), 44025 frames/s, 0 discontinuities. Needed on the way: back-to-back chains keep
one sample clock (a late go used to drop ~3% at every 64 KiB boundary) and 1 ms pacing steps (the HAL reads
input up to 96 frames behind its clock; 10 ms steps left it reading stale frames every cycle). Shazam 1.5.3 (installed on golden-appsync) records through it:
"Listening..." with its level meter (screens/2026-09-27-shazam-listening.png) while CDMA 0x1b runs 64 KiB
capture chains back to back (16384 frames each, 0.37-0.38 s apart).

## Bluetooth: parked, BTServer disabled (2026-09-27)

Bluetooth has no controller model. UART3 (BCM4329 HCI) is a silent Samsung UART, and its CDMA
receive chains park instead of reading zeros (36d1915323). BTServer's retries then cost SpringBoard
a ~1 s stall about every 12 s. That's BluetoothManager blocking on BTServer: respcheck measured
1.07-1.19 s taps at a ~12 s period. With BTServer unloaded on nand-jb, every tap took 191 ms
(control: 790 ms on every tap). Hiding the DT node (uart3/bluetooth compatible=none) does not help.
So `ipad1_rootfs.py bake` sets `Disabled` in `com.apple.BTServer.plist` by default
(`--keep-bluetooth` to skip). With it, respcheck shows 16/16 taps at 178-201 ms and Settings >
General shows Bluetooth greyed as **Unavailable** (screens/settings-bluetooth-unavailable.png).
Sam: no Bluetooth is fine.

Parked work, for whoever wants a real controller. In the ipad1-kbd worktree, `git stash@{0}` and
`stash@{1}` hold it, with debug prints still in:
- UART3 on `it_bt_chardev()` (ipod_touch_bt.c).
- The UART's Rx DMA request (exynos4210_uart sysbus irq 2) wired to a CDMA "uart-rx" GPIO. It pulls
  bytes into a parked chain on channel 0xc.
- A pause/resume model. AppleCDMA's append/position query (c044d7e4) does `ctrl |= 0x24`
  (hold + abort, CDMA v2), polls until `(ctrl & 0x230000) != 0x10000`, reads +0x0C/+0x10/+0x14,
  then writes the saved ctrl back (state bits = running, no go) to resume.

That gets HCI Reset and the 0xfc18 baud reply through. The next Rx-timeout harvest panics in
AppleOnboardSerial (`command->headCount <= byteCount`, byteCount - headCount = -2034). The v2 byte
count at c044d94a is `ring[idx(+0x14)].w5 - BC + ([cdma+0x54] + Σ ring[i].len for i = idx(+0x14) ..
[cdma+0x48]-1)`, where ring entries are {next, flags 0x303, addr, len, cmd*, 0, 1, 0}, so w5 is 0.
The sum is 2048 on the first harvest and 0 on the next, so the model's +0x14 has to track how the
driver advances [cdma+0x48]/[cdma+0x54] on completion. Decode those, then retest. Reporting +0x14
as the prefetched next descriptor instead avoids the panic but breaks the first count.
