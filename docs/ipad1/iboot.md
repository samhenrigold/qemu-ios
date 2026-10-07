# Booting K48 through iBoot

The `ipad1` machine can boot the stock 7B500 kernelcache through iBoot-817.29,
including NOR DeviceTree loading, NAND filesystem lookup, decompression, and the
ARM kernel handoff. FirmwareKit's k48ap recipe builds and seals through
real iBoot by default. Test runners select a device directory with `--device`;
`--kboot` remains an explicit bring-up fallback.

A fresh device contains `iBoot.bin`, `nor.bin`, `gid-blobs.bin`, and `nand/`.
The key file is generated from that IPSW and its catalog key page; the emulator
has no compiled 7B500 key table. A4 records contain a 48-byte KBAG followed by its 48-byte IV/key plaintext
(96 bytes per record). Both production and development KBAGs are extracted
explicitly: each wraps the same DATA encryption key under a different GID.

```sh
firmwarekit create --catalog CATALOG --id k48ap-7B500 --ipsw IPSW \
  --out /path/to/device --helper LIGHTTOUCHDEVICE
python3 tests/ipad1/regress.py --device /path/to/device
```

CATALOG is LightTouchMac's `LightTouchMac/Resources/firmware-catalog.json`; LIGHTTOUCHDEVICE is the
LightTouchDevice executable. FirmwareKit takes the iBoot32Patcher next to
`firmwarekit` first, then `FIRMWAREKIT_IBOOT_PATCHER` or `IBOOT32PATCHER`,
then PATH. `tests/ipad1/fresh-device.sh IPSW OUT --helper LIGHTTOUCHDEVICE`
creates one and runs boot and persist. Test defaults point to
`~/Developer/qemu-ios-files/ipad1/repro/default-iboot`; create it explicitly or
pass `--device`. Older direct-kernel NAND stores lack the IMG3 kernelcache and
must be rebuilt before using this default.

The generated NOR DeviceTree enables HSIC for the emulated USB keyboard, as
the former direct-kernel DeviceTree did. This is board configuration data,
re-encrypted with the selected catalog key.

The 4.x data-protection preparation still explicitly boots its helper ramdisk
with `kboot`; normal sealing and runtime boots use iBoot. This is not yet a
stock USB restore pipeline.

This path uses **pattern-patched iBoot**, not verified secure boot. The stock
IPSW images are unpersonalized, and the captured device kernelcache's signature
does not match its payload. `iBoot32Patcher --rsa --debug -b ...` bypasses image
signature/personalization checks and enables the same debug boot arguments as
firmwarekit's KBoot. No emulator-side RSA success forgery or fixed-address iBoot
patch is required. The SHA, RSA, and AES device models remain functional.

## Prepare and run

Build QEMU using `scripts/configure-patched-ffmpeg` and `scripts/ccninja` as for
the normal iPad/iPod build. Supply your own firmware and iBoot32Patcher:

```sh
# firmwarekit's K48IBoot (Light Touch, Packages/FirmwareKit) makes this now; the Python tool is gone
```

The tested patcher is the arm64 macOS binary in Legacy-iOS-Kit v25.09.01. It
returns status 1 even on success; the builder also checks the output exists,
has the expected size, and differs from the input. Its `-a` environment boot
argument patch crashes this iBoot version; use the builder's `-b` path.

The NAND system volume must contain the original IPSW **img3** kernelcache at
`/System/Library/Caches/com.apple.kernelcaches/kernelcache`. Add
`--kernelcache /path/to/ipsw/kernelcache.release.k48` to the existing
`ipad1_rootfs.py build` command. Then bake and seal the store using the normal
[rootfs workflow](../research/userland-boot.md). Do not rebuild the shared golden store;
prepare a separate store for iBoot. The one-time sealing boot deliberately
halts before displaying the lock screen.

```sh
build/qemu-system-arm \
  -machine ipad1,iboot=/path/to/device/iBoot.bin,nor=/path/to/device/nor.bin,gid-blobs=/path/to/device/gid-blobs.bin,die-id=WORD2:WORD3,nand=/path/to/device/nand,nand-overlay=/path/to/overlay \
  -serial stdio

python3 tests/ipad1/iboot-check.py \
  --iboot /path/to/prepared/iBoot.bin --nor /path/to/prepared/nor.bin \
  --gid-blobs /path/to/device/gid-blobs.bin --die-id WORD2:WORD3 \
  --nand /path/to/nand --out /tmp/k48-check --unlock
```

Take WORD2:WORD3 from the device identity.json `die-id` pair; a zero identity
is rejected by iBoot. Use a private writable `nor-rw=` copy for persistent
NVRAM and 4.x effaceable storage.

The check captures the lock screen and optional unlock, then requests a clean
shutdown. Reuse the same output directory to verify a persistent second boot.
Abrupt termination can invalidate the FTL context and require a rescan. Logs
and PPM screenshots are retained in the output directory.

## Firmware settings and hardware corrections

firmwarekit's K48IBoot creates K48 SysCfg and NVRAM from the existing test iPad identity
(`--identity` accepts JSON overrides), without borrowing iPod settings. SysCfg
uses 20-byte records (four-byte tag plus sixteen inline bytes). The two 8 KiB
NVRAM banks contain CHRP partition headers, folded header checksums, generation
numbers, and Adler32 checksums. `debug-uarts=1` enables the iBoot console.

PMGR clock mux/divider reset values are reconstructed from the real K48
IODeviceTree clock table and iBoot's register decoder. Previously zero divisors
made iBoot publish a zero FMI frequency, causing the stock kernel to panic in
`AppleS5L8920XIOPFMI::_fmiInitTimings`. These values are a reconstruction, not a
raw PMGR hardware dump. GPIO 0x607 is low on the Wi-Fi configuration; iBoot then
prints `Radio not detected.` and renames the baseband `device_type` property to
`AAPL,ignore`, avoiding the long radio timeout and dead-radio alert.

A debugger capture at the stock kernel's Mach-O entry confirms:

- ARM SVC entry at physical `0x40063040`, r0 points to boot_args at `0x40938000`.
- ABI revision 1/version 2; virtual/physical bases `0xc0000000`/`0x40000000`;
  memory size `0x0f700000`; 1024×768 framebuffer, stride 4096, mirrored base
  `0x5f700000`; DeviceTree at `0xc092a000`, length `0xe000`.
- Serial/model/MLB/region, Wi-Fi/Bluetooth MACs, board/chip/ECID and debug-enabled
  agree with the direct boot configuration. iBoot constructs its memory map.
- All 64 clock table slots match the captured real device except source slot 2
  (the model reports its existing 400 MHz PLL; the real dump reports zero).
- iBoot supplies the panel IDs and ignores the absent baseband itself. Its
  normalized panel ID differs from the raw panel ID used by direct boot; both
  successfully attach the LCD.

Dump the DeviceTree **at handoff**, not after launchd: the kernel reclaims or
modifies that physical memory. Fixed addresses above describe this diagnostic
capture only; they are not patch locations in the preparation tool.

## Validation

The stock IPSW kernelcache reached root mount, launchd, and the activated lock
screen through iBoot. [Home-screen capture](screens/iboot-home.png) shows the
result after touch unlock (including the stock first-use icon-editing tip).
The direct-kernel smoke check passed after final integration. The complete
8-check iPod regression passed with the matching staged GLES shim: boot, fsck,
persistence, app installation, app launch, GLES, agent, and audio. That full run
used integration commit `6f3ae6e7a7`; the subsequently integrated battery/UART
work carries its own iPod boot/agent validation and was rechecked here for
normal iPad direct boot, iBoot unlock/shutdown, and recovery.


Clean cycles also reached the SpringBoard home screen after touch unlock,
then powered off through the PMU. With the integrated battery/UART fixes, a
subsequent boot on the same overlay mounted root at 4.5 seconds, started launchd
at 5.5 seconds, and lit the lock screen at 22.6 seconds (host wall time, not a
performance guarantee).

The integrated `ipad1-app` battery/UART fixes remove iBoot's gas-gauge timeouts.
Recovery mode reached its command prompt without the previously reported
`usb-high-curren` stack panic during a 90-second soak. USB restore and a
SecureROM/DFU boot chain remain untested. The physical iPad was not modified.

## Saved local run

The September 27 working artifacts are under
`~/Developer/qemu-ios-files/ipad1/iboot-2026-09-27/`: `prepared/` has the patched
firmware and NOR, `nand/` is an independent base, and `check/overlay/` is the
persistent guest state. `run.sh` launches that configuration; use the guest's
power-off slider before closing the emulator. These proprietary firmware and
NAND files are local artifacts and are not committed to the repository.

## SecureROM capture, September 28

A RAM-only limera1n/DFU exec read captured the physical iPad's 64 KiB ROM.
SHA-256: `4f34652a238a57ae0018b6e66c20a240cdbee8b4cca59a99407d09f83ea8082d`,
matching ipwndfu's A4 reference. The local artifact is
`~/Developer/qemu-ios-files/ipad1/securerom/SecureROM-574.4-RELEASE.dump`.
No flash was written. A preceding iBEC probe at address zero hung its console;
the successful ROM-stage read used the ROM alias `0xbf000000`, copied into the
DFU upload buffer by the ROM's own memmove. Firmware remains untracked.

## SecureROM, DFU and recovery USB

`-machine ipad1,bootrom=ROM` maps a supplied 64 KiB ROM at reset address zero
and its A4 alias. It is mutually exclusive with `iboot=` and `kboot=`. GPIO
board straps now yield the measured K48 POWER_ID (`0x01020001`); the DRAM
controller provides register readback and immediate DLL calibration completion.
The latter represents ideal RAM timing, not an analog DRAM timing simulation.

Production fuse values remain the default. For the unpersonalized development
certificates shipped in IPSWs, `development-fuses=on` selects the engineering
production/ECID fuse policy. The stock ROM verifies the certificate and image,
then decrypts with the catalog mapping of its selected development KBAG. This
is an explicit different security configuration, not a claim that retail fuses
accept an unpersonalized restore. Synthetic identity ECID and chip revision
now agree with what the ROM derives from the die-ID words.

On macOS the [libirecovery transport adapter](../../contrib/libirecovery-qemu/README.md)
connects unmodified host executables to emulated USB. It is a private replacement
transport library, not a native physical USB device. Build it, then run:

```sh
python3 tests/ipad1/restore-smoke.py \
  --device ~/Developer/qemu-ios-files/ipad1/repro/default-iboot \
  --rom ~/Developer/qemu-ios-files/ipad1/securerom/SecureROM-574.4-RELEASE.dump \
  --ipsw ~/Downloads/ipad1-ios32-feasibility/iPad1,1_3.2.2_7B500_Restore.ipsw \
  --libirecovery /tmp/libirecovery-qemu
```

The test starts isolated recovery and usbmux sockets, selects only the synthetic
ECID, and invokes stock `idevicerestore -c -z`. `-c` disables TSS personalization
and invokes the host tool's legacy limera1n flow; no custom firmware is supplied.
A separate stock `irecovery -f` test also boots the unmodified iBSS directly,
without that exploit flow. For 7B500 the restore path uses iBSS as its recovery
loader; this firmware does not require a separate iBEC handoff.

Validated: real ROM DFU (`05ac:1227`), stock iBSS recovery (`05ac:1281`), restore
ramdisk/DeviceTree/kernel upload, USB handoff to usbmuxd, and successful
`idevicerestore -z` termination after detecting restore mode. The ROM and
firmware files are supplied locally and are never repository artifacts.

`--erase` extends this test to a disposable APFS clone of the device NAND and a
private NOR with boot images erased (identity/NVRAM retained). It never restores
the selected source device in place. Logs and the disposable flash remain in
`--out` for inspection. Without `--out`, the test creates a temporary directory.

The 7B500 erase test also completed: stock `restored` created partitions and
filesystems, ASR transferred and verified the system image, firmware/NOR was
flashed, the NAND epoch was finalized, and the host reported **Restore Finished**.
This is stronger than the restore-mode smoke test, but is not a verified stock
SpringBoard boot. The restored NOR retained `auto-boot=false`; stock `irecovery
-n` on the disposable target changed that, after which ROM → LLB → iBoot loaded
the restored kernel. That follow-up remained at the Apple logo during a 55-second
observation and did not complete the powerdown check. The ordinary prepared
iBoot device continues to use the guest preparation/shims described above.

Validation artifacts from September 28 (local, not committed):

- `ipad1-stock-erase-final/restore.log`: full successful 7B500 erase restore.
- `ipad1-restore-smoke-final/restore.log`: final runner/adapter restore-mode pass.
- `ipad1-default-final-boots/`: two clean boots of the final default device.
- `ipad1-post-rom-regress/`: boot, USB, AFC, persistence and Wi-Fi pass; browser
  timed out during concurrent restore, then passed in `ipad1-post-rom-net/`.
- `ipad1-post-rom-audio/`: all four waveforms correlate above 0.8. Additional
  sound segments exposed the checker's positional matching; it now finds each
  required sound in order without reusing events. A fresh capture in
  `ipad1-audio-final/` passes all four checks (correlations 0.86–0.92).

These directories are under `/private/tmp`. Earlier fresh builds also completed
for 3.2/7B367 and 4.2.1/8C148. USB restore validation here is **7B500 only**.
