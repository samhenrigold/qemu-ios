# iPad 1 (K48AP / S5L8930 "A4") on iOS 3.2.2 (7B500): plan

Supersedes `~/Downloads/ipad1-ios32-feasibility/plan/plan.md` (7B367). The research reports there remain
the evidence base for *mechanisms*; every *address* in them is 7B367 and is re-derived for 7B500 in
`docs/ipad1/addresses-7B500.md`.

## What changed from the research plan

| Research plan said | Correction |
|---|---|
| Target 3.2 (7B367, xnu-1504.2.27, iBoot-817.28) | Target **3.2.2 (7B500, xnu-1504.2.60~1, iBoot-817.29)**. Same kext set (139), same mechanisms, new addresses. |
| LLB `0x4ff00000` vs iBoot `0x5f700000` proves a 256 MiB mirror | Wrong arithmetic (difference is 0x0F800000). Mirror is still likely (iBoot links at 0x5ff00000 with 256 MiB of DRAM at 0x40000000) — **measured on the real iPad in HW-1**, not assumed. |
| No `0xc0000000` alias needed | Unverified; HW-1 probes it. |
| Kernel self-format yields a mountable filesystem | It yields an **empty FTL** (no MBR, no HFS+). The base image comes from a **real 3.2.2 NAND dump** (HW-2), with a generator as the long-term path and self-format as its test oracle. |
| iBoot sig-check bypass "works the same, no new mechanism" | Unresolved (SHA1 semantics, security-policy word write site untraced). Moved off the critical path by booting the kernel directly first. |
| GLI path has 17 entry points; MBX route covers graphics | 19 entry points; MBX route is **ES 1.1 only**. ES 2.0 needs its own GLI shim. |
| M1 = iBoot console | M1 = **kernel on serial via direct kernel boot**. iBoot becomes a fidelity milestone. |
| GPIO IC `0x800+4g` enables, `0x840+4g` disables (kernel-platform report §3) | Reversed: `0x800` **disables**, `0x840` **enables** (7B500 `disableVectorHard` 0xc0643a6c / `enableVector` 0xc0643aa4; agrees with openiBoot). The report's AppleS5L8930X addresses are 0x2000 low for 7B500. |
| Kernelcache LZSS "ends 14 bytes short" | Root cause was img3 decryption: the last partial AES block is encrypted into the tag padding. Fixed in `tools/img3tool.py`; 7B500 kernelcache now passes Adler-32 (9,383,936 B). |

## 7B500 vs 7B367 (from `docs/ipad1/addresses-7B500.md`)

- **iBoot-817.29 and the EmbeddedIOP firmware are structurally identical to 817.28**: same link bases, same
  MMIO literals at the same VAs, same security-policy word `[0x5FF2CFC0]`, same IOP vector table, stacks
  and config offsets. The iBoot boot-path and IOP mailbox specs transfer verbatim; the IOP blob sits at
  0xc074e000 in the kernelcache (was 0xc074c000).
- **Device tree:** no behavioural differences.
- **Kernel:** entry 0xc0063040, `boot_args` revision 2, CommandLine at +0x38. AMFI's four boot-args are
  still gated on `debug-enabled`. The `debug_enabled` global is **still VA 0xc02787b8 / phys 0x402787b8**
  (verified directly; the agent's "moved to 0x...d8" was wrong). New: `mac_proc_check_get_task{,_name}`
  found at 0xc01d4614 / 0xc01d46ac (slot-order identification, not independently confirmed).
- **NAND:** `WMR_Start` 0xc07f3a64; reformat/wipe boot-args unchanged; the NANDDRIVERSIGN version string
  changed, so any baked NAND must carry the 7B500 one.
- **Shared cache moved wholesale:** `"AppleMBXDevice"` is at **0x336bdb5c** (verified), OpenGLES base
  0x336a8000, QuartzCore 0x34279000.
- Only the kernel `debug_enabled` and the MBX string were independently re-checked by the main session;
  treat the other agent-derived VAs as "high confidence, unreviewed" until something uses them.

## Ground truth: the real iPad

Sam's iPad 1 is available with no restrictions (wipe, downgrade, jailbreak), plus a 30-pin serial cable.
The A4 bootrom has limera1n, so we always have pre-iBoot code execution. Tooling: Legacy-iOS-Kit
(limera1n, powdersn0w downgrade to 3.x, iBoot32Patcher, xpwntool).

- **HW-1 (non-destructive, ready):** `contrib/ipad1-hw/` — limera1n → signature-patched 7B500 iBSS → iBEC with two
  new console commands, `md <addr> [n]` and `mw <addr> <val>` (replacing `bgcolor` and `go`), read back over
  `irecovery -s`. Probes in `contrib/ipad1-hw/probes.txt`: ChipID, POWER_ID, PMGR PLL/clock/gate registers, timer,
  GPIO, VIC, and the DRAM mirrors. This is an interactive register oracle we can reuse whenever the
  emulator stalls on an unknown value.
- **HW-1 results (2026-09-26):** limera1n + pwned iBSS + md/mw iBEC work. Measured: ChipID
  `31800387 80758000 00000000 00000000`, POWER_ID `01020001`. Reading PMGR `0xbf100000` from iBEC hangs
  it, as does reading past a block's end; remaining registers are read from the running jailbroken iOS
  instead (kernel-memory reader, `hw2/regs/`).
- **HW-2 status:** the iPad already runs 3.2.2 (downgraded from 5.1.1, jailbroken, OpenSSH). Captured:
  MBR (4 KiB sectors; system 0xAF @LBA 63, data 0xAE, tiny p3), the 1.34 GB system partition, p3, dmesg
  tail, sysctl hw (bus/periph 100 MHz, timebase 24 MHz, cpu/mem 0), nvram. Treat as working data, not a
  pristine reference (see memory note). Still to capture: raw NAND pages, IORegistry, early boot log.
- **HW-2 (destructive, original plan):** powdersn0w restore to 3.2.2, jailbreak; capture: raw NAND dump (all banks, with
  spare/meta), the device tree and boot_args as the kernel receives them, verbose kernel log over serial
  (`serial=3 -v debug=...` via patched iBEC), IORegistry dump, syslog. These become the reference logs the
  emulator is diffed against.
- **HW-3 (as needed):** SecureROM dump (limera1n allows it) so the emulator can eventually boot from ROM like
  the iPod's `bootrom_240_4`; live register reads while debugging a model; performance baselines.

## Milestones (kernel-first)

**M0 — Tooling (≈1 week).** Promote `tools/` (fixed img3tool, vfdecrypt, dscextract, hfslist, DT dump) into
`qemu-ios/imgtools/` with one self-check each. A `fetch-7b500.sh` that reproduces `7B500/dec` from the IPSW +
keys. Commit plan + research to a qemu-ios branch.

**M1 — Kernel prints on serial (≈3–4 weeks).** New `ipad1` machine: `cortex-a8`, DRAM at 0x40000000 plus
whatever mirrors HW-1 confirms, 4× PL192, PMGR timer + clocks (enough for `AppleS5L8930XPerformanceController`
and the timebase), ChipID, GPIO, UART0 (reuse), watchdog. A direct-kernel loader: map the kernelcache
Mach-O, build the flattened DT from `DeviceTree.img3` filled the way iBoot fills it (memory-map, chosen,
vram, frequencies, serials — HW-2 gives the real values), build boot_args (`-v serial=3 debug=… amfi… `),
enter at the Mach-O entry with the MMU off. Success: xnu banner → kexts matching →
"Waiting for root device". No CDMA, SHA1, PKE, H2FMI, NOR needed.

**M2 — Root filesystem mounts, launchd runs (≈4–6 weeks).** IOP HLE: `0x86300000` control regs, IOP VIC
SOFTINT doorbells, ring 0 control messages, ring 5/6 FMI commands (1,2,4,8,11 read path) against a
page-directory NAND store seeded from the HW-2 dump. CDMA: only the AES path the kernel uses (FTL keys,
data-partition key 0x89B) — not the full descriptor engine. D1815 PMU + new I2C controller (enough to not
panic). `debug-enabled` forced (DT + 7B500 kernel global) so AMFI honors its boot-args.
Success: launchd, then SpringBoard attempts, diffed against the HW-2 serial log.

**GLES (2026-09-27, merged 83609cba9b):** ES 1.1 and 2.0 apps render through the GLI shim to the host executor
(screens/2026-09-27-gles1-gltest.png, -gles2-gltest2.png). Open: accelerated CoreAnimation draws on the host but
IOMFB swaps never complete (contrib/ipad1-gles/README.md); iPod GLES fixture coverage dropped 0.42→0.24 after
the gles-host change (still passes; being checked). Battery ~80% (a3bd58585e). App: LightTouchMac ipad1 branch
has overlay, usbmux, USB keyboard, upright panel, rotation. Net: guest en1 DHCP only; host bridge pending.

**M3 DONE + rotation (2026-09-27):** copy-on-write NAND overlay (f8faca33e5: `nand-overlay=DIR`; base
read-only; reset = delete overlay; changes survive unclean kill + reboot, fsck clean). Rotation through the
accelerometer to all four orientations (screens/2026-09-27-rotate-*.png). Slide-to-power-off after a held
Hold (02ac1db7e4).

**Input (2026-09-27):** apps launch by tap and Home returns (screens/2026-09-27-app-*.png); host multi-touch
via QEMU mtt events drives Maps pan/pinch (9ec85b1f05); host-settable orientation (01c883227b); USB
keyboard on the CCK host path types into Notes with usbmux still up (merged, docs/ipad1/usb-keyboard.md).
Full iPod regression (8/8, incl. appinstall/applaunch/gles/audio) green on this head.

**M4 DONE — home screen (2026-09-27, ba6cd0b812):** screens/2026-09-27-home-screen.png. Unlocked by a host
drag through the real Zephyr2 → AppleMultitouchZ2SPI → SpringBoard path (portrait-native digitizer axis map,
report ids 0xBF/0xAF, the K48 sensor profile from the real unit's IORegistry). iPod regression green.
Next: persistence across reboots, app launch, multi-finger gestures, rotation, keyboard, network, app.

**Activated lock screen (2026-09-27, 4d0eb1b54f):** screens/2026-09-27-lock-screen-activated.png — slide to
unlock, clock, wallpaper, no baseband alert (real MACs + baseband unmatch fixed activation and the alert).
The display switches off between short lit windows like a real idle lock screen, so screendumps need
timing or a wake. Open: Home via the machine chord doesn't wake the display; battery shows red / Not
Charging although USB is attached.

**Sleep (2026-09-27, 6bffc067e6):** SpringBoard comes up then the device auto-locks and deep-sleeps
("USB cable detached" → "System Sleep" → "pmu go hib"). Next: cable present like a Mac-attached iPad;
PMU wake on buttons; BT UART has no chip ("bluetooth: Software Overflow").

**SpringBoard draws (2026-09-27, 5eae009b6d):** screens/2026-09-27-first-springboard.png — "Connect to
iTunes" plus the no-baseband alert. Next: baseband presented as on a Wi-Fi iPad; activation.

**M2 DONE (2026-09-27, 107d5406d2):** launchd runs: fsck, / and /private/var mounted, multitouch firmware
downloaded (0x0146.bin, as on the real iPad), mDNSResponder and sandboxd start. Fixes on the way: CDMA IRQ
numbering (0x30+n), SHA-1 engine. Next: SpringBoard on screen; baseband SPI2 (0x82200000) is polled.

**M2 status (2026-09-27):** root filesystem mounts ("BSD root: disk0s1") from the generated pristine
store; next panic is the first CDMA M2M transfer. The nondeterministic stall was the display pipe's
DP_FLAGS reading 0x20 (fixed 8b54f07424).

**M2 status (2026-09-26):** IOP HLE done; the kernel self-formats a blank store with
`nand-enable-reformat=1` and re-opens it on the next boot (VFL_Open/FTL_Open OK, "waiting for root
device"). Remaining for M2: an FTL carrying the MBR + system partition (generator in progress), then the
root mount and launchd.

**M3 — Writes and persistence (≈2–3 weeks).** FMI program/erase opcodes; overlay persistence matching the
iPod machine. Self-format (`nand-enable-reformat=1` on a blank image) as the write-path stress test.

**M4 — GPU-less home screen (≈3–4 weeks).** Display pipe 0 + CLCD + reused MIPI-DSIM; DART2 (identity first,
or drop `iommu-parent`); IOMFB swap FIFO, VBL IRQ 0x2a; SGX node removed; SpringBoard env `CA_ENABLE_OGL=0`
(a failed EAGL init is never cached, so leaving it on retries GLEngine loading repeatedly) and
`MBX2D_PAGE_FLIP=0` (single page: the scaler-backed page copy has no CPU fallback, it is just skipped).
CA's software renderer draws into the IOMFB surface; UIKit, not CA or the scaler, rotates to portrait.
No scaler model needed. Measure software CoreAnimation speed at 1024×768 early — if it's unusable, M7
moves up. Details: `userland-gl-display.md`.

**M5 — Input, USB, sensors (≈3–4 weeks).** Zephyr2 multitouch on SPI1, DWC OTG + TCP-USB bridge + usbmuxd,
LIS331DLH accelerometer, bq27545 gas gauge, buttons, orientation.

**M6 — GLES 1.1 via mbxshim (≈1–2 weeks).** mbxshim does not run unmodified: the dispatch table grew
822 → 826 slots (3 inserted at 761–763, one new at 825). Set `N_SLOTS=826`, regenerate `slotmap.txt`, add 3
to the 23 hard-coded indices ≥761. Still needs the `"AppleMBXDevice"` patch (cache 0x336bdb5c) and
`MBXGLEngine.bundle`. ES1 only on 3.2 — CA stays in software. May be skipped in favour of M7.

**M7 — GLES 1.1 + 2.0 via a GLI shim.** Replace `GLEngine.bundle/GLEngine` (one rootfs file, no
shared-cache patch): 12 of the 19 `gli*` entries are called; `gliGetVersion` returns 1 so no
IOAcceleratorES service is needed. Fill the two 826-slot tables (`gli-dispatch-7B500.tsv`) the mbxshim
way; ES2 core sits below slot 761 so the host wire numbering carries over. The accelerated pixel-format
flag decides whether CA's own compositor also goes to host GL. Rejected alternative: forwarding at the
~80-function `gld*` driver layer.

**M8 — Boot-chain fidelity.** Note: iBoot sets `boot_args.version = ((CHIPID[0] >> 9) & 0x7f) + 1` and the
kernel panics unless it's 2, so the ChipID model's power-epoch field must be 1 (iEmu's 0x31800587 gives 2). iBoot-817.29 via `direct-iboot`: full CDMA + AES/KBAG oracle, SHA1, PKE forge,
H2FMI, NOR on SPI0; boot logo, recovery mode, DFU. Later: boot from the dumped SecureROM.

**M9 — LightTouchMac device profiles.** The app assumes one machine (320×480, 128 MiB); add a per-device
profile (machine, geometry, bezel, NAND set, GL shim).

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

## Principle: vanilla guest (Sam, 2026-09-27)
The iPod needed guest services because its USB was limited. The iPad on 3.2.2 doesn't: prefer faithful
hardware models plus Apple's own services over USB (USB Ethernet, lockdown/AFC/installation_proxy,
syslog_relay, crash reports, DDI ScreenShotr). Guest code only where unavoidable: the GLEngine shim
(until an SGX model) and possibly a pasteboard helper. Keyboard target: a USB keyboard through the Camera
Connection Kit host path (stock USB HID); open question whether the kernel runs host (EHCI) and device
(usbmux) together, else a CCK plug/unplug mode switch. The IOHIDUserDevice daemon is only a fallback.

## Principle: IPSW-agnostic guest changes (Sam, 2026-09-27)
When this fans out to many iOS versions, no version may need hand work. So guest changes are only:
boot-args, or helpers/dylibs baked in by the image builder that find what they need by stable API or symbol
name at runtime (IOKit properties, dlsym/interposing). No byte patches at hand-found offsets. That rules out
the --usb-eth-link kernel patch long-term: it is being replaced by the it_ethlink helper (sets
LinkStatus=1 through IOKit, as USBEthernetSharing does). AppSync on the iPad is an injected dylib that
interposes MISValidateSignature, not an installd patch.

## Network: Wi-Fi is the default, stock kernel (Sam, 2026-09-27)
The ipad1 machine has `wifi=on` by default: an emulated BCM4329 behind the IOP SDIO ring
(docs/ipad1/wifi.md), bridged to QEMU user networking (`type=user,id=wifi0` is created when no `wifi0`
netdev is given). Stock AppleBCMWLAN joins the open BSS "qemu-ios" on its own, DHCPs, and Safari and the
rest of the system use it, with no kernel patch and no guest helper. `wifi=off` opts out. USB Ethernet
(below) remains as a secondary path: stock kernel plus the baked `it_ethlink` helper, with the
`--usb-eth-link` kernel patch as an opt-in fallback (`7B500/k48-kboot-ethpatch.bin`).

## Keyboard / network decisions (2026-09-27, docs/ipad1/keyboard-and-network.md)
1. Keyboard: USB keyboard via the Camera Connection Kit host path (EHCI + usb-kbd) — now; IOHID daemon as fallback.
2. USB Ethernet: the device's own Apple USB Ethernet configuration bridged to libslirp — now.
3. Wi-Fi: fake BCM4329 behind the IOP SDIO ring — last item; until then SDIO answers "no card".

## USB Ethernet link: the one kernel patch (2026-09-27)
USB Ethernet works end to end: en1 takes a DHCP lease from usbmuxd's slirp (10.0.2.0/24), Safari loads
www.google.com and Maps draws live tiles (`screens/2026-09-27-net-*.png`). Three parts:
- host: usbmuxd-qemu `ipad1-net` selects configuration 4 (PTP + Apple Mobile Device + Apple USB Ethernet),
  sets the Ethernet interface to alt 1, bridges its bulk pair to libslirp; usbmux keeps working beside it.
- guest prefs: `ipad1_rootfs.py` seeds an en1 DHCP service (vanilla: a plist, like a configured unit).
- guest: the baked `it_ethlink` helper raises the link through IOKit (LinkStatus 0 then 1 on each of the
  service's interest messages), as USBEthernetSharing does on a tethering iPhone, and the kernel is stock
  (guest-services.md). The byte patch described below stays in `ipad1_kboot.py` as an opt-in fallback
  (`--usb-eth-link`).
  On by default is safe because it only fires when a host selects the Ethernet interface's alt setting 1,
  which only usbmuxd-qemu's ipad1 branch does. The built-in USB host configures configuration 3 (no
  Ethernet) and bridges without that branch never select it, so for them the kernel behaves as stock.

Why a patch: AppleUSBEthernetDevice marks its link active, starts its output queue and arms the first
bulk read only in `setProperties({"LinkStatus": 1})`. The only stock caller is configd's
USBEthernetSharing, and only while MobileInternetSharing tethers; a Wi-Fi iPad has no carrier
provisioning (misd State 1020, ENOTSUP), so en1 stays `Link Active: FALSE` and IPConfiguration never
DHCPs. Nothing the host sends over USB can raise the link (alt 0 only lowers it). The patch (2 sites, 24
bytes, byte-checked) makes the host's SET_INTERFACE alt 1 run that same LinkStatus=1 path; details and
addresses in `USB_ETH_LINK` in `imgtools/ipad1_kboot.py`.

Not covered yet: the real-iBoot boot path, which loads the signed kernelcache from NAND, so kboot's patch
never applies there. Open decision: have the machine apply the same bytes at runtime for that path.
Full-fidelity alternative for later: Wi-Fi (BCM4329), which the stock stack brings up by itself.

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
Gap — microphone: capture is i2s0 RX on CDMA ch 0x1b (FIFO 0x84500438; RX command +0x34, RX FIFO +0x38).
The paced CDMA only moves memory -> FIFO, and the I2S RX FIFO reads 0. To add: a QEMU `AUD_open_in` voice
feeding an RX ring that +0x38 drains, and the device -> memory direction in `cdma_paced_advance`. Needs an
app that records (3.2 has no Voice Memos) to prove it.

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

## After SpringBoard: app compatibility (Sam, 2026-09-27)
Once SpringBoard and installs work, test apps from Legacy Store (https://legacystore.app) and the IPA collection in ~/Downloads/ios3:
install each, launch, exercise touch/keyboard/rotation/GL, and record a compatibility table
(works / degraded / fails + cause) in docs/ipad1/app-compat.md. Failures feed back as bugs.

## Verification

Every milestone is checked against a real-iPad reference: serial logs (M1–M3), IORegistry dumps (M2–M5),
screenshots (M4+). `regress.py`-style harness per machine; the iPod machine's suite must stay green through
every shared-model refactor.
