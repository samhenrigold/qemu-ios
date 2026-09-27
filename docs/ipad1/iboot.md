# Booting K48 through iBoot

The `ipad1` machine can boot the stock 7B500 kernelcache through iBoot-817.29,
including NOR DeviceTree loading, NAND filesystem lookup, decompression, and the
ARM kernel handoff. Direct kernel boot remains the default.

This path uses **pattern-patched iBoot**, not verified secure boot. The stock
IPSW images are unpersonalized, and the captured device kernelcache's signature
does not match its payload. `iBoot32Patcher --rsa --debug -b ...` bypasses image
signature/personalization checks and enables the same debug boot arguments as
`ipad1_kboot.py`. No emulator-side RSA success forgery or fixed-address iBoot
patch is required. The SHA, RSA, and AES device models remain functional.

## Prepare and run

Build QEMU using `scripts/configure-patched-ffmpeg` and `scripts/ccninja` as for
the normal iPad/iPod build. Supply your own firmware and iBoot32Patcher:

```sh
python3 imgtools/ipad1_iboot.py \
  --iboot /path/to/7B500/dec/iBoot.bin \
  --all-flash /path/to/ipsw/Firmware/all_flash/all_flash.k48ap.production \
  --patcher /path/to/iBoot32Patcher \
  --out /path/to/prepared
```

The tested patcher is the arm64 macOS binary in Legacy-iOS-Kit v25.09.01. It
returns status 1 even on success; the builder also checks the output exists,
has the expected size, and differs from the input. Its `-a` environment boot
argument patch crashes this iBoot version; use the builder's `-b` path.

The NAND system volume must contain the original IPSW **img3** kernelcache at
`/System/Library/Caches/com.apple.kernelcaches/kernelcache`. Add
`--kernelcache /path/to/ipsw/kernelcache.release.k48` to the existing
`ipad1_rootfs.py build` command. Then bake and seal the store using the normal
[rootfs workflow](userland-boot.md). Do not rebuild the shared golden store;
prepare a separate store for iBoot. The one-time sealing boot deliberately
halts before displaying the lock screen.

```sh
build/qemu-system-arm \
  -machine ipad1,iboot=/path/to/prepared/iBoot.bin,nor=/path/to/prepared/nor.bin,nand=/path/to/nand,nand-overlay=/path/to/overlay \
  -serial stdio

python3 tests/ipad1/iboot-check.py \
  --iboot /path/to/prepared/iBoot.bin --nor /path/to/prepared/nor.bin \
  --nand /path/to/nand --out /tmp/k48-check --unlock
```

The check captures the lock screen and optional unlock, then requests a clean
shutdown. Reuse the same output directory to verify a persistent second boot.
Abrupt termination can invalidate the FTL context and require a rescan. Logs
and PPM screenshots are retained in the output directory.

## Firmware settings and hardware corrections

`ipad1_iboot.py` creates K48 SysCfg and NVRAM from the existing test iPad identity
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
`~/Developer/qemu-ios-files/ipad1/iboot-codex/`: `prepared/` has the patched
firmware and NOR, `nand/` is an independent base, and `check/overlay/` is the
persistent guest state. `run.sh` launches that configuration; use the guest's
power-off slider before closing the emulator. These proprietary firmware and
NAND files are local artifacts and are not committed to the repository.
