# iPad 1 / iOS 5 spike (and what 4.3 already needs)

A bounded probe (2026-09-28, branch `ios5-spike` off `ipad1` @ `082b45e77d`) of iOS 5.1.1 (9B206)
on the `ipad1` machine, with 5.0.1 (9A405) and 4.3.5 (8L1) read alongside. The purpose is the
inventory, not the boot: each blocker is named by emulator component and by its class in
LightTouchMac `docs/fidelity-ledger.md` (R register-level, H high-level stand-in, P guest patch,
S stub), and whether the fix raises the class or adds a special case. No special case was added.

Inputs: the three IPSWs (sha1 `ad9b6074…` 9B206, `732fc3ca…` 9A405, `66a11463…` 8L1) and the public
key pages (ipsw.me mirror of the wiki), rendered into the pipeline's keys-file format; every
component decrypted and checksummed by `imgtools/ipad1_fw.py` (the kernelcache Adler-32 verifies).
`manifests/ipad1-9B206.json` is the catalog entry. The 8L1 ramdisk keys are not published, so 4.3.5
has kernel + DeviceTree + iBoot only.

## Static: what 5.x expects that 4.2.1 did not

| Area | 4.2.1 (8C148) | 4.3.5 (8L1) | 5.0.1 / 5.1.1 (9A405 / 9B206) |
|---|---|---|---|
| Kernel | xnu-1504.58.28 | xnu-1735.47 | xnu-1878.4.46 / 1878.11.10 |
| iBoot | 931.71.16 | 1072.61 | 1219.43.32 / 1219.62.15 |
| IMG3 security epoch (SEPO) of LLB/iBoot/kernelcache | 1 | **2** | 2 (+ CHIP tag) |
| boot_args.Version demanded by `pe_identify_machine` | 2 | **3** | 3 |
| IOP firmware / AP framework | AppleS5L8920X-257.21, kexts `AppleS5L8920XARM7M/IOPFMI/IOPSDIO` | AppleS5L8920X-283, **EmbeddedIOP-20.4** (`EmbeddedIOP`, `AppleARM7M`, `AppleIOPFMI`, `AppleIOPSDIO`, `IOSlaveProcessor`) | AppleS5L8920X-300.4 / 300.8, EmbeddedIOP-33.2 / 33.4 |
| IOP firmware image | v2 marker, `cnfg` @0x15018, bss 0x15160-0x22000 | same layout, bss to 0x24000 | `cnfg` @0x17000, bss 0x17180-0x25320; **config block layout changed** (below) |
| USB host gate | `hsic-enabled` DT + `enable-hsic=1` | **both gone**; `publish-criteria` per port node | same as 4.3 + `clock-mask`, `companion-id`, `disable-park-mode` |
| PMGR DT | `voltage-states`, `function-core_voltage_0/1` | `voltage-states0`, `#performance-domains`, `function-voltage_domain0`, `performance-domain-features` | + `enable-dvd`, `ema-settings0` |
| Watchdog | inside pmgr | inside pmgr | new `arm-io/wdt` node, `wdt-version 1`, reg 0xbf102020 (the PMGR watchdog the model has) |
| `compatible` strings | `*,s5l8930x` | same | `*-1,samsung` for uart/spi/i2s/pke/swi/mipi-dsim (kexts renamed `AppleSamsung*`) |
| NAND DT | `*-clks`, PPN props | + `ppn-ftl-ver`, `ppn-spec-version`; `dt-has-info` gone | + `ce-bitmap`, `toggle-device`, `adl/whr/ce-setup/ce-hold/read-pre/read-post/write-pre/write-post-clks`, `use-4k-aes-chain 1`, `retire-on-invalid-refresh`; `reg` gone |
| Partitioning | MBR (`IOFDiskPartitionScheme`) | MBR | `defaults/use-lwvm` + `LightweightVolumeManager` kext (MBR scheme still present; fstab still `/dev/disk0s1`, `/dev/disk0s2`) |
| Data protection | effaceable in NOR, `systembag.kb` (keybag one-shot) | same | same mechanism (`restored_external` still does `format_effaceable_storage` + `MKBKeyBagCreateSystem`; rc.boot is now a Mach-O that execs the same four names, so `it_keybag` still slots in); `defaults/content-protect`, `chosen/root-ticket-hash` (APTicket), `dram-vendor` |
| Wi-Fi | `AppleBCMWLAN` 2.60, firmware in the kext, `ver` 4.218 | same driver | `AppleBCMWLANCore` + `AppleBCMWLANBusInterfaceSDIO`, `IO80211Family_Embedded`; firmware from `/usr/share/firmware/wifi/4329b1/duo.bin` via `wifiFirmwareLoader`; driver strings 4.221.54.22 / 4.221.90.5 |
| GL | GLEngine in the cache, 841 dispatch slots, gld plugin via libGFXShared | not read | GLEngine + IMGSGX535GLDriver in the cache (bundle dirs empty on disk), `IOAcceleratorFamily`; **905 slots**, the first 772 identical to 8C148 (`glitsv.py` derives the table; a `GLEngine-9B206` would need a build); dyld override switch present; `MISValidateSignature` in the cache (AppSync recipe applies) |
| Multitouch | Zephyr2 firmware in the kext | not read | `/usr/share/firmware/multitouch/iPad.mtprops`; DT `function-enable_download` unchanged |
| Boot-args still read by the kernel | `cs_enforcement_disable`, `amfi_allow_any_signature`, `amfi_get_out_of_my_way`, `debug-enabled`, `nand-enable-reformat` | same minus `enable-hsic` | same |
| iBoot32Patcher | ok | ok (RSA + ECID/BORD/PROD/SEPO patches) | ok (`--rsa` patches `verify_shsh`, `--debug`, `-b`) |

The IOP config block, live on the kboot boot (`xp` of `IOP_FW_BASE`+0x17000):

```
v2 (iBoot-931 fw, what s5l8930_iop.c parses):  'cnfg' ? msgbuf  ring[h].{addr,count} from +0x0c ...
v3 (EmbeddedIOP-20/33 fw):                      'cnfg' 1 msgbuf 0 ring[h].{addr,count} from +0x10  0x3000  guid ...
```
so the HLE reads every ring's address as its count ("ring 3 has 3239116800 entries") and never
answers the control ring's startup ping. The 8L1 firmware (EmbeddedIOP-20.4) fails the same way, so
this is a 4.3 change, not a 5.0 one.

## Predicted confrontations (before booting) and what happened

| # | Component | Class | Predicted | Actual |
|---|---|---|---|---|
| 1 | kboot boot_args (`imgtools/ipad1_kboot.py`) | P | version field per iBoot generation | **Hit first** on 8L1 and 9B206: `pe_identify_machine: Epoch Mismatch` (Version 3 wanted). Fixed generically: `boot_args_version()` reads the demanded value off the kernel's own check (2 for 7B500/8C148, 3 for 8L1/9A405/9B206). |
| 2 | IOP HLE (`hw/arm/s5l8930_iop.c`) | H | new mailbox ABI with EmbeddedIOP-33 | **Hit** on 8L1 and 9B206: `IOP::_sendControlMessageGated: control message timeout` → `panic "IOP: startup ping failed"` (EmbeddedIOP-20.4 line 221 / 33.4 line 210). Cause: config-block layout (above). Not fixed: a v3 table keeps the class at H. |
| 3 | Real iBoot chain, `iboot=` path (`ipad1_iboot.py`, PMGR `POWER_ID`) | P + S | NOR/NVRAM format, PMGR values | **Hit**: iBoot-1219 panics `miu_init: Epoch Mismatch` in a reset loop (15 resets/2 s; the console is not on the UART yet, so serial stays empty). It compares `POWER_ID[31:24]` (the model's fixed 0x01020001, epoch 1 as captured under iBoot-817) with its own epoch 2 (SEPO 2 from 4.3.5 on). On hardware LLB writes that byte; the `iboot=` path skips LLB. iBoot-1072 (4.3.5) carries SEPO 2 too. |
| 3b | same, diagnostic with epoch 2 (temporary, reverted) | S/H | – | iBoot-1219 then runs `platform_init`, writes an unmodelled I2C register (+0x14, 525×), and ends in the PMU power-off/reset sequence (D1815 reg 0xe9 read, 0xe0 ← \|1, \|3) and `b .`: an early "power off" decision the D1815 stand-in does not act on. Not diagnosed further. |
| 4 | SecureROM chain, `bootrom=` + `development-fuses=on`, stock NOR images | R | signature rejection of unpersonalised 5.x images | **Partly better than predicted**: the ROM accepts and runs LLB-1219 from SRAM; LLB does the same I2C +0x14 writes, touches unmodelled blocks at 0xbfc00000/0xbfe00000 (miu/DMC-side) and 0x89e0xxxx/0x89f0xxxx, then takes the same PMU 0xe9/0xe0 power-off path and spins at 0x84001984. |
| 5 | `contrib/guest-package/mkpkg.py` FAMILIES by build | P | no family for 9B206 | **Hit**: bake aborted "0 packages for build 9B206". Fixed generically: no family → stock volume, lock records `family: null`. |
| 6 | `imgtools/ipad1_rootfs.py bake` opening `GLEngine` | P | – | **Hit** (unpredicted): with `--no-ca-ogl` and a firmware whose GLEngine lives only in the cache there is no file; bake now treats that as "no shim". |
| 7 | GLI dispatch table per build | P | new layout | Confirmed: 905 vs 841 slots, derivable; a per-build shim build remains (ledger proposal: parse the @encode at load). |
| 8 | USB host `enable-hsic` / DT `hsic-enabled` | P | ignored from 4.3 | Confirmed: 5.1.1 logs `AppleS5L8930XUSBPhy::start: hsic disabled` yet `AppleSynopsysUSBEHCI ... HSIC ports enabled` (the `publish-criteria`/`hsic-ports` DT does it). The two kboot edits are dead on 4.3+. |
| 9 | PMGR (S) new DT props | S | performance controller reads new props | Passed: `AppleARMPerformanceController configured with 1 Performance Domains` on 5.1.1; DVFS enabled. |
| 10 | Watchdog `wdt` node | S | new driver | Passed at start: `AppleS5L8930XWatchDogTimer::start: _wdtBaseAddress: 0xc5a6d020 _wdtResetCount=0xe4e1c00`; the PMGR watchdog never counts, so a real timeout would not fire. |
| 11 | Display (CLCD/DisplayPipe/Pinot) | H/S | new driver | Passed at start on the kboot path: `AppleCLCD::start_hardware`, `ApplePinotLCD: _lcdPanelID: 0x00a1d13c`, `DisplayPipe fRegisters ...`. |
| 12 | Wi-Fi (`AppleBCMWLANCore`) | H | new driver, firmware from a file | Not reached (needs the IOP SDIO ring). |
| 13 | NAND / LwVM / data protection | H (IOP store) + P (offline FTL, keybag one-shot) | LwVM | Not reached (needs the IOP FMI ring). The keybag one-shot's kboot boot fails on 1 and 2 first. |
| 14 | `[PKE] Montgomery operation failed` ×3 | R | – | Seen on every 8L1/9B206 kernel boot before the IOP panic; unexplained, did not stop the boot. |

How far each path got (serial evidence in `~/Developer/qemu-ios-files/ios5-spike/boot-*/serial.log`
at the time of writing; scratch):

- kboot 9B206 and 8L1: SecureROM/iBoot skipped → kernel → AMFI boot-args honoured → platform, PMGR
  performance domains, IOSDIOFamily-34/AppleIOPSDIO-12, watchdog, I2C/PMU (`AppleD1815PMU`), SPI,
  UARTs, USB PHY + arbitrator, ARM7M start + firmware upload, CLCD/Pinot/DisplayPipe up, "Waiting for
  root device", PMU power source, EHCI HSIC ports → **IOP ping timeout → panic** (~2400 serial lines,
  about 25 s).
- `iboot=` 9B206: iBoot-1219 → `main` → "iBoot start" (debug console only) → platform_init →
  **`miu_init: Epoch Mismatch` → watchdog reset**, looping.
- `bootrom=` 9B206: SecureROM → LLB-1219 accepted and running → PMU power-off path → spin.

## What 4.3.x already needs

From the 8L1 static diff and its kboot boot, in the order the matrix run will meet them:

1. boot_args Version 3 on the kboot path (done here, generic).
2. The IOP config-block v3 layout and whatever EmbeddedIOP-20's control ping expects
   (`s5l8930_iop.c`): blocks both the keybag one-shot (kboot) and every boot. Same fix serves 5.x.
3. Security epoch 2: iBoot-1072 will check `POWER_ID` like iBoot-1219 does (SEPO 2 on all three
   4.3.5 images). Either the `iboot=` path takes the epoch from the image (a machine property the
   builder fills from the IMG3 SEPO tag, generic) or the chain starts at the ROM so LLB writes it.
4. `enable-hsic` and `hsic-enabled` are no-ops; the USB keyboard depends on `publish-criteria` (DT,
   stock) and `hsic-ports`; nothing to add, but the two kboot edits and `usb-kbd,max-power` should be
   re-verified.
5. PMGR: new `voltage-states0`/performance-domain props are read fine (S survives); `enable-dvd`
   (5.x) untested.
6. Not yet known (blocked behind 2): NAND/FTL on 4.3 (same YaFTL, `ppn-*` props added), Wi-Fi driver
   deltas, GL dispatch layout for 8L1 (not derived; the rootfs was not decrypted).

## Verdicts for the ledger

| Blocker | Class | Fix that raises the class | Special case instead (not done) |
|---|---|---|---|
| IOP mailbox ABI (v1 → v2 → v3 config block, EmbeddedIOP-20/33 ping) | H | Run the ARM7 firmware on a second core (ledger K48 #33; 20-30 d). | A `cnfg` version switch (+0x4) and ring table at +0x10, plus the new control op: 0.5-1 d, stays H, and the next EmbeddedIOP will do it again. |
| `POWER_ID` epoch byte (`s5l8930_pmgr.c`) on the `iboot=` path | S + P | Boot from the ROM (`bootrom=`), where LLB sets it (R); the ROM already accepts LLB-1219 under development fuses. | Machine property `security-epoch` filled from the image's SEPO by `ipad1_iboot.py`: generic, 0.5 d, but the `iboot=` path stays P. |
| D1815 PMU power-off/reset registers (0xe0/0xe9) not acted on | H | PMU power sequencing (ledger K48 #15; 5-10 d). | – |
| I2C controller register +0x14 (unmodelled, written by LLB/iBoot-1219) | R (gap) | Add the register from the S5L8930 I2C block (0.5 d); harmless until proven otherwise. | – |
| Unmodelled blocks 0xbfc00000 / 0xbfe00000 (LLB-1219 miu path), 0x89e00000 / 0x89f00000 | S | Identify (DT `dmc`/`dart`?) and model; 1-3 d. | – |
| kboot boot_args version | P | (kboot is the debug path; the R path builds boot_args itself) | done generically: read off the kernel |
| guest-package family by build, bake's GLEngine assumption | P | key on (board, major) per the sweep | done: no family → stock volume |
| GLI dispatch per build | P | parse the @encode at load (2-3 d) | a `GLEngine-9B206` build |

## The IOP v3 instrument (branch `iop-v3`, 2026-09-28)

Class H, to be deleted when the IOP core lands. What EmbeddedIOP-20 (iOS 4.3) and -33 (iOS 5) changed
in the mailbox, read off the 8L1 firmware and live memory (`s5l8930_iop.c`, `s5l8930_sdio.c`,
`s5l8930.h`):

| Item | v2 (iBoot-931 fw) | v3 (EmbeddedIOP-20+ fw) | Evidence |
|---|---|---|---|
| `cnfg` block | `+0x4` flags, `+0x8` msgbuf, ring table `{addr,count}` from `+0xc` | `+0x4` flags, `+0x8` msgbuf, `+0xc` 0, table from `+0x10` | live blocks on 8C148 vs 8L1/9B206; fw 0x954 (`add r0, r4, #0x18; ldm {addr,count}` = ring 1) |
| addresses handed to the IOP | physical | the IOP's DRAM window `0xc0000000 + (pa - 0x40000000)` (rings, messages, FMI CE arrays, SDIO segments) | ring 0 item `0xc1282000` found at phys `0x41282000` in a DRAM dump; the block's own map `{0xc0000000, 0x40000000, 0x40000000}` |
| ring entry | 16 bytes (`lsl #4`, cache-clean 0x10) | **64 bytes** (`lsl #6`, cache-clean 0x40); owner bit and rx index unchanged | 8L1 fw 0xee4 vs 8C148 fw 0xd78 |
| FMI command arguments | from `+0x14` | from `+0x18` (one more word); outputs and erase arrays move with it | rejected set_config hexdump: page bytes at `+0x2c`, spare at `+0x30` |
| control opcodes | nop/rsum/spnd/ttin/slep | same literals | fw literal scan |

`iop_doorbell` probes the table offset per doorbell (`+0xc == 0 && +0x10 != 0` → v3, `fmi_arg = 8`,
64-byte entries); `s5l8930_iop_pa()` folds the window; SDIO scatter-gather entries are folded too. On
a rejected set_config the argument words are hexdumped (`-d guest_errors`).

Where the two builds stop now (kboot, golden 7B500 store, blank NOR):

| Build | Got to | Next blocker | Class |
|---|---|---|---|
| 4.3.5 (8L1) | ping, set_config x2, reset, chip IDs on 8 CEs, 280 read-multiple ops, `VFL Init [OK]`, SDIO enumeration, then `AppleNANDLegacyFTL::_FIL_static_Notify: epoch roll wait` and `Still waiting for root device` | the FTL waits for a NAND "epoch" notification (IOFlashStorage-410.4 line 2229: "epoch wait failure"); the store/NOR here carry no epoch state (blank effaceable: `[effaceable:ERR] unable to find content`). Likely device-preparation state (the keybag one-shot + a sealed 8L1 store) rather than the IOP; to be seen on a prepared 8L1 device | P (pipeline state) / H (effaceable NOR) |
| 4.3.5 | Wi-Fi: `AppleBCMWLAN-84 ... no successful firmware download after 60000 ms` | the card model's ready handshake does not satisfy the 4.3 driver | H (K48 #32) |
| 5.1.1 (9B206) | ping answered, SDIO enumerates the 4329 (`AppleBCMWLANCore` starts), `AppleIOPFMI started` + timings, then no FMI command ever, `Still waiting for root device` | AppleIOPFMI-49 gates on an IOP **event** (`_iopEvent`, "SetActive received when already active"): EmbeddedIOP-33 delivers endpoint activation through the IOP->AP message ring (ring 1, the fw's `iop message` endpoint, `notifyEndpointEnabled`), which this HLE never produces. The message format is the firmware's; not attempted here | H (K48 #33) |

## Changes on this branch

- `imgtools/ipad1_kboot.py`: `boot_args_version()`; boot_args.Version read off the kernel (2 or 3).
- `contrib/guest-package/mkpkg.py`: `seed()` returns an empty seed when no family covers the build.
- `imgtools/ipad1_rootfs.py`: bake tolerates a cache-only GLEngine with no shim; summary line.
- `manifests/ipad1-9B206.json`.
- This document.
- `iop-v3`: `hw/arm/s5l8930_iop.c`, `hw/arm/s5l8930_sdio.c`, `include/hw/arm/s5l8930.h` (the v3 instrument above).

Selfchecks green: `ipad1_kboot.py`, `ipad1_rootfs.py --selfcheck`, `mkpkg.py selfcheck`,
`tests/guest-package/test_it_boot.py`. The 7B500 and 8C148 kernels still get Version 2 (checked by
`boot_args_version` on their kernelcaches); no prepared device was rebuilt for them here.

Scratch (not committed): `~/Developer/qemu-ios-files/ios5-spike/` holds the IPSWs, rendered key
files, decrypted components, `dt-*.diff`, `gli-dispatch-9B206.tsv`, the extracted IOP firmware
images, and the small drivers used here (`boot9.py` headless boot + QMP register/memory dump,
`kcdis.py`/`ibdis.py` capstone disassembly with literal resolution, `dtdiff.py`/`dtnode.py`,
`kcinfo.py`, `iopfw.py`). Boot clones and the 9B206 device build were deleted.
