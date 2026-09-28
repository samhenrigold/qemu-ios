# iPad 1 / iOS 4.2.1 (8C148): the declared-inputs pipeline across a major version

Probe of 2026-09-27: `manifests/ipad1-8C148.json` (IPSW sha1 `8717b3bedc925b587566442ad375aa65d857e79a`, keys
rendered from `api.ipsw.me/v4/keys/ipsw/iPad1,1/8C148` in the 7B367 keys-file format) through
`imgtools/ipad1_device.py create`. There was no iPad 4.0; 4.2.1 is the first 4.x.

**Reached M4 (2026-09-28)**: `ipad1_device.py create manifests/ipad1-8C148.json` makes a device that boots
to a lit "Connect to iTunes" screen (SpringBoard, 12-19 s), powers off cleanly through the slider and boots
lit again on the same overlay with its keybag and effaceable NOR intact (`tests/ipad1/fresh-device.sh
manifests/ipad1-8C148.json`, no activation hook: 4.x activation is out of scope). Three layers had to go, in
order: the system keybag (the restore-ramdisk one-shot below), a kernel panic in the accelerometer
driver, and AppleCLCD refusing a panel iBoot had not programmed; plus the UI orientation. See "Past the
keybag". Before that the probe had reached M2 and was blocked by data protection (`keybagd`: `FATAL
KEYBAG ERROR: kb_load` / `REBOOTING INTO RESTORE MODE`).

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
| GLI dispatch ABI | 826 slots | 841 slots | a TSV per layout (`contrib/ipad1-gles/glitsv.py` derives it from the shared cache), one `GLEngine-<BUILD>` per TSV; `gli_engine` picks by the cache's `__GLIFunctionDispatchRec` @encode. See "GL CoreAnimation on 4.2.1" |

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

### The trusted-root gate: RESOLVED — it is DeviceTree state, not a signature check (2026-09-27)

The refusal comes from `AppleEffaceableStorageUserClient::externalMethod` (thumb `0x80335f7c`): it fetches a
trust byte and selector 4 (format) branches to `"format attempt from untrusted root"` (string `0x80337a88`)
when it is false; `isFormatted` (3) and the locker selectors gate on the same byte. Disassembled the whole
chain — **the byte comes from a DeviceTree property, not from any in-kernel img3/signature verification.**
Full evidence, decisive: the emulator can present this legitimately for a restore-ramdisk boot.

Trust chain, bottom-up:

- `externalMethod` 0x80335fa4-fd8: `r0 = this->provider [this+0x78]`; calls a provider virtual (vtable
  `+0x1d8`) to get an object X; loads `X.vtable[+0x1f8]` (the trust virtual) and calls it with `r1` = the
  literal string **`"SecureRoot"`** (`0x80337980`) and `r2` = `&trustByte` at `[sp+0x13]`. So the gate is
  simply "is the `SecureRoot` IOResource present?". `"root"` (`0x8033793c`) nearby is only the calling
  proc name in the log line, not the gate.
- `SecureRoot` is an **IOResource published by `com.apple.driver.AppleARMPlatform`** (owner-mapped via
  `__PRELINK_INFO`). Consumers that gate on it: `AppleEffaceableStorage`, `IOFlashStorage`,
  `IOStorageFamily`, `AppleKeyStore` — each carries its own `"SecureRoot"` string copy.
- AppleARMPlatform's boot-device init (0x802d7ac4) reads, via `IORegistryEntry::getProperty` (vtable
  `+0x9c`) on the boot node, the DeviceTree property **`secure-root-prefix`** (`0x802dfafc`), plus
  `no-rtc` / `no-suspend`. It stores the prefix and sets the trust-state bytes `[this+0x96..0x9c]`. The
  publisher (0x802d7820-0x802d79b4) publishes `SecureRoot` when the **booted root device's name matches
  that prefix** (again the `+0x1f8` virtual, same match callback).
- The IPSW's **restore DeviceTree already ships `secure-root-prefix = 'md'`** (top level; `DeviceTree.txt`
  line 6). `'md'` = the `md0` RAM-disk device (`md%d` in the kernel; the `RAMDisk` memory-map string is the
  consumer). On a normal boot root is `disk0s1` (NAND) — no `md` prefix match → `SecureRoot` unpublished →
  format refused. On a restore boot root is `md0` → match → `SecureRoot` published → format allowed.

**Decision: route 1 is viable and needs no kernel patching or signature forging.** To present a trusted
root for the one-shot we do exactly what iBoot does for a genuine restore: boot the restore kernelcache with
the **restore DeviceTree** (which carries `secure-root-prefix = 'md'`), load the raw-HFS restore ramdisk as
the `md0` RAM disk, and select it as root. That is faithful restore-boot state, not a bypass.

The restore recipe, straight from the ramdisk (`038-0024-002-ramdisk.dmg`, raw HFS+ `H+`@0x400, mounts
clean): `usr/local/bin/restored_update` links `MobileKeyBag.framework` and does, in order,
`format_effaceable_storage` (open `AppleEffaceableStorage` user client → `isFormatted` sel 3, else `format`
sel 4; strings `"effaceable storage formatted successfully"` / `"...is formatted, nothing to do"`) then
**`MKBKeyBagCreateSystem`** (stable export `_MKBKeyBagCreateSystem` @0x23a0 in
`System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag`). The guest one-shot helper calls the
same two stable symbols — no reimplemented crypto.

The system keybag it creates lands on the data volume (`/private/var/keybags/systembag.kb`), i.e. inside the
NAND store, and the effaceable lockers land in NOR. So the one-shot must run against the device's *writable*
NAND store + `nor-rw` before the seal, and both outputs then travel with the device (`nand/` + `nor.bin`).

## The one-shot, as built (2026-09-28)

`create` for a manifest with `options.writable_nor` (4.x) runs, between the NAND store and the seal:
`imgtools/ipad1_keybag.py nand/ nor.bin --dec DEC --ramdisk <Update identity RestoreRamDisk>`.

- **kboot RAM-disk mode** (`ipad1_kboot.py --ramdisk DMG`): the raw-HFS ramdisk sits in DRAM right after
  the kernel (below `topOfKernelData`, so the VM never reuses it), `chosen/memory-map` gets `RAMDisk (pa,
  len)`, boot-args gain `rd=md0`, and `chosen/root-matching` is left empty: xnu reads the `RAMDisk` entry
  only when root-matching does not match, then roots on `md0`. Kernelcache and DeviceTree are the stock
  ones: in this IPSW `RestoreKernelCache`/`RestoreDeviceTree` are the same files as the normal-boot ones,
  and that DeviceTree's `secure-root-prefix = 'md'` makes `md0` a SecureRoot while `disk0s1` never is. No
  kernel bytes are patched, no signatures forged; 3.x and normal boots take the unchanged path.
- **The ramdisk copy**: a private copy of `038-0024-002-ramdisk.dmg`, grown by 1 MiB (the stock one is
  100% full), gets `it_keybag` as `/usr/local/bin/restored_external`: the ramdisk's `/etc/rc.boot` execs
  the first of `restored_external`, `restored_update`, `restored`, `ramrod` that exists, so this is the
  place iBoot's restore boot hands control to the restore daemon.
- **`it_keybag`** (`contrib/it-keybag`, built and ldid-signed by `contrib/ipad1-guest/build.sh`, not baked
  into the rootfs): waits for `/dev/disk0s2` and `AppleEffaceableStorage`, selector 3 (isFormatted) then 4
  (format) then 3 again, `mount_hfs /dev/disk0s2 /mnt2` (as restored does), `MKBKeyBagCreateSystem(NULL,
  "/mnt2")` found with dlsym, checks `/mnt2/keybags/systembag.kb` (1335 bytes), unmounts and
  `reboot(RB_HALT | RB_QUICK)`. The ramdisk has no `IOKit.framework/IOKit` top-level link, only `Versions/A/IOKit`.
  RB_QUICK (2026-09-28): a plain RB_HALT SIGTERMs every process first, and once the
  ramdisk's launchd saw its job die, started its own `reboot(RB_AUTOBOOT)` alongside the halt, and the
  two shutdowns panicked ("ARM7M: timed out waiting for workloop to process completed command") instead
  of halting, so the one-shot waited out its 300 s. 50 runs with RB_QUICK (20 of them concurrent) halted
  cleanly; the race was never reproduced live, so `ipad1_keybag.py` also stops a boot at `panic(` and
  retries it (3 attempts) from a clone of the store and NOR, logging the reason.
  Serial: `it_keybag: effaceable open 0 isFormatted 0 -> 0`, `format 0`, `MKBKeyBagCreateSystem -> 0`, then
  `it_keybag: effaceable formatted, system keybag created; halting`, about 4 s after power-on.
- `ipad1_keybag.py` requires that line, a clean halt, and a changed `nor.bin` (the lockers: ~8 KiB of
  non-`ff`); the device lock records the ramdisk under `inputs.restore_ramdisk`. The normal sealing boot
  follows unchanged (`it_seal`, 54 s).

## Past the keybag (2026-09-28)

With the keybag in place keybagd, lockdownd and SpringBoard all run; three more things stood between
that and a lit screen.

1. **Accelerometer panic** as soon as userland enabled the sensor: `AppleLIS331DLH::enableAccelerometer -
   Boot bit did not return to zero in 500 msecs`. The LIS331DLH keeps BOOT in CTRL_REG2 bit 7 (the
   LIS302DL's is bit 6, which is all the shared model cleared). `ipod_touch_lis302dl.c` now clears the bit
   the part's WHO_AM_I says (0x32: bit 7). 3.2.2's driver never polls it.
2. **No LCD framebuffer**: `AppleCLCD::start_hardware` stopped after the Pinot line and never logged
   `Added framebuffer device: AppleCLCD` (only AppleRGBOUT did), so CoreAnimation had no main display and
   the Apple logo stayed up. The 4.x driver (8C148 `0x809d7e42`) reads CLCD `+0x58` and silently returns
   false unless both its fields are nonzero: the vertical timing iBoot programs. iBoot-931 fills CLCD
   `0x00/0x04/0x14/0x18/0x54-0x60` from its `display-timing` table entry `k48` (`0x5ff2b028`: 1024x768,
   68.4 MHz, h 133/133/135, v 10/10/12, 18 bpp; code `0x5ff01e0e-0x5ff01eaa`). The display model's reset
   now leaves those values, as it already did the pipe registers iBoot leaves.
3. **UI orientation**: with CLCD up, 4.2.1 drew a *landscape* UI for the attitude 3.2.2 (checked on the
   real unit) reads as portrait, a quarter turn off for every accel-orientation, so the power-off knob was
   not where the gesture aimed. 4.x lays the UI out by the chosen `display-rotation` (iBoot:
   its video rotation byte x 90, iBoot-931 `0x5ff0ffda`; MobileGestalt `main-screen-orientation`); 3.2.2
   ignores it. Tried all: 0 is a quarter off, 90 upside down, 270 matches 3.2.2 in all four orientations,
   so kboot now sets 270 and the existing unlock/power-off coordinates hold for 4.x. 3.x gates unchanged.
   Open: kboot's own boot logo is turned the opposite way (top at the panel's right, the UI's top is at
   its left); cosmetic, not changed.

Measured on a fresh device (manifest defaults, no hook): lit 12.5-18.7 s after power-on, slider power-off
17.7 s to QEMU exit 0, boot 2 without an FTL rescan.

## GL CoreAnimation on 4.2.1 (2026-09-28)

`manifests/ipad1-8C148.json` now says `ca_ogl: true`: SpringBoard composites through the GLI shim and
host GL, and the "Connect to iTunes" screen is drawn that way (lit 12.7 / 15.6 s on a fresh device,
clean power-offs). What 4.x added, and what the shim does about it:

**The blocker, and the plugin contract.** 4.x EAGL makes a sharegroup only through libGFXShared:
`-[EAGLSharegroup loadGLIPlugin:]` (for each pixel format with the accelerated bit 0x100) calls
`gfxCreateSharedState(&pf->renderer, 1)`, which looks up a plugin by `renderer & 0xffff00` and a device
by `renderer & ~0xff` and calls the plugin's `gldCreateShared(&slot, device mask, 4)`; no plugin, no
context. 3.2.2's libGFXShared has the same API and discovery, but its EAGL never calls it (only the stock
GLEngine did), so replacing GLEngine sufficed there. libGFXShared (read from the 8C148 cache,
`_gfxPluginConnectAll`) registers plugins two ways, both by name, nothing hand-found:
- IOKit: for each service EAGL passes to `gliInitializeLibrary` (it matches `IOAcceleratorES`), the
  service's `IOGLESBundleName` names `/System/Library/Extensions/<name>.bundle/<name>`. That is how the
  real plugin, `IMGSGX535GLDriver.bundle` (in the shared cache; `gldGetVersion` 3.1.0, renderer 0x7000),
  is found. It opens the SGX IOAccelerator's user client in `gldInitializeLibrary`, i.e. it needs the SGX
  kernel driver and hardware.
- Resources: when the IOSurface callbacks are set (EAGL passes one, the engine the other), every
  `GLRendererFloat*` entry of `$GL_RESOURCES` or OpenGLES.framework's resources directory is loaded as
  `<dir>/<name>.bundle/<name>` and gets one device (Apple's software-float slot on the Mac).
Per plugin: `dlopen`, `gldInitializeLibrary(svcs, 0, mask, flush, bind, init)`, then `gldGetVersion`
must return nonzero with 3, 1, 0 and a renderer with only bits 8-15 set; the plugin ID is that `| 0x20000`
and its first device `| 1 << 24`; then all 79 `gld*` names in libGFXShared's table must `dlsym`, or the
plugin is dropped. The stock GLEngine calls `gfxInitializeLibrary` + `gfxPluginConnectAll` from its
`gliInitializeLibrary` and takes pixel formats from each plugin's `gldChoosePixelFormat`.

**Design: a gld plugin shim on the resources path (chosen).** `contrib/ipad1-gles/gldshim.c`, installed
as `OpenGLES.framework/GLRendererFloatQEMU.bundle/GLRendererFloatQEMU` by `ipad1_rootfs.py build` when
the firmware's OpenGLES imports `gfxCreateSharedState`: version 3.1.0, the SGX's renderer 0x7000, a
`gldCreateShared` that allocates a token, and stubs for the rest (nothing but the shared-state
bookkeeping calls them: the GL itself stays in glishim). The shim's `gliInitializeLibrary` does what the
stock one does (`gfxInitializeLibrary` with EAGL's arguments, then `gfxPluginConnectAll`), checks
registration with `gfxGetPluginWithDriverID`/`gfxGetDeviceWithDeviceID`, and its pixel formats carry
device 0x01027000 and the accelerated bit. All found by symbol; libGFXShared's 79 names are read from the
firmware at build time and the build fails if gldshim lacks one. Fails closed: 3.2.x never loads
libGFXShared (the lookups fail, nothing changes); a 4.x with no registered device logs why and leaves
pixel formats unaccelerated (no GL context, CA in software). Rejected: an emulated SGX IOAccelerator for
the real plugin. Its interface is the SGX user client and command streams, far larger than 6 functions.

**Loading the shim over a cached GLEngine.** 4.2.1 ships GLEngine in the shared cache and iOS dyld matches
cached images by path only (`findInSharedCacheImage`, 0x2fe01604), so a file at that path is never read.
dyld has Apple's switch for this: when `/System/Library/Caches/com.apple.dyld/enable-dylibs-to-override-cache`
exists (checked once at launch into `sDylibsOverrideCache`), `loadPhase5` tries the file on disk before
the cache. The build creates that empty file when the cache holds GLEngine, and fails if this dyld lacks
the string. This replaced the earlier one-byte edit of the cached path. No environment setting reaches it
(OpenGLES builds the path from the `com.apple.opengles` bundle; `DYLD_SHARED_REGION=avoid` would drop the
whole cache, and 4.2.1 has no standalone dylibs), and a `DYLD_INSERT_LIBRARIES` interposer on `dlopen`
would have to be in every GL process's environment. Every other cached image has no file on disk, so it
still comes from the cache.

**What else changed in 4.x EAGL**, handled in glishim (3.2.x paths unchanged, keyed on libGFXShared being
loaded):
- `gliCreateContextWithShared` (all contexts of an EAGL sharegroup; the pixel format EAGL embeds keys
  the group).
- `-renderbufferStorage:fromDrawable:` now only calls `gliBindViewES`, and returns its result. The stock
  engine binds CA's drawable itself: `drawable->bind(fourcc, block)` with a 4-entry block (create,
  destroy and a new `preflight`), the first `nextBuffer`, then its own `gliSetInteger(0x38E)` attach as
  GL_RENDERBUFFER. glishim does the same (`gli_bind_view4`).
- A buffer CA has just allocated may have no pages mapped (`present-surface: write failed at row 0`
  before the host faulted pages in): the shim touches each page of the frame's buffer before presenting,
  which now only saves the host a fault round trip per page.
- CA puts an EAGL layer's surface in its own IOMFB layer (UI0) under a full-screen UI1, with a
  destination rectangle: +0x54 origin, +0x60 source size, +0x64 far corner (`x << 16 | y`). The display
  model (`s5l8930_display.c`) assumed full-panel layers at 0,0 and now honours the rectangle.

**The GL check without activation.** Apps can't be launched from an unactivated home screen, so
`contrib/it-gltest/it_gltest.c` is a launchd job (`ipad1_device.py create ... --gl-test`, recorded as
`gl_test` in the lock) that puts a CAEAGLLayer on its own remote CAContext, ordered above everything,
and draws a magenta / cyan / yellow ES 1.1 scene with a moving blue band. `tests/ipad1/gltest.py DEVICE`
checks the fixture's glReadPixels probes, the colour fractions of two screendumps (the iPod GLES check's
method), the fixture's present rate and tearcheck's score. On 8C148: readback PASS, magenta 0.071 /
cyan 0.153 / yellow 0.076 (layer 0.076 / 0.153 / 0.076), presents 61.5 fps (vsync), 0 torn, black or
partial frames in 92 captured changes. On 7B500 it passes too, but only its first frame reaches the
panel (293 presents/s, one distinct capture): 3.2.x CA does not update a remote context's EAGL layer
this way; GLTest.app is the 3.x fixture.

Fps and tearing of SpringBoard's own animations on 4.x: see "End to end, activated" below.

**Untouched guest pages (2026-09-28).** A client array, index list or output pointer in a page the guest
never touched (a static const table in `__TEXT`, untouched `__DATA`/`__bss`, fresh heap) used to fail the
host's debug access: the draw dropped its geometry (`failed to read ... array data`, `cannot read N index
bytes`) and a glGen* name written there was lost. The host now probes each page through the caller's
MMU and, if one would fault, raises that fault as a data abort on the trapping mcr, so the kernel pages it
in and the call is reissued (`gles_guest_rw` in `hw/arm/guest-gles.c`); stores probe for write access, so
copy-on-write and modified-bit tracking see them too. The fixture now draws from static tables alone in
their pages and writes a texture name into an untouched `__bss` page: readback PASS on 8C148 and 7B500
(before: magenta where the cyan quad goes, name 0).

## End to end, activated (2026-09-28)

With the user's 4.2.1 activation tool (`create --activation-hook`, a black box here) the whole list was
run against fresh 8C148 devices from `manifests/ipad1-8C148.json`, GL CoreAnimation on. Scratch outputs:
`~/Developer/qemu-ios-files/ipad1/repro-8C148/` (`acc3`, `dev3`, `ui`, `rb1`, `set1`, `perf`, `apps`).

| item | result |
|---|---|
| acceptance (`fresh-device.sh ... -- --activation-hook`) | PASS: both boots lit (the wallpapered lock screen, ~148,700 lit samples against boot-smoke's 20,000 threshold, which stands), unlocked with one drag, home screen, power-off to QEMU exit 0 in 15.6 / 16.5 s, no FTL rescan; boot 2 activated on the same overlay, so the keybag held. GL CA live: `[glishim] gld plugin registered, device 0x1027000`, `gliCreateContext api=2 group, root`, 0x38e attaches of CA surfaces |
| unlock, Home, power-off in 4 orientations | PASS with 3.2.2's coordinates (display-rotation 270): unlock (959,477)->(959,47); the power-off sheet and a clean exit in accel-orientations 1-4 (16-19 s); Home leaves Safari. 4.x differences are elsewhere (below) |
| regress.py (`--device`) | 7/7: boot, usbmux (ProductVersion 4.2.1), afc (sha256 at 1 B-256 KiB), persist, wifi, net (Safari over Wi-Fi), audio |
| snapshot-check (`--device`) | 6/6, GL CA live across save/restore: screen 0.0% diff, touch + keyboard + Wi-Fi fetch after resume, usbmux, no panic, mid-sound audio 0.90/0.89/0.90 |
| tearcheck (`--boot DEVICE`) | 53.8 fps capture, 2 torn of 251 changed (0.8%), 0 black/partial (3.2.2 today: 53.4 fps, 1.5%) |
| respcheck (`--device`) | lock lit 14.0 s, knob tracks 1.9 s later; tap-to-highlight median 214.5 ms (216/225/214/215/214/515/214/204; 3.2.2 ~210) |
| animfps (fixed, below) | 44.9 frames/s while animating on 8C148, 41.5 on a fresh 7B500 device, same scenario |
| settings | time right (UTC; the zone is the fresh device's Pacific default, the app syncs it), About: 4.2.1 (8C148), 13.7 GB capacity, Wi-Fi and Bluetooth addresses; battery 83% charging ("83% Charged" on the lock screen); Wi-Fi icon and `qemu-ios` in Settings; Bluetooth "Unavailable" (BTServer off, as on 3.2.2's images); upright portrait after a reboot from landscape, landscape lock screen when the accelerometer says so at boot; Safari loads https://example.com through the web proxy (itwebproxy direct, TLS bridge) with the lock icon |
| app pass | 44 works, 2 with issues, 5 crash, 1 won't install of 52 (docs/ipad1/app-compat.md, 4.2.1 table). DoodleJump 3.7 works; KP by Bing's repack still fails installd |
| multitasking | PASS: a double Home (0.15 s presses, 0.2 s apart) raises the switcher bar with Safari; tapping it resumes Safari (2/2). Presses 0.1 s long do nothing; a 0.1 s gap reads as two presses (Spotlight) |

What 4.2.1 needed, beyond the pipeline:
- **USB device side** (`hw/arm/ipod_touch_usb_otg.c`): the driver clears the global IN/OUT NAK with
  DCTL.CGNPINNAK/CGOUTNAK and polls GINTSTS for GINNakEff/GOUTNakEff to drop (`DWCUSB_GINT_GINNAKEFF did
  not clear in time`, usbmux never attached); the model now drops them. AppleUSBDeviceMux relies on the
  ZLP a host sends after a bulk OUT write that ends on a max-packet boundary (usbmuxd does, for
  `length % wMaxPacketSize == 0`); the transfer-per-transaction transport had none, so the next transfer
  was read as the same message (`expected 16384 bytes, received 32768`, `message was too large (65536
  bytes)`, TCP RST): AFC past 16 KiB failed. The model delivers that ZLP on the next arm.
- **USB keyboard**: 4.x's `AppleS5L8930XUSBArbitrator::handleStart` (0x80525788) publishes the host
  nubs for the DT's `hsic-enabled` only when the boot-arg `enable-hsic` is 1 (kboot passes it; 3.x ignores
  it). `_publishNubs` gives the host side `AAPL,power-supply` 50, and IOUSBFamily refuses QEMU's 100 mA
  keyboard ("not enough power available"); `usb-kbd,max-power=20` (a new property) presents a low-power
  one. 4.2.1 then raises "Cannot Use Device / The connected USB device is not supported", OK at
  (565,382) a few seconds after unlock, and the keyboard types.
- **Tests**, version-detected from the device lock: the Wi-Fi lease line (`receivedIPv4Address():
  Received address ...`), the alert above, the unlock sound (4.x plays `UISounds/unlock.caf`, the same
  file 7B500 ships), Settings' icon (445,470; Game Center has 3.2.2's spot). `regress.py`,
  `snapshot-check.py`, `respcheck.py`, `app-compat.py` take `--device DIR`, `tearcheck.py --boot DIR`,
  with a private NOR copy beside each overlay (paired with it in snapshots). animfps's swipe predated the
  upright-portrait UI and never unlocked (it measured the lock-screen shimmer); it now unlocks with
  regress.py's slider and taps Notes/Calendar, which sit at the same place on both versions.
- Screendumps of the scenario scripts: tearcheck's and animfps's page swipe lands on the Spotlight page
  on both versions (one app page), so their numbers include its caret blink.

Open:
- The power-off gesture from a *lock screen* hung 2 of 37 tries (QEMU still running 45 s later), with
  software CA and with GL; 0 of 20+ from the home screen. Not reproduced since; boot-smoke now saves a
  `-stuck.png` when it happens.
- glishim lacks `glDiscardFramebufferEXT` (slot 838 of the 841-slot layout; Bejeweled calls it). It is a
  hint, so a no-op is correct.

## What is done (2026-09-27, earlier session)

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
| Trust gate: what the `+0x1f8` virtual reads. ~~Gated on this.~~ **DONE 2026-09-27** — it is the DeviceTree `secure-root-prefix='md'` property + root-device match (`SecureRoot` IOResource from AppleARMPlatform), not the img3 chain. Route 1 confirmed viable, no kernel patching. Evidence above. | — |
| ~~Restore-ramdisk one-shot~~ **DONE 2026-09-28** (see "The one-shot, as built"). The plan was: (1) **kboot**: teach `ipad1_kboot.build` an optional RAM-disk mode — add a segment carrying the raw-HFS ramdisk at a chosen PA in DRAM, a `chosen/memory-map` `RAMDisk` entry `(pa,len)` for it, boot-args `rd=md0` (root selects `md0`), and keep the restore DeviceTree's `secure-root-prefix='md'` (do NOT overwrite it in `fill_dt`; the normal-boot DT has no such prefix so 3.x/normal boots are unaffected). Restore kernelcache+ramdisk+DeviceTree come from the BuildManifest's Update/Restore identity via `ipad1_fw.py`. (2) **guest helper** (built like the others, ldid-signed, AMFI boot-args already on): call the two stable symbols `format_effaceable_storage`-equivalent (AppleEffaceableStorage user client sels 3/4) + `_MKBKeyBagCreateSystem(NULL, dataMount)`; mount the data volume, write `/private/var/keybags/systembag.kb`, `it_seal`-style `reboot(RB_HALT)`. (3) **pipeline**: `ipad1_device.py create` runs this one-shot boot against the device's *writable* NAND + `nor-rw` before the normal boot+seal; verify effaceable formatted + `systembag.kb` present; version-gated by manifest (`options.writable_nor`/a `restore_keybag` flag). | — |
| ~~GLI shim for 4.2.1~~ **DONE 2026-09-28**: GL CoreAnimation is the 8C148 default (below) | — |
| ~~Activation~~ the user's own 4.2.1 tool, passed as `--activation-hook` | — |
| ~~Unlock / power-off coordinates on 4.x SpringBoard~~ **DONE 2026-09-28**: unlock, Home and power-off in all four orientations verified on an activated device | — |
| ~~End-to-end validation with GL CA~~ **DONE 2026-09-28** ("End to end, activated") | — |
| Lock-screen power-off hang (2/37), glDiscardFramebufferEXT | open |

Scratch artefacts (untracked): `~/Developer/qemu-ios-files/ipad1/repro-8C148/` (decrypted firmware,
extracted IOP images `iopfw-{7B500,8C148}.bin`, probe boots `p1`-`p7`, the diagnostic `diag/it_ps.c` and the
keybag experiment `diag/it_kb.c`; `re/kc.py` — the kernelcache VA↔file/xref/disasm helper used to resolve
the trust gate above; its source is gone, `re/kc2.py` replaces it), `bin/pwrprobe.py` (orientation and
power-off sheet probes) and `bin/kboot_rot.py` (the display-rotation experiment).
