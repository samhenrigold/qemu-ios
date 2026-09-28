# iPad 1 / iOS 4.2.1 (8C148): the declared-inputs pipeline across a major version

Probe of 2026-09-27: `manifests/ipad1-8C148.json` (IPSW sha1 `8717b3bedc925b587566442ad375aa65d857e79a`, keys
rendered from `api.ipsw.me/v4/keys/ipsw/iPad1,1/8C148` in the 7B367 keys-file format) through
`imgtools/ipad1_device.py create`. There was no iPad 4.0; 4.2.1 is the first 4.x.

**Reached M2**: the stock 4.2.1 kernel boots on the ipad1 machine, the FTL opens the pipeline's NAND store,
`BSD root: disk0s1`, launchd starts, fsck passes and both volumes mount (`/dev/disk0s2 on /private/var (hfs,
local, nodev, nosuid, journaled)`, the plain 0xAF data partition). **Blocked before M3** by data protection:
`keybagd` finds no system keybag, logs `FATAL KEYBAG ERROR: kb_load` / `REBOOTING INTO RESTORE MODE` and
reboots. With keybagd disabled the boot stays up, but SpringBoard never draws (details below).

Components: kernel `xnu-1504.58.28~3/RELEASE_ARM_S5L8930X` (Darwin 10.4.0), iBoot and IOP firmware
`iBoot-931.71.16`, AppleS5L8920X-257.21, rootfs `038-0018-002.dmg` (HFSX, 101511 x 8 KiB = 831 MB; fits the
1280 MiB system partition).

## What differed, and what the pipeline does now

| per-build value | 3.2 / 3.2.2 | 4.2.1 | now derived from |
|---|---|---|---|
| component names, keys | | `038-0018-002.dmg` rootfs, `038-0032/0024-002` ramdisks | BuildManifest.plist + keys page (unchanged `ipad1_fw.py`) |
| kernel link base (boot_args virtBase, all VA->PA) | 0xC0000000 | **0x80000000** | the kernelcache's lowest segment, `& 0xF0000000` (`ipad1_kboot.build`) |
| DT `flash-controller0/disk` | `*-ns` timings | `*-clks` timings, `meta-per-logical-page`, `valid-meta-per-logical-page`, `logical-page-size`, `ppn-device` + PPN geometry | kboot sets only the props the IPSW DT has; the 4.x meta layout (12 total / 10 DMA bytes, 4096) is a property of this raw NAND, declared next to the other NAND values. PPN props stay 0 (iBoot-931 fills them only when `ppn-device` = 1, iBoot 0x5ff077fc). `*-clks` stay 0: the IOP model ignores timings (the kernel prints `tRP 41ns ...`) |
| IOP firmware `cnfg` block / bss | 0xf018 / 0xf160-0x1b000 | 0x15018 / 0x15160-0x22000 | `s5l8930_iop.c`: bss bounds from the image header words fw[0x318]/[0x31c], `cnfg` by scanning the loaded image for its magic |
| IOPFMI command ABI | v1 | **v2** (below) | `s5l8930_iop.c` picks v2 when the loaded image carries `h2fmi_iop_read_chip_ids` |
| NAND geometry / FTL | 3,925,449 sectors | the same; NANDDRIVERSIGN `0x43313131` read as "new style signature", metadata whitening honoured | unchanged `ipad1_nand.py` store; 4.x runs its own full R/O restore on it (and erases free blocks while doing so: ~1800 erase-multiple commands per bus) |
| data partition | plain 0xAF + fstab patch | the IPSW fstab already says `/dev/disk0s2`; the unprotected HFSX volume mounts journaled | unchanged. No EncryptedMediaFilter / content-protection mount is needed to mount it |
| launchd jobs used by rootfs/bake | | SpringBoard, BTServer, storage_mounter, lockdownd, installd: same paths | unchanged |
| AppSync `MISValidateSignature` | | found by symbol in the 4.2.1 shared cache (VA 0x3075d924) | unchanged (`appsync_cachepatch`) |
| GLI dispatch ABI | 826 slots | 841 slots | `gli_abi_problem` refuses GL CA, as designed; the manifest says `ca_ogl: false` |

Checked equal rather than derived: the NAND chip ID path, `NANDDRIVERSIGN`, whitening, the MBR, the
unimplemented-register profile (DART1 at 0x88d00000 is polled on 3.2.2 too). Guest helpers built against
the 3.2 SDK run on 4.2.1 (`it_ethlink: up`, `it_prefs ... reloaded`, ldid signatures accepted with the AMFI
boot-args).

### FMI v2 (EmbeddedIOP iBoot-931)

From the firmware's FMI task (8C148 fw 0x1020, jump table 0x1090, opcodes 1-23; `[fmi+0x10]` = is_ppn
selects the PPN handler per opcode). Raw NAND uses 1-12 (same numbering as v1), 20 and 21.

- Every argument moves up one word: v1 +0x10 is v2 +0x14 (set_config: bus +0x14, #ce +0x18, CE mask +0x1c,
  pages/block +0x20, page bytes +0x28, spare +0x2c, blocks/CE +0x30, timings +0x34..+0x44, is_ppn +0x48,
  valid meta +0x50, total meta +0x54; fw 0x283c).
- CE numbers are u16 (`ldrh`), in single-page commands and in the multi-page CE arrays (fw 0x4444).
- Op 2 only resets; op 20 `h2fmi_iop_read_chip_ids` returns the 16 x 5-byte ID table at the address in +0x14
  (fw 0x2774).
- Multi-page outputs: +0x60 pages done, +0x64 status, +0x6c failing (v1: +0x5c, +0x60, +0x70/+0x74).
- Erase multiple (fw 0x26c4): +0x14 count, +0x18 16 u16 CEs, +0x38 16 u32 blocks; out +0x78 done, +0x84
  failing; status 1 / 0x80000001. No per-operation status ring (v1 had one at +0xa0).
- Meta DMA stays 10 bytes per page (`valid`); YaFTL's struct is 12 (`total`).

Not modelled: op 21 (fw 0x966c, never sent on this boot), the PPN handlers, and the failing-CE details on
error paths (the model never fails a read).

## The blocker: system keybag and effaceable storage

Evidence, in boot order (serial, the stock `keybagd`, and crash reports forced with SIGABRT from a scratch
diagnostic job):

1. `AppleEffaceableStorage::findCurrentClone(): unable to find content`: the NOR's effaceable region
   (spi0 `nor-flash/effaceable`, new in the 4.2.1 DT) is blank.
2. `keybagd` loads `/private/var/keybags/systembag.kb`, finds none: `FATAL KEYBAG ERROR: kb_load`,
   `REBOOTING INTO RESTORE MODE.` launchd shuts down and reboots within a minute, before `it_seal` fires at
   40 s, so `ipad1_seal.py` never gets its clean halt and the device cannot be sealed.
3. keybagd `Disabled`: the boot stays up but SpringBoard only shows the Apple logo. SpringBoard's main thread
   is blocked early in its own `main` (before UIKit); DataMigrator is stuck in AccountMigrator ->
   MobileSync -> liblockdown, i.e. waiting on lockdownd; lockdownd's main thread is blocked inside
   Security.framework (securityd round trip) after `data_ark_load` found no Lockdown dir. securityd needs
   AppleKeyStore class keys, which need the system keybag.
4. On a real restore `restored_external` formats effaceable storage (AppleEffaceableStorage user client:
   selector 3 isFormatted, 4 format, 7 efface) and calls `MKBKeyBagCreateSystem(NULL, <data mount>)`.
   Doing the same from a guest job on a normal boot: the format is refused
   (`AppleEffaceableStorageUserClient::externalMethod(): format attempt from untrusted root`), so
   `MKBKeyBagCreateSystem` fails (-1, `AppleEffaceableStorage::setBytesGated(): couldn't find the droid`)
   and leaves only `systembag.kb.new`. AppleKeyStore itself runs (`cp_key_store_action`), and the CDMA
   AES model already has a fixed stand-in UID key.

So an unprotected data volume *mounts*, but 4.x userland will not come up without a system keybag, and the
keybag needs formatted effaceable storage that persists across boots.

## Approach chosen: the IPSW restore ramdisk as the trusted root (2026-09-27)

Two routes reach a formatted effaceable region + a system keybag. Evaluated, and picked the first:

1. **Restore-ramdisk one-shot (chosen).** At `create` time, boot the IPSW's own restore ramdisk
   (`038-0024-002-ramdisk.dmg`, already decrypted; kernelcache + ramdisk from the Update identity of the
   BuildManifest) once against the device's writable NAND store and `nor-rw`, run the data-protection step
   restored/asr does (format effaceable, `MKBKeyBagCreateSystem`), then seal. This is literally what Apple's
   restore does; it matches Sam's rule ("take the IPSW and know what to do with it", no per-version crypto);
   the working code lives in the ramdisk, so nothing here reimplements key wrapping.

2. **Offline effaceable + keybag (rejected).** Write the `AppleEffaceableNOR` on-flash format (clones, locker
   headers, Adler) and a valid `/private/var/keybags/systembag.kb` offline. Rejected: the system keybag is a
   `BAG1` blob whose class keys are wrapped by a device secret held in an effaceable locker (EMF/Dkey),
   itself under the hardware UID key (0x835). Producing it offline means reimplementing MobileKeyBag +
   AppleKeyStore key wrapping *and* the effaceable locker format — real, fragile crypto that duplicates what
   the ramdisk already does, and, even "derived", is far from the IPSW-agnostic rule.

**The pivotal unknown for route 1** is the trusted-root gate. The refusal comes from
`AppleEffaceableStorageUserClient::externalMethod` (thumb `0x80335f7c` in this kernelcache): it computes one
root-trust byte via a virtual on the `AppleEffaceableStorage` provider (vtable slot `+0x1f8`) and selector 4
(format) branches to `"format attempt from untrusted root"` (string `0x80337a88`, xref `0x80336258`) when it
is false; `isFormatted` (selector 3) and the locker selectors gate on the same byte. On a normal disk boot it
is false — the previous agent's `it_kb` confirmed the refusal. On a real restore, iBoot boots the ramdisk in
restore mode and that state reaches the kernel; we boot direct-kernel (no iBoot), so the **first task of the
one-shot is to confirm empirically whether a restore-ramdisk boot presents a trusted root**, and if the trust
virtual reads a DT property / boot-arg (as restore mode sets), set it for that one boot only — legitimately,
it *is* a restore boot. If instead it requires the secure-boot img3 chain we bypass, reassess (that would push
route 1 toward the multi-day mark and reopen route 2). Next concrete step: disassemble the `+0x1f8` trust
virtual and identify what it reads.

The system keybag it creates lands on the data volume (`/private/var/keybags/systembag.kb`), i.e. inside the
NAND store, and the effaceable lockers land in NOR. So the one-shot must run against the device's *writable*
NAND store + `nor-rw` before the seal, and both outputs then travel with the device (`nand/` + `nor.bin`).

## What is done (2026-09-27, this session)

`nor-rw` on the ipad1 machine (`hw/arm/ipad1.c`): a private writable 1 MiB NOR copy whose guest writes (the
effaceable region) persist across boots, mirroring the iPod machine's option. `ipod_touch_nor_spi_open_overlay`
(shared with the iPod) was relaxed to allow a standalone writable NOR with no read-only `nor=` base, since the
iPad ships a blank effaceable NOR. Threaded a per-device `nor.bin` through `ipad1_device.py` (created blank and
sealed in when the manifest sets `options.writable_nor`; recorded in `device.lock.json`), `ipad1_seal.py`
(`--nor-rw`, so the sealing boot's effaceable writes persist), `boot-smoke.py` (`--nor-rw`) and
`fresh-device.sh` (boots both times on one private writable copy, so the effaceable/keybag survives the
power-off → reboot). 3.x manifests do not set `writable_nor`, so their pipeline is byte-identical to before
(a blank writable NOR reads all-`0xff`, exactly like unset flash, and 3.x never touches effaceable-in-NOR).
The `8C148` manifest sets `writable_nor: true` in readiness; it still cannot boot to a lit screen until the
keybag one-shot exists. Gates green after the change: `fresh-device.sh` for 7B500 and 7B367 (both with the
activation hook), and the iPod regression — see the commit.

## What is left

| item | estimate |
|---|---|
| ~~NOR persistence on the ipad1 machine (`nor-rw`) and a per-device `nor.bin` through the pipeline~~ **DONE this session** (the app still needs to pass its own private writable `nor.bin` copy as `nor-rw=`; see below) | — |
| Restore-ramdisk one-shot: a kboot bundle booting `038-0024-002-ramdisk.dmg` as `rd=md0` with the restore DeviceTree, then the DP step (format effaceable + `MKBKeyBagCreateSystem`), run against the device's writable NAND + `nor-rw` before the seal. **Gated on** confirming the trust virtual (`+0x1f8`) permits the format from a ramdisk boot | 1-2 days (more if trust needs the img3 chain) |
| GLI shim for 4.2.1: 15 new dispatch slots (13 inserted after slot 764: `draw_elements_base_vertex` ... `sample_maski`, then `discard_framebuffer_EXT` and `resolve_multisample_framebuffer_APPLE`); a per-firmware TSV for `gligen.py` and a check of the new entry points | 0.5-1 day |
| Activation: the opt-in hook refused 4.2.1's lockdownd (it fails closed; exit 1, file unchanged) | the hook owner's call |
| Unlock / power-off coordinates on 4.x SpringBoard | unverified (no UI yet) |

Scratch artefacts (untracked): `~/Developer/qemu-ios-files/ipad1/repro-8C148/` (decrypted firmware,
extracted IOP images `iopfw-{7B500,8C148}.bin`, probe boots `p1`-`p7`, the diagnostic `diag/it_ps.c` and the
keybag experiment `diag/it_kb.c`).
