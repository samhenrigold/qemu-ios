# iPod touch 2G from a stock IPSW (manifest → device)

LightTouchMac's Swift FirmwareKit builds a device from declared inputs only: a sha1-pinned IPSW, its catalog
entry's keys and a seed for a synthetic identity; no third-party tarballs (the iPod no longer gets a shell). It is the
same catalog and `device.lock.json` as the iPad; the iPod board is FirmwareKit's N72 recipe. CATALOG below is
LightTouchMac's `LightTouchMac/Resources/firmware-catalog.json`; 4.x builds also take `--helper` with the LightTouchDevice executable.

```
firmwarekit create --catalog CATALOG --id n72ap-7E18 --ipsw IPSW --out OUT
tests/ipod/fresh-device.sh IPSW OUT                            # create + boot, fsck, persist
tests/ipod/regress.py --qemu build/qemu-system-arm --device OUT --checks boot
```

`OUT` holds `nand/` (page directory), `nor.bin`, `iBoot.bin` (3.x+ only), `gid-blobs.bin`, `identity.json`
(mode 600), `device.lock.json`, `create.log`. `regress.py --device OUT` picks all four up (explicit
`--base-nand/--nor/--direct-iboot/--gid-blobs` win).

## Status

| build | pipeline | furthest point | blocker |
|---|---|---|---|
| 3.1.3 7E18 | complete | SpringBoard up, GL CA through the shim, "Connect to iTunes" (lit, see below) | activation |
| 4.2.1 8C148 | complete (NOR, NAND, GLES check, AppSync, gid-blobs, activation hook, data protection) | home screen, GL CoreAnimation through the shim (`regress.py --device ... --checks boot,gles` PASS; see "8C148: GL") | none for GL |
| 2.1.1 5F138 | complete (activation hook; the guest package's OpenGLES hook, the GL front end; AppSync; no modern guest helpers) | home screen once a host sets the time (brick state), GL CoreAnimation through the front end (`regress.py --device ... --checks boot,gles` PASS; see "2.x: CoreAnimation through the GL front end") | ad-hoc app install + launch verified with the legacy AppSync helper |
| 3.0 7A341 | complete (the legacy-linked engine MBXGLEngine-30, the n72-ios30 package, installd AppSync; no modern guest helpers) | home screen once a host sets the time (brick state, every boot), app GL through the engine (`regress.py --device ... --checks boot,gles` PASS; see "3.0: GL through the legacy-linked engine") | no helpers yet (smoke #10) |
| 2.2 5G77a, 2.2.1 5H11a | complete (as 2.1.1; the NOR wraps every image but the LLB) | home screen through LightTouchMac's pipeline (matrix, 2026-09-29) | the hold button (below) |

### P1, 7E18: activation

The fresh image boots to SpringBoard in about 90 s and shows the unactivated "Connect to iTunes" screen
(`regress.py --checks boot`: lit=35868, need 180000; screenshot in
`qemu-ios-files/ipod-ipsw/runs/7E18-a-boot/boot1/boot.ppm`). The shipping image passes only because its
`/var/root/Library/Lockdown` holds an Apple-issued activation record for the unit whose serial/MACs its NOR
carries; a synthetic identity cannot have one.

The opt-in activation hook (the user's `patch_lockdownd.py`, run as a black box on a copy of 7E18's
`/usr/libexec/lockdownd`) refuses this binary: `expected one verified activation branch, found 0`, exit 1,
file unchanged. It is designed to fail closed and is validated only on iPad 7B500, so this thread stops
here. Everything before activation is proven (NOR, iBoot, kernel, FTL mount, launchd, lockdown, SpringBoard, the GLES
shim drawing CA through the host). The 8-check acceptance needs an activated image; `ipod2g_device.py` would
take a hook the same way the iPad bake does (`ipad1_rootfs.activation_hook`) once one exists for 3.1.3.
Nothing else was tried.

### P2, 8C148: NAND identification

Everything the builder derives came out right for 4.2.1 (`device.lock.json` "derived"): kernelcache
`kernelcache.release.n72` installed at `/System/Library/Caches/com.apple.kernelcaches/kernelcache` (the path the
4.2.1 iBoot names, not 3.1.3's `kernelcache.s5l8720x`), xnu-1504.58.28, iBoot-931.71.16, NAND epoch 4, SHSH
wrapped, NOR without `nsrv` (4.x ships no needservice), AppSync by symbol (`_MISValidateSignature` at
cache off 0x40d76d4), installd job `com.apple.mobile.installd.plist`. The GLES check then refused the shim
(`dispatch table differs from gli-dispatch-7E18.tsv at slot 441 (841 vs 822 slots)`); since 2026-09-28 the shim
reads the layout at load ("8C148: GL" below).

Boot: the emulated AES engine has no GID key, so it answered only KBAGs in its built-in 5F138/7E18 table and
exited on 4.2.1's first NOR image. The new `gid-blobs=FILE` machine option (the builder writes
`gid-blobs.bin` from each img3's KBAG and the keys page) fixed that. iBoot then prints its banner with the
synthetic serial and stops at `[NAND] findNandInfo:291 No NAND Detected` / `[FIL:INF] could not find NAND
config in the new NAND tables` → `root filesystem mount failed` → recovery. The emulated chip ID 0xb614d5ad is
still in 4.2.1 iBoot's table (0x250e4), but the entries have a new layout (id, 0, geometry... instead of
id, 0x100000ff, ...). The chip ID is not the problem; the emulated controller was (below). Activation will block after the
kernel, as on 7E18.

#### How iBoot probes the NAND (traced: disassembly of both decrypted iBoots + the gdbstub under lldb)

FMSS is a programmable sequencer in front of the FMC NAND controller. The driver writes a program's
address to 0xC04, the program's inputs to the 0xDxx variables, and starts it with CSCTRL (0xC00) bit 0;
completion is CSIRQ (0xC0C) bit 0. `ResetAndReadId` (8C148 iBoot 0x0ff0d198, 7E18 0x0ff14e08) runs the
reset program (8C148 0x25988, 7E18 0x25258: FMCTRL0 all-CE, FMCMD 0xff), then sets D08 = ID buffer,
D0C = 8 chip enables and runs READ ID (8C148 0x25a60, 7E18 0x25330). READ ID, per CE: FMCTRL0 (0x0) =
CE bit (bits 1..8) | 0x77001, FMCMD (0x8) = 0x90, one address byte 0 (FMANUM 0x2c, FMADDR0 0xc), FMDNUM
(0x30) = N-1 data bytes, poll 0x40 bit 1, then store FMDATA to the buffer.

| | 3.1.3 iBoot-636.66.33 | 4.2.1 iBoot-931.71.16 |
|---|---|---|
| ID bytes per CE (FMDNUM) | 5 (4), one word from 0x60 | 8 (7), words from 0x60 and 0x64 |
| record stride, CEs | 4 bytes x 8 | 8 bytes x 8 (buffer 0x80, 16 records) |
| between D08 write and start | cache flush | `memset(buf, 0, 0x80)`, cache invalidate |
| page-read program | 0x254a0 | 0x25be8, identical (0x10d0 bytes) |
| VFL context checksum | verifier is a stub (`return 1`, 0x0ff086b4) | verified (0x0ff1bba2) |

Program instructions are two words, `op<<24 | a<<16 | b`, `imm`: 00 end, 01 `fmc[b]=imm`, 02 `fmc[b]=r[a]`,
04 `r[a]=reg[b]&imm` (FMC or 0xDxx), 05 `r[a]=imm`, 07 wait for FMC event a, 0b/0c/0d/13 `r[a]=r[b]` or/add/sub/shl
`imm`, 0e/17 branch to byte offset `imm` if `r[a]` != 0 / == 0, 11 `mem32[r[b]]=r[a]`.

4.2.1's `findNandInfo` (0x0ff025e8) takes the 16 records; a record is a chip unless it is all 0x00 or all
0xff (0x0ff022b0), all present chips must match (0x0ff0228c compares 4 bytes, a zero byte on either side
is a wildcard), then it looks the ID up in the new table (stride 0x1c). No valid record: `No NAND Detected`.

Root cause: the model DMAed four 4-byte IDs at the moment the CPU wrote D08. 4.2.1 zeroes the buffer
after that write, so it saw no chip at all (which is also why the 8-byte-record trial changed nothing).
Fix (hw/arm/ipod_touch_fmss.c `fmss_run_script`): run the guest's program at start, with FMCMD0x90 receiving the selected CE's ID bytes on FMC4=0xe2, then the
counted FMC40 receive transfer moving those bytes into FMDATA; each build's own
program lays out its own records. The flash
is four Hynix dies, ID `ad d5 14 b6` (0xb614d5ad, in both builds' tables), CE 4..7 unpopulated (0). An
opcode outside the decoded set stops the program (LOG_UNIMP); the page read/write path is still the
CPU-side model. Result: `[FIL:INF] Found chip id 0xb614d5ad on CS 0..3`.

Next, `[VFL:ERR] CHECK_VFL_CXT_CHECK_SUM_NO_ASSERT(0) failed`: the VFL context carries the word sum
+ 0xaabbccdd at +0x7f8 and the word xor ^ 0xaabbccdd at +0x7fc over its first 0x7f8 bytes (3.1.3's
writer, 0x0ff086b8, computes the same). The generator wrote zeros; 3.1.3 never checked. Fixed in
`imgtools/ipod2g_nand.py` `vfl_page`. Then: `VSVFL Register`, `VFL_Open [OK]`, `FTL_Open [OK]`,
`HFSInitPartition`, `Loading kernel cache`, xnu-1504.58.28 runs.

#### The 4.2.1 kernel

The kernel is based at VA 0x80000000 (3.x: 0xC0000000), and iBoot builds boot_args at 0x08825000, past
the late boot-args scan's first 8 MiB. The scan accepts either base and covers 16 MiB
(hw/arm/ipod_touch_2g.c `boot_args_signature`); `imgtools/klog.py` reads the msgbuf with either base.
The early command line reaches it too (see "Remaining emulator compatibility behavior"), so AMFI's
flags and the serial console are in effect; without it amfid rejected every re-signed binary
(`verify_code_directory returned 0x10004005`).

The kernel's NAND stack never read a page: `AppleS5L8720XFMSS` ran its reset program and then slept in
`IOSleep(10)` (0x807c4218) before READ ID, and never woke (lldb over the gdbstub: the breakpoint after the
sleep is never hit). xnu-1504 arms timer 4 with START|MANUALUPDATE (0x80068078: `a8 = count; a4 = 3`) and,
after its FIQ (0x80068024), programs a new deadline only if it is sooner than the running period. The
timer model treated MANUALUPDATE as one-shot, so after one expiry nothing fired again. MANUALUPDATE only
latches the buffer; the timer reloads (hw/arm/ipod_touch_timer.c). With that: `[FTL:MSG] VFL_Open/FTL_Open
[OK]`, `Got boot device = ... AppleNANDLegacyFTL/IOFlashBlockDevice/... Untitled 1@1`, `BSD root: disk0s1`,
launchd.

#### Data protection

keybagd's own log, persisted on the data volume, showed the blocker: `validateSecureFile: Unable to load
/private/var//keybags/systembag.kb` → `FATAL KEYBAG ERROR: kb_load` → `Rebooting...` about 27 s in, into
restore mode, so the next boot stopped at iBoot's recovery screen. Same as the iPad (docs/ipad1/ios4.md);
the 4.2.1 DT's `nor-flash/effaceable` is `effaceable,nor`, so the lockers live in NOR.

**The one-shot** (firmwarekit's keybag step, run by `ipod2g_device.py` for manifests with
`options.data_protection`, i.e. 8C148). As on the iPad, the IPSW's own (Update) restore ramdisk, a private
copy with `it_keybag` (contrib/it-keybag, armv6 build `build-ipod.sh`: the iPod volume is one, disk0s1,
data at `/private/var`) as `restored_external`, boots as `md0` so the root is a SecureRoot (the normal
4.2.1 DT already carries `secure-root-prefix = md`). The difference is the handoff: everything iBoot loads
is a signed img3 (its `boot-ramdisk` NVRAM path loads type `rdsk` to 0x0c000000 and validates it), so a
modified ramdisk cannot come through iBoot. iBoot boots the device normally; at the kernel's entry (LC_UNIXTHREAD pc, MMU off) a gdbstub breakpoint adds what iBoot's restore
path adds: the ramdisk at topOfKernelData, a `RAMDisk` (pa, len) entry in a spare `MemoryMapReserved`
slot of chosen/memory-map, an empty chosen/root-matching, topOfKernelData moved past it,
and the host-owned `rd=md0` command line. `it_keybag`
formats effaceable (nor-rw) and creates `/private/var/keybags/systembag.kb` (1335 bytes); the overlay's
pages (at their logical homes) are folded into `nand/` and the written NOR becomes `nor.bin`. About 40 s.

**AES.** The keybag made that way failed to open on the next boot (`kb_deserialize=e00002c9`). The AES
model's single-shot UID path never writes a result where the kernel reads it (it reads the kernel's output
buffer at 0x20 and writes into its input at 0x28), so the UID-derived keys 0x835/0x89B come out as the
untouched output buffer, zeros, and whatever AppleKeyStore wrapped cannot be unwrapped. The machine option
`aes-uid=engine` runs UID operations through the engine like a custom key (input 0x28, output 0x20,
KEYLEN's direction) with a fixed stand-in key, and GID operations shorter than a KBAG (the restore kernel
derives key 0x837 from a 16-byte seed, `345a2d6c5050d058...`; unknown, and fatal before) the same way with
a stand-in GID key. KBAG lookups are unchanged. The default stays `legacy`, because existing 3.x images'
keychain items were encrypted under the legacy keys. Every device `imgtools/device.py` makes for the iPod
(5F138, 7E18, 8C148) boots with the engine, data protection or not, and records it in `device.lock.json`
(`"machine": {"aes-uid": "engine"}`); `regress.py --device` applies it. nand-current and devices adopted
from it have no such lock and keep `legacy`. A fresh 7E18 with the activation hook passes all eight default
checks this way, and a generic-password keychain item added in one boot reads back after a clean
shutdown and reboot.

### 8C148: GL (2026-09-28)

The MBX shim discovers the firmware's dispatch layout at load, like the iPad's glishim (both are one
binary per architecture; `contrib/it-gles/gles_dispatch.c`): the ObjC @encode of `__GLIFunctionDispatchRec` in the
running OpenGLES names every slot (7E18: 822, 8C148: 841, the same layout as the iPad's 8C148), and each slot is
matched by name to `include/hw/arm/guest-services/gles-names.h`, the table the host is built from too, whose ids
are what the wire carries (3.1.3's slot numbers below 822, assigned above). A firmware without the @encode gets
its layout from OpenGLES's exported trampolines instead, decoded in place (the slot is loaded straight into pc,
`mov lr, pc; ldr pc, [ip, #off]`, or tail-called after a conditional early return, `popeq`/`bxeq lr`; 4.x's
`glIs*` call it with `blxne`), which is also the cross-check `gles-debug=on` runs. `contrib/ipad1-gles/glitsv.py`
still derives a `gli-dispatch-<BUILD>.tsv` offline, as research: `docs/ipod/gli-dispatch-*.tsv` are what the
discovered tables were checked against, not build inputs.

What 4.2.1 changed, read from the 8C148 cache: EAGL still loads `MBXGLEngine.bundle` through `GLESGetEGLInterface`
when `AppleMBXDevice` matches (`eagl_init` 0x34ff870c; no libGFXShared or gld plugin on the MBX path), and the
MBXGLEngine it loads is in the shared cache, so the builder creates dyld's `enable-dylibs-to-override-cache`
switch (`ipad1_rootfs.gli_uncache`, the iPad's). The interface grew two entries, `+0x24` Set / `+0x28` GetProperty
(`setParameter:to:` / `getParameter:to:`); CA never called them here. And the swap notification's first argument,
`IOMobileFramebufferGetID`, is no longer an IOConnect port (0x80b94000): the 3.1.3 call fails, CA's swaps never
complete and SpringBoard sits on the boot logo until the watchdog restarts it, so the shim then signals the main
display (`IOMobileFramebufferSwapSignal`), as glishim does. The engine is ldid-signed.

Fresh 8C148 device (`firmwarekit create --catalog CATALOG --id n72ap-8C148 --ipsw IPSW --out OUT --helper LIGHTTOUCHDEVICE`, lock
`derived.gles = "shim MBXGLEngine-8C148"`): `regress.py --device OUT --checks boot,gles` PASS, home screen lit=285214,
GLTest magenta 0.141 / cyan 0.281 / yellow 0.141, no unimplemented slot. Host log: one SpringBoard
`GLESGetEGLInterface`/`GLESCreateGC` pair, `[gles] host GL up`, `swap: framebuffer ID ... signaling the main
display`, then GLTest's `GLESBindView` and `present tally: ok=600 failed=0`. 7E18 with the new shim staged
(`regress.py --stage-gles-shim`, default tier): 8/8, same GLES colors.

### 8C148: Wi-Fi and screen lock (2026-09-28)

Two 4.2.1 platform bugs from the 20260928c test, both in the card/PMU models, both fixed at the register the
driver reads rather than per version (branch `ipod-421-platform`).

**"No Wi-Fi".** With `IPOD_SDIO_TRACE=1` the 4.2.1 driver enumerates the card, enables function 1, sets the
backplane window to 0x18000000, reads chipcommon ChipID (CMD53, 4 bytes) and then re-enumerates from CMD5, in a
loop, with no firmware download and nothing on serial. `AppleBCMWLANChipManager::withDriver` (8C148 0x80779998)
takes `chipInfo & 0xffff`: 0x4325 -> rev 5 "BCMWLAN revision D0" / 6 "D1", 0x4329 -> B0/B1/C0, anything else
"Unknown/Unsupported chip ID". The model answered 0x00050000: revision 5, chip number 0. 3.1.3's
AppleBCMWLAN-1.25 only looked at the revision. Fix: `CHIPCOMMON_CHIPID` 0x00054325, the real chip's number and
the same revision (`include/hw/arm/ipod_touch_sdio.h`). Then: 256 KiB firmware download, "dongle announced
ready", the `ver`/`cap`/`event_msgs`/... CDC set, association, DHCP (`regress.py --checks boot,wifi`: PASS,
"Link Up on en0, 2 DHCP reply/replies"). 3.1.3 accepts the same value (its SDIO trace is unchanged).

**Lock only dims.** PMU trace (`IT_PMU_TRACE=1`) at Hold: 4.2.1 writes 0x30=0x26, 0x31=0x00 (the dim), then
0x1d=0x12 and 0x10 0xe0 -> 0xa0; at wake 0x1d=0x12, 0x10 -> 0xe0, then 0x30=0xd2, 0x31=0x05. 3.1.3 writes
0x30=0x01, 0x30=0x00, 0x31=0x00 and leaves 0x10 at 0xe0. So 4.2.1's `function-backlight_enable`
(`AppleD1759PMUBacklightEnableFunction`, the DT `backlight` node's PMU function) is bit 6 of 0x10, the
regulator-enable register, and the level is left dim; the model only read 0x30. iBoot never writes 0x10 and its
logo lights, so the bit is set out of reset. Fix (`hw/arm/ipod_touch_pcf50633_pmu.c`): the panel level is 0x30
while 0x10 bit 6 is set, 0 otherwise. Probe (screendump lit count, no `IT_LCD_BRIGHT`): 8C148 Hold -> dark
(lit 0) within 1 s, Home -> lit 460207 within 1 s; the lock screen's own idle sleep goes dark the same way.
7E18 (nand-current) unchanged: dark within 1 s, lit 253200 within 1 s. 0x31 (0x05 whenever the light is on in
every build, iBoot included) is stored but not decoded.

Gates (`regress.py`, one emulator at a time): fresh 8C148 device (the app's 20260928c preparation) with
`boot,fsck,persist,appinstall,applaunch,gles,agent,audio,wifi,webproxy`: 10/10 (4.5 min; webproxy = the baked PAC
through the web proxy's guestfwd, Safari's path). nand-current 7E18 with `--stage-gles-shim`, same list over two runs: 9 PASS, webproxy SKIP (that image has no
baked PAC).

### 2.2 / 2.2.1 (5G77a, 5H11a): the LLB's 0x38100000 block and the epoch-2 NOR (2026-09-29)

Both are iBoot-385.49, Restore.plist SCEP 2, SEPO 2 on LLB and iBoot. Two findings, in boot order:

1. SecureROM → LLB looped into DFU (a CPU reset every ~0.15 s, nothing on the UART). `-d int` showed a data abort,
   DFSR 0x808, DFAR 0x38100044, then the LLB's own watchdog reset. The store is in the routine that latches the
   security epoch into POWER_ID (`(POWER_ID & 0xffffff) | max(chipid epoch, 2) << 24`; the ROM had already written
   0x02000001): it also writes 0x38100040 <- 1, 0x38100044 <- 0x033f0100 and 0x3D7000bc. 0x38100000 was unmapped;
   2.1.1's LLB (385.22) and every 3.x+ iBoot never touch it. It is now an unimplemented RAZ/WI device; what it is
   remains unknown (LightTouchMac docs/smoke.md #11).
2. iBoot then loaded the kernelcache and stopped at `load_macho_image: failed to load device tree` / recovery with
   2.1.1's NOR (only iBoot's SHSH wrapped). The epoch-2 chain unwraps every NOR image it loads; the SecureROM still
   verifies the LLB raw (wrapping it too keeps the ROM at 0x3186). `ipod2g_device.py` (and FirmwareKit's N72Board)
   wrap every image but the LLB when SCEP >= 2. Kernel xnu-1228.7.36 then boots to SpringBoard.

The hold button does nothing on 2.x (2.1.1 too): no power sheet, no lock, so the machine's powerdown sequence never
completes. 2.x's DeviceTree has no `function-button_hold` (3.x: GPIO 0xC02), only `function-wake_button_hold` (PMU
STAT 0x191). With IT_GPIO_TRACE/IT_PMU_TRACE the guest acks the hold GPIO edge (group 3 bit 26) and never reads the
PMU; awake, the kernel unmasks only EVENT_C bits 2/4/6 (masks 0x95/0xdf/0xab), while the model latches hold at
EVENT_C bit 1 on the press only. The hold's 2.x PMU event path is unmodeled (LightTouchMac docs/smoke.md #12).

### P3, 5F138: LLB → iBoot

2.1.1 needed pipeline fixes, all derived: no BuildManifest (component paths from Restore.plist and the board
name, `ipad1_fw.components`), the final partial AES block of 2.x img3 left in plaintext (detected from the
kernelcache's Adler-32 and applied to every component), NAND epoch 1, only iBoot SHSH wrapped, no direct iBoot
(2.x boots bootrom → NOR LLB), no shared cache (AppSync off in the manifest: 2.x has a standalone libmis.dylib, which
no symbol-located patcher covers yet), firmware's own libncurses kept. The generated NOR must wrap **only iBoot's SHSH** under the emulated
UID. LLB unwraps that signature, while 2.x iBoot verifies its other NOR images
raw. The previous all-or-nothing `--no-wrap-shsh` setting was wrong.
`--wrap-shsh-types ibot` now expresses this mixed layout, and the board builder
selects it for 2.x. The lock records `wrap_shsh_types`; the older `wrap_shsh`
boolean denotes the all-images convention.

Measured: the old generated NOR differs from the working dump's image area
only at iBoot's 128-byte SHSH. Wrapping that SHSH reproduces the dump's image
area exactly. With signature forging disabled, gdb reaches the stock iBoot
entry through SecureROM and LLB; serial then reaches xnu-1228.7.27 and userland.
The remaining screen stall at lit=9852 is independent of NOR validation.
`tests/ipod/test_nor_wrapping.py` checks mixed/all/no wrapping and preservation
of signed bytes; a generated 5F138 NOR matches the traced corrected NOR exactly.

## Inventory: what the shipping image (nand-current.new) depends on, and where each comes from now

| input | shipping lineage | fresh pipeline (file:line) |
|---|---|---|
| NOR IMG2/SysCfg/nvram | copied from a real unit's NOR dump (serial, battery serial, BT and Wi-Fi MACs) | synthesized from identity.json, `build_nor.synth_base` (imgtools/build_nor.py:224, nvram 207); with the unit's values it reproduces `ios3/nor_7E18.bin` byte for byte |
| NOR images | IPSW all_flash, SHSH wrapped | same; image set = stock order ∩ the IPSW's all_flash manifest (`ipod2g_device.build`, :158) |
| iBoot | `ios3/iBoot.bin` (decrypted, stock; identical to the IPSW's) | `OUT/iBoot.bin` from the decrypt cache |
| GID table | built into hw/arm/ipod_touch_aes.c (5F138, 7E18) | plus `OUT/gid-blobs.bin` (`ipod2g_device.gid_blobs`, :140) |
| identity | real unit's (NOR) | `ipod2g_device.identity` (:84): serial and MACs from firmwarekit's KBoot `synth_identity`, battery serial from the seed, Mod#/Regn from the manifest; UDID SHA1(serial+Wi-Fi+BT) |
| NAND geometry / FTL metadata | 50 pages copied from `nand-canonical` (itself generator output) | generated, `imgtools/ipod2g_nand.py` (build_nand.py:219); 49/50 identical to nand-current.new, the 50th being the protective MBR's size, stale (128010) there and correct here |
| NANDDRIVERSIGN epoch | '411C' (the 3.1.3 kernel rewrote the template's '111C') | Restore.plist `SCEP` |
| volume | rootfs grown to 1835008 blocks, fstab rw | same (build_nand.py), zero blocks not written (:243) |
| kernelcache | encrypted IPSW img3 at kernelcache.s5l8720x | path read from the decrypted iBoot (`kernelcache_path`, :96) |
| activation / Lockdown | Apple record + pair records for the real unit | none (blocker above) |
| fake Wi-Fi setup | not in the image (SystemConfiguration is configd's own runtime output) | none needed |
| guest services | it_agent, it_typein, sblaunch, sbdlicon, markers, sound defaults (bake-guest-tools.sh) | same script, run inside build_nand's mount (`ipod2g_device.bake`, :306), owners patched in the catalog |
| pasteboard | it_pbd binary present, job retired | not installed (the agent owns the clipboard) |
| sound defaults | set-sound-defaults.py | same |
| AppSync | cache MISValidateSignature (by symbol) + libappsync in installd | same script, contrib/appsync/patch-appsync-dylib.sh |
| GLES shim | MBXGLEngine shim, CA_ENABLE_OGL=1, MBX2D/auto off | same, the one `contrib/it-gles/MBXGLEngine`, which reads the firmware's dispatch layout at load (`ipod2g_device.gli_engine` only logs what it will find); 3.0 (the bundle a plain file, legacy dyld) → the same source legacy-linked, `MBXGLEngine-30`; 1.x/2.x (no engine bundle) → the guest package's OpenGLES hook (`contrib/it-gles/gles2x.c`, the same core) and CA_ENABLE_OGL=1, only when the stock binary's exports match `opengles-2x.exports` (`ipod2g_device.gles2x_front_end`) |
| shell + ssh | Cydia bootstrap files copied as uid 99, stock modes clobbered by `chmod 755`, sshd by overwriting ReportCrash.SafetyNet, host keys shared by every copy | **none**: no freeze, OpenSSH or OpenSSL; guest services are stock lockdown services plus it_agent v2 (docs/ipod/guest-services-plan.md), marker `.lt-guest-tools-v3` |
| web proxy / CA trust | itproxy/ittrust run over SSH | the iPad's PAC baked into the en0 Wi-Fi service (`ipod2g_device.install_web_proxy`); CA by a MCInstall profile at run time |
| byte patches | none left on the default path: installd/SpringBoard are stock | none; see "emulator-side per-version code" |

### Remaining emulator compatibility behavior

- hw/arm/it_iboot.c (board-agnostic; the iPod machine calls it after staging iBoot) finds by
  pattern, in any iPod touch 2G iBoot (2.1.1 .. 4.2.1, pinned by tests/ipod/test_iboot_literals.py):
  the build's security epoch
  (the floor its epoch helper applies to the chip ID fuse field: 1/2 on 2.x, 3 on iBoot-596 = 3.0,
  4 from iBoot-636 on), which the SYSIC model returns in POWER_ID[31:24] in place of the LLB's
  latch, so miu_init's "Epoch Mismatch" panic no longer keys on one build; and the 2.x
  gBootArgs.commandLine buffer for the NAND-boot data write. The host boot-args data write finds
  `boot_args` by signature (kernel base 0xC0000000 or 0x80000000).
  The direct-iBoot empty-string literal redirect is removed: that literal also
  names the DeviceTree root, so replacing it prevented iBoot from populating
  serial/model/region and produced the wrong USB identity. The existing repeated
  handoff-buffer writer supplies the compatibility arguments. Native 7E18
  identity, guest services, import and cold reboot validate this boundary;
  other firmware generations still need native timing qualification.
- 3.0 (7A341): the baked helpers (it_agent, it_typein DYLD_INSERTed into SpringBoard, sblaunch, it_prefs)
  are linked for the 3.1+ dyld; 3.0's refuses LC_DYLD_INFO_ONLY like 2.x, so `ipod2g_device.py` omits them
  below 3.1 (stock SpringBoard) until a legacy-linked set exists. 3.0 has no dyld shared cache (it arrived
  with 3.1): AppSync is the installd injection alone (contrib/appsync), and the GL engine is a plain file,
  which gets the legacy-linked shim (see "3.0: GL through the legacy-linked engine").
- The obsolete fixed-address logo thunk is removed along with the DeviceTree thunk.
- The research-only IT_AMFI_ALLOW_TASKPORT kernel patch and its address overrides
  have been removed; guest integration uses the existing boot-args and AppSync path.
- The legacy command-line data write (without direct iBoot) discovers its buffer
  from iBoot's own literal references to `gBootArgs.commandLine = [%s]`; absent,
  ambiguous and out-of-range matches cause no write. No fixed build address remains.
- The BCM4325 card accepts the unit's `wifi-mac` machine option for its CIS,
  NIC and event identity; unconfigured callers retain the old card address.
  `bt-mac` supplies the Apple OTP record so the 2.x driver retains the unit's
  Bluetooth address instead of generating a fallback. Both values come from the
  prepared identity. See [radio identity](../research/n72-radio-identity.md).

## Assumed → derived

| per-build value | derived from |
|---|---|
| component files | BuildManifest (3.x+), Restore.plist + board name (2.x) |
| img3 tail convention | kernelcache Adler-32 (2.x plaintext tail, 3.x+ encrypted) |
| GID KBAG plaintexts | the IPSW's KBAG tags + the keys page → gid-blobs.bin |
| NAND epoch (NANDDRIVERSIGN) | Restore.plist DeviceMap SCEP |
| SHSH wrap | all images for 3.x+; only iBoot for 2.x (LLB unwrap) |
| direct iBoot | ProductVersion major ≥ 3 |
| NOR image set | stock order ∩ all_flash/manifest |
| kernelcache path in the volume | the decrypted iBoot's `/System/Library/Caches/com.apple.kernelcaches/...` string |
| kernelcache member | BuildManifest KernelCache / Restore.plist KernelCachesByPlatform |
| GL dispatch layout | read by the shim at load from OpenGLES's `__GLIFunctionDispatchRec` @encode (else its trampolines); the prepare step only logs the slot count and any field the name table lacks |
| MISValidateSignature | shared-cache symbol table (appsync_cachepatch.py) |
| installd job name | whichever of the two known plists exists |
| volume size, Mod#, Regn | manifest (`volume_blocks`, `model_number`, `region_info`) |

## Fresh 7E18 image vs nand-current.new (tests/ipod/nand_manifest.py)

(Measured before the builder dropped the shell package; the shell and sshd rows below no longer apply.)

Method: `nand_manifest.py --img` (new: a flat volume) on the builder's volume and on `dumpvol.py` of
nand-current.new. 538 differing rows, every one in these classes:

- **Identical**: dyld shared cache (the same MISValidateSignature patch), MBXGLEngine shim and `.stock`,
  SpringBoard and installd jobs, libappsync, it_typein, sblaunch, sbdlicon, kernelcache, fstab, markers' content.
- **it_agent**: 71264 vs 70832 bytes, rebuilt from this tree.
- **Owners/modes of the shell package** (~380 rows): root:0 here, 99:99 there. Tar modes here (setuid `su`,
  `login`, `passwd`; 0555 libraries), where the old install had run `chmod 755`/`644` over everything.
- **Stock modes restored**: `launchctl`, `configd`, `launchproxy`, `notifyd`, `nvram`, `pppd`, `racoon(ctl)`,
  `scutil`, `syslogd` are 0555 as in the IPSW, and `scselect` keeps its setuid bit. The old install-ssh.sh's
  `chmod 755 /bin/* /usr/sbin/* ...` had clobbered them.
- **sshd**: its own `/Library/LaunchDaemons/com.openssh.sshd.plist` here; there, the stock
  ReportCrash.SafetyNet plist was overwritten with the sshd job (stock here). Host keys are generated per device.
- **Preferences**: here only the sound defaults, mode 0644; there, what SpringBoard wrote at run time (icon
  state, SB* keys, 0600) plus a New York time zone (the app sets the time zone over lockdown).
- **Only in nand-current.new** (148): run-time state from its boots (`/var/run` sockets and pids, logs,
  CrashReporter, caches, icon caches, keychain/TrustStore, SMS/Calendar/... databases, mobile installation
  cache). Also the Lockdown activation record, device keys and pair records, and the retired `it_pbd`.
- **Only here**: `/private/var/run/.scratch` (stock; removed at run time there), the sshd job.

## Regression (emulator change: `gid-blobs=`, hw/arm/ipod_touch_aes.c)

- iPod, shipping 3.1.3 image (nand-current), one run per group: boot, fsck, persist, appinstall, applaunch,
  agent and audio PASS. gles FAILs with `305 (glTexParameteriv), 817 (glDrawTexfOES)`: nand-current still ships
  the old shim, and that failure is documented and reproducible in nand-current-new-verification.md §3. With
  `--stage-gles-shim` it PASSes (magenta 0.141, cyan 0.281, yellow 0.141).
- iPad `tests/ipad1/fresh-device.sh manifests/ipad1-7B500.json` (through the refactored builder), before and after
  the change: lock PASS, no FTL rescan, clean power-off in 14.4 s. The boot step fails on "screen lit: never"
  because without an activation hook the device sits at "Connect to iTunes" and cannot be unlocked
  (docs/research/userland-boot.md: "Without an activation hook the device stops at Connect to iTunes").

## Boot-chain fidelity verification (2026-09-28)

The old DeviceTree injection thunk is removed. Its comment described a failure
before `build_nor.py` wrapped NOR SHSH signatures with the emulated UID-derived
key. No PKE or AES behavior change is needed for the generated 7E18 NOR.

Verified over the gdbstub using stock `7E18-a/iBoot.bin` and `nor.bin`, with
all inherited `IT_*` variables removed, no injection and no signature forging:
`image_load` at the DeviceTree call returns r0=0; iBoot's output globals contain
address 0x0bf00000 and length 0x894c (35,148 bytes). The call-site bytes remain
`0af057fa002803da002323602b600ce0`. These addresses are diagnostic observations
for this exact build, not emulator constants. The NOR builder's UID wrapping is
the prerequisite; raw IPSW SHSH bytes in flash are not the restored NOR format.

Validation after removal: `scripts/ccninja -C build qemu-system-arm` succeeds;
all eight default regression checks pass across `/private/tmp/ipod-bootchain-regress-dt`
and `...-dt-apps` (the second run supplies initially missing guest fixtures).
GLES uses `--stage-gles-shim` for the shipping NAND's older shim.

The S5L UART acknowledgment mode now applies to every boot strategy. Previously
it was selected only for direct iBoot; SecureROM boots used Exynos acknowledgment
semantics and 2.1.1 spun in AppleS5L8900XSerial's ISR. Before/after gdb samples
move from that handler to the CPU idle loop. All eight 7E18 regression checks
pass after the UART change (`/private/tmp/ipod-bootchain-regress-uart`) and after
command-line discovery (`/private/tmp/ipod-bootchain-regress-args`). The discovered
5F138 command-line buffer is 0x0ff2a584, matching the traced iBoot literal.
Finder tests exercise relocation, ambiguity, truncation and address bounds.

### 2.1.1 userland and MBX result

The remaining logo stall was caused by injecting a modern helper into
SpringBoard: 2.x dyld rejects `LC_DYLD_INFO_ONLY` (0x80000022). The same error
appears repeatedly in `/var/log/it_agent.log`. Omitting the helper injection
lets stock SpringBoard draw the activation screen. MBX initializes successfully;
no new GPU completion workaround was necessary.

The MBX register-read side effects that rewrote the USB function gate and
BCM4325 kernel code are removed. `usb-patch-mux-gate=on` now fails explicitly;
`off` remains accepted. The retired migration byte remains reserved, preserving
the v1 stream layout. Native snapshot round-trip and all eight 7E18 regression
checks pass (`/private/tmp/ipod-bootchain-regress-mbx`). A 5F138 gdb check confirms
the formerly overwritten BCM4325 instructions match the stock kernelcache.

Fresh manifest builds of 5F138 and 7E18 both succeed. 2.x omits the incompatible
helper binaries, launch job, injection and capability markers; the preparation
report records this limitation. Software CoreAnimation and stock MBXGLEngine
remain enabled. 7E18 keeps its existing helper integration. Baked-component
checks validate both branches, including ownership and launch settings.

The fresh 5F138 framebuffer settles at the stock Connect to iTunes screen by
15 seconds and is unchanged at 20, 25 and 30 seconds. Screenshot:
`/private/tmp/ipod-bootchain-5f138/fresh-30.png`; PPM SHA256
`2dcf07734d263e220243be8c49925c1c20aeb1cbcbce54868924f3a40d8d4278`.
This proves UI boot, not activation or a passing home-screen regression tier.
7E18 still uses direct iBoot; SecureROM boot for every firmware, 4.2.1 NAND
identification, and DFU/restore are not claimed by this work.

The final iBoot cleanup also removes the unused logo-injection thunk. On a
fresh 7E18 manifest build, gdb observes both the stock logo and DeviceTree
`image_load` calls returning 0 with `forge-sigcheck` disabled and the generated
GID table supplied. Early argument injection now scans the loaded image for a
unique verified handoff and decodes its literal; no code or empty-string address
is fixed in the machine. Its tests cover relocated and ambiguous handoffs.

Final acceptance: all eight default 7E18 regression checks PASS, including\nclean shutdown/reboot persistence and fsck, in\n`/private/tmp/ipod-bootchain-regress-final` (252 seconds). The working branch is\n`ipod-bootchain`; changes are intentionally not merged into `ipad1` or `main`.\nFMSS edits are confined to discovering/resetting the boot-argument data buffer;\nNAND identification, geometry and controller behavior are untouched.

### 2.1.1 with the activation hook (2026-09-28)

`firmwarekit create --catalog CATALOG --id n72ap-5F138 --ipsw IPSW --out OUT` builds (hook applied, daemon re-signed, aes-uid=engine). The
boot does not reach home: the screen stays at Connect to iTunes (lit about 35600), and the kernel panics
after a run of `AppleCS42L58Audio: I2C register read/write failed ... device error` lines: `kernel abort type
4` (write translation fault, fsr 0x808) at pc 0xc05f6eac, lr 0xc05f6abc, in a mediaserverd thread, then
`Debugger message: Fatal Exception` and a wait for KDP. The same image under aes-uid=legacy panics at the
same pc, so the AES mode is not the cause. Not investigated further.

Root cause (f2ec3285e4): `audio-hw=auto` added the CS42L58, amp, I2S0 and AMC only for direct iBoot, so the
SecureROM 5F138 boot had none of them. The codec NAKed at 0x4A (the I2C trace shows IICSTAT 0xd1). The panic
came from the same missing hardware and was not a codec error path. pc 0xc05f6eac is
`AppleS5L8900XI2SController`'s register write (`str r2, [r3, r1]`, base `[this+0x78]` = 0xea5dd000, the
i2s0 mapping it logged at start). lr 0xc05f6abc is its configure routine, writing register 0 with
`config[0] | 1`. The write reached an unmapped 0x3CA00000 and took an external abort (fsr 0x808, FS=0b01000;
not a translation fault). It fires when USB power reaches 500 mA and mediaserverd plays the charging sound.
`auto` now means present. With the parts present, 5F138 sends the same first 40 codec accesses as 7E18 (chip
ID 0xe0 at register 01, then 00/33/32 power-up, 03-0f, 1a-1d); later accesses differ only in volume values.
Two boots of the hook build and one without the hook: no I2C failures, no panic, and the same Connect to
iTunes frame at 300 s (charging icon, PPM SHA256 `511cc57f…f562` in all three).

### 2.1.1 home screen: lockdownd's brick state (2026-09-28)

With the updated activation hook (`strategy: ipod-no-record-initializer`), lockdownd reports Activated and
SpringBoard logs `lockdown says the device is: [Activated], state is 3`, but the screen still shows the
Connect to iTunes view. That view is SpringBoard's own `Activate.png`, composited into its three scanout
buffers with the status bar above it. It is not stale framebuffer contents, and MBX and the LCD are fine.
Under gdb, `-[SBAwayView updateInterface]` gets YES from `-[SBLockdownManager brickedDevice]`, and
`-[SBAwayView setLockoutUIVisible:mode:]` creates `SBActivationView` with mode 1.

`brickedDevice` is lockdown's `BrickState`. On first boot lockdownd sets it (`_set_brick_state: Enabling the
brick state`). `determine_activation_state` clears it only when `is_phone` (DeviceClass == iPhone) is true.
On an iPod, only `toggle_brick_state` clears it, and only when a paired host sets `TimeIntervalSince1970`
or `iTunesHasConnected` (the `verify_set` path of lockdownd's set_value handler). iTunes does this on
connect. This is host-protocol behavior, so the emulator needs no change.

Measured: `idevicepair pair` then `idevicedate -c` logs `toggle_brick_state: Disabling the brick state (time
interval)`. On the same boot, a home press and unlock swipe reach the home screen (first-run Edit Home Screen
tip; Dismiss works). The cleared state persists: the next boot goes lock screen -> home with no host action.
The host has to set the time once per fresh device, for example next to the app's existing TimeZone set
over lockdown.

`regress.py` does this for a 2.x device (lock `product_version` 2.x): while the boot frame stays dark it
pairs and runs `idevicedate -c` once, then wakes from sleep (power, home, unlock swipe). SpringBoard
re-reads the state only at a wake; a home press on the Connect to iTunes view does nothing.

### 2.x: CoreAnimation through the GL front end (2026-09-29)

1.x/2.x have no GL engine bundle: `OpenGLES.framework/OpenGLES` is the IMG driver itself. The guest
package's `n72-ios2` family replaces that binary with `contrib/it-gles/gles2x.c` (the mbxshim core under the
firmware's own 218 export names, `opengles-2x.exports`), and the bake sets `CA_ENABLE_OGL=1` (MBX 2D and auto
off) only when the stock binary's exports match the list (`ipod2g_device.gles2x_front_end`; a package
without the hook is refused at seed time rather than left to drive the MBX). QuartzCore 2.1.1's display
renderer (`gles_context`) then composites every frame through the host: 11 egl calls and 44 gl* (its whole
OpenGLES import list), pixmap surfaces over its CoreSurface buffers, no framebuffer objects.

What it took, each found on the device:

- **Link against 2.x libraries by name.** `-undefined dynamic_lookup` left NSObject, the constant-string
  class and the messengers flat, and a bare dlopen failed "Symbol not found". `build-gles2x.sh` links text
  stubs naming just those (NSObject and `__CFConstantStringClassReference` in CoreFoundation, the
  messengers in libobjc, as in the 2.0 SDK) and loads Foundation as the stock binary does.
- **r9 is the thread pointer on 2.x.** 2.x libSystem's `pthread_getspecific` is `add r0, r9, r0, lsl #2;
  ldr r0, [r0, #0x48]`; 3.0 made r9 an ordinary register. Code built without `-ffixed-r9` hung in
  `pthread_once` and read garbage TSD. `armv6.sh` now passes `-ffixed-r9` under `LEGACY_LINK=1` (every
  legacy-linked binary, it_boot's loader included).
- **The two extensions.** QuartzCore refuses GL ("unsupported graphics hardware; need APPLE_texture_rectangle
  extension; need APPLE_core_surface_texture extension") without them in `GL_EXTENSIONS`; both are real
  here (`glTexImageCoreSurfaceAPPLE` is the core's BindCoreSurface; the host samples rectangle textures).
- **Map the display buffers.** CA renders into PurpleGfxMem CoreSurfaces, which have no client mapping
  until a lock with flags 2 (QuartzCore's own CPU lock); the core's lock takes 2 on CoreSurface.
- **Window order.** An EGL pixmap's first memory row is the top of the picture (GL's last), so
  `GLES_SURFACE_WINDOW_ORDER` in the bind target makes the host reverse rows on upload and write-back.
  Without it the home screen came out upside down.
- **Frame end.** 2.x CA brackets each frame with `eglMakeCurrent(buffer)` ... `eglMakeCurrent(none)` and
  swaps right after; it never calls glFlush (the MBX driver's swap token waits for the GPU). The front end
  writes the rendered buffer back at that second eglMakeCurrent. Without it an animation's last frame
  never reached the panel (a Safari close zoom stuck with its icons half way).
- **Surfaces die with no context current.** CA's `finalize_surface` destroys its egl surfaces with nothing
  current; the front end now deletes the texture and framebuffer through the surface's own GC. Left alive,
  the host surface stayed dirty over the buffer CA frees next, and a later flush wrote into unmapped pages
  (SpringBoard SIGSEGV in `glFinishTextureAPPLE`). The host now also unbinds a deleted bound framebuffer, as
  GL does (the next bind asked the default framebuffer for COLOR_ATTACHMENT0: INVALID_ENUM).

Measured on 5F138 (`IT_LCD_FRAMETRACE` presents in each gesture's window, three passes each, same host
minutes; the host had other emulators running, load 9-15 on 16 cores). "cores" is QEMU's host CPU over the
window.

| gesture | software CA (fps, max gap, cores) | GL front end (fps, max gap, cores) |
|---|---|---|
| home page swipe left (one page: rubber-band) | 30-37, 45-64 ms, 0.06-0.07 | 31-33, 60-62 ms, 0.06-0.08 |
| home page swipe right | 45-49, 52-67 ms, 0.05-0.07 | 46-53, 44-65 ms, 0.07-0.08 |
| slow drag (40 steps over 2 s) | 28-29, 417-433 ms, 0.09-0.11 | 29-30, 395-414 ms, 0.13-0.15 |
| Safari launch zoom | 40-41, 149-183 ms, 0.37-0.39 | 40-41, 166-171 ms, 0.27-0.31 |
| Safari bookmark-list scroll | 34-35, 116-118 ms, 0.16 | 35, 116-117 ms, 0.13-0.14 |
| Safari close zoom | 57-59, 33-37 ms, 0.18-0.20 | 57-60, 21-35 ms, 0.15-0.16 |

Neither path is CPU-bound on this host: frame pacing follows the gesture input (the script's 20-50 ms
touch steps) and vsync, and the max gaps are the gesture's own (the 400 ms is the drag's touch-down delay;
the 170 ms is Safari's process launch), the same in both. GL takes about a quarter less host CPU on the
launch zoom and a fifth less on the close and the list scroll (full-screen rasterization leaves the
emulated CPU), and a third more on the slow drag (a small damaged region is cheap to rasterize in
software; GL pays the per-frame write-back). `gles-rejects` stayed empty, and the frames (home, zooms
mid-flight, bookmarks) are correct.

Gates: `regress.py --device <5F138> --checks boot,gles` PASS (the 2.x leg: one front-end hello, CA's pixmap
surfaces, a live host context, no refusals, the frames following a swipe, a Safari launch and a close);
7E18 (nand-current with this tree's MBXGLEngine staged offline: `--stage-gles-shim` cannot put a 355 KB
engine through the agent's 256 KB request, true at bd8d6b1363 too) and a fresh 8C148 device: boot,gles PASS.

Not done: an App Store 2.x game through EAGL (waits on 2.x app installs); planar-YUV video layers (the LCD
plane path, unchanged); 2.2.1 (does not boot yet).

### 1.x (the 1G): LayerKit through the GL front end (2026-09-29)

Read from the 4B1 root filesystem (iPod1,1 1.1.5; LayerKit and OpenGLES only, nothing kept). LayerKit's
GLES renderer (`LKRenderGLESRenderDisplay`, `LKRenderOGL.c`) imports 54 names from OpenGLES:

- egl (9): `eglGetDisplay eglInitialize eglChooseConfig eglCreateContext eglCreatePixmapSurface
  eglMakeCurrent eglDestroySurface eglGetError eglTerminate`
- gl (45): `glActiveTexture glBindTexture glBlendFunc glClear glClearColor glClientActiveTexture glColor4f
  glColor4ub glColorMask glColorPointer glCullFace glDeleteTextures glDepthMask glDisable
  glDisableClientState glDrawArrays glDrawElements glEnable glEnableClientState glFinish
  glFinishTextureAPPLE glFlush glFrontFace glGenTextures glGetError glGetFloatv glGetIntegerv
  glLoadIdentity glLoadMatrixf glMatrixMode glOrthof glPopMatrix glPushMatrix glRotatef glScalef glScissor
  glStencilMask glTexCoordPointer glTexEnvfv glTexEnvi glTexImageCoreSurfaceAPPLE glTexParameteri
  glTranslatef glVertexPointer glViewport`

Against 2.x QuartzCore's set: LayerKit adds `eglTerminate glColorMask glDepthMask glStencilMask` and lacks
`eglDestroyContext eglEnableInternalSurface eglGetConfigAttrib glGetString glTexImage2D` (so no extension
check). All 54 are implemented by the front end (egl in gles2x.c, gl by gles-names.h row). Its errors
("OpenGLES bad display / can't init / can't make config / can't make context") are the same egl sequence
as 2.x's pixmap renderer.

3A101a (1.1, devos50's public n45ap set, the build the 1G boots) has the same LayerKit imports and the same
186 OpenGLES exports. `OpenGLES-1x` is `contrib/it-gles/gles2x.c` built by `build-gles2x.sh 1x`: the same
core under 1.x's names (`opengles-1x.exports`), `GLES2X_EAGL=0` (1.x's Objective-C is the old ABI:
CoreFoundation exports `.objc_class_name_NSObject`, libobjc has no `objc_msgSendSuper2`, so the EAGL half is
compiled out and the binary is plain C over libSystem, no ldid), `eglSwapNotification` as the egl form of
2.x's no-op `-swapNotification:` and `glVertexAttribPointerARB` as a counted refusal (nothing on the device
imports either). The guest package's `n45-ios1` family carries it as the OpenGLES hook, and
`firmwarekit create --catalog CATALOG --id n45ap-3A101a --ipsw IPSW --out OUT` bakes a device: the hook when the stock exports match the
list, and SpringBoard's job gets `LK_ENABLE_OGL=1 LK_AUTO_ENABLE_OGL=0 LK_ENABLE_MBX2D=0`. LayerKit then
composites the home screen, app zooms and scrolls through the host: its pixmap renderer, as 2.x's CA, and it
ends every frame the same way (`gles_make_buffer_current(0)` is `eglMakeCurrent(dpy, 0, 0, 0)`, after
`glFlush`), so the 2.x write-back at the frame end serves it unchanged.

What it took, each found on the device:

- **The 1G had no guest services.** `ipod_touch_1g.c` now has the QEMU_CALL cp15 register (the GL bridge,
  the ping, guest-package delivery; no agent yet) and the gles-rejects/-contexts/-debug properties.
- **CoreSurface is a public framework on 1.x**, and the core's `dlopen` of the private path succeeded anyway
  (dyld's framework fallback path has `/System/Library/Frameworks`), so 1.x got 2.x's lock flag 2: a NULL
  dereference in 3A101a's IOCoreSurface (kernel panic at SpringBoard's first `glTexImageCoreSurfaceAPPLE`).
  The public path goes first now, and 1.x locks with 3, as its own driver does for textures and pixmaps
  (1 leaves a LayerKit image, `CoreSurfaceBufferWrapClientImage`, unmapped).
- **The MBX was an id stub (S).** It read 0x12c without the idle bit 0x40, and the first swap AppleMBX was
  asked to order (LayerKit ties every GL frame's swap to the GPU with `mbx2DSwapNotification`) spun on it
  forever. The 1G now has the 2G's `ipodtouch.mbx` at 0x3B000000 on the device tree's interrupt 0xC, and the
  model gained two registers 1.x's driver uses: the mask at 0x130 reads back (the driver re-arms it
  read-modify-write), and a write to 0x12c sets status bits, the software interrupt its ISR runs the command
  queue from (3A101a c03aaa18). Before that, the fourth swap waited forever.
- **The LCD lost GL frames.** The bridge's surface tracking (surfaces by their pages) clears QEMU's VGA dirty
  bits of every surface page it checks, its own write-backs included, and the iPod LCD converts only dirty
  lines: the panel kept showing the frame from before the last gesture. The bridge now keeps the newest write
  generation per page (`gles_host_ram_gen`) and the LCD redraws when the scanout's advances. This is the 2G's
  LCD too.
- **The loader on 1.x.** The legacy-linked `it_boot` died with a bus error under 1.x launchd: a NULL
  dereference in `snprintf` (in `__PAGEZERO`, so SIGBUS), because 1.x libSystem does not initialize
  itself. Its crt1 (10.4's, as in `/bin/launchctl`) sets `NXArgc`/`NXArgv`/`environ`/`__progname` in the
  executable and calls `*mach_init_routine` and `*_cthread_init_routine`; 2.0+'s crt1 only calls `main`,
  and LC_UNIXTHREAD entered `main` directly. Past that, 1.x's `stat` and `readdir` are the 32-bit-inode
  ones while every SDK here is `__DARWIN_ONLY_64_BIT_INO_T` (`st_size` read 0, so every package looked
  torn), and 1.x leaves `kern.osversion` empty. The toolchain now gives every LEGACY_LINK executable
  `crt1old.c` (that crt1, the two hooks called only where non-NULL) and force-includes `legacy.h`
  (`stat`/`lstat`/`fstat` through 1.x's `stat64` family, `readdir` converted, both only where libSystem
  exports `stat64`, which only 1.x does); `it_boot` reads the build from SystemVersion.plist when the
  sysctl is empty. The same binary runs on 1.x, 2.x and 3.x, and `n45-ios1` has its loader like every
  family.

Measured on 3A101a, `IT_LCD_FRAMETRACE` presents (a present writes the scanout base twice, 0 then the
buffer; the non-zero writes are counted) in each gesture's window, three passes each, same host minutes (load
2-5 on 16 cores). "cores" is QEMU's host CPU over the window. Settings is the app (the one with a list; 1.1's
home screen has one page, so no page swipe).

| gesture | software LayerKit (fps, max gap, cores) | GL front end (fps, max gap, cores) |
|---|---|---|
| Settings launch zoom | 36-43, 116-126 ms, 0.13-0.18 | 35-43, 118-148 ms, 0.10-0.14 |
| Settings list drag and bounce | 34-36, 99-118 ms, 0.09-0.10 | 25-35, 117-283 ms, 0.08 |
| Settings close zoom | 45-60, 19-249 ms, 0.07-0.09 | 58-60, 19-37 ms, 0.07-0.09 |

As on 2.x neither path is CPU-bound: the zooms pace at the display (the close runs at the panel's 60), the
drag at the touch steps, and the long gaps are the gesture's own (the launch's is Settings' process start);
GL costs about a quarter less host CPU on the launch zoom and a little less on the drag, the same on the
close. `gles-rejects` stayed empty; the home screen, the Settings list mid-drag and the home screen after the
close are correct frames.

Gates: `regress.py --device <prepared 3A101a> --checks boot,gles` PASS (the 1G leg: the same fixture as
2.x's, lit threshold 100k for 1.1's black home screen, and the close must bring the home screen back); with
the LCD change reverted the same leg fails (Safari never reaches the panel).

Not done: apps' own GL (the App Store starts at 2.x); a 1.x build other than 3A101a (the list and the hook are
per major, `3*`/`4*`).

### 3.0: GL through the legacy-linked engine (2026-09-29)

3.0 (7A341) is not a 2.x-style firmware for GL: it already has 3.1's split, a stock OpenGLES front
(EAGL, `__GLIFunctionDispatchRec` @encode with 821 slots, all named by gles-names.h; 298 exports) over
`OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine`, which exports `GLESGetEGLInterface` like 3.1.3's. What
it lacks is 3.1's dyld: no shared cache (the engine is a plain file on the volume) and no
LC_DYLD_INFO_ONLY. So the fix is the engine, not a front end: `contrib/it-gles/build.sh` also builds
`MBXGLEngine-30`, mbxshim.c unchanged under `LEGACY_LINK=1` (classic relocations, r9 reserved), and
`ipod2g_device.gli_engine` picks it when the volume has the engine bundle but no cache. OpenGLES, EAGL and
QuartzCore stay stock. The guest package has a family for it, `n72-ios30` (builds `7A341`: the legacy
loader and the engine hook, no helpers), so `n72-ios3` lists the 3.1.x builds by id (the iPod's 3.x
series is closed); family by dyld capability, not by major.

What it took, found on the device:

- **Sign it.** An unsigned engine hung every boot on the Apple logo with the boot spinner: 3.0's
  SpringBoard creates two GL contexts at every boot, CA_ENABLE_OGL=0 included, so the engine maps into a
  signed process, which is killed at the unsigned library's first page, and launchd respawns it forever.
  Signed (`ldid -S`, as MBXGLEngine), the same image reaches the home screen. mkpkg now refuses an
  unsigned MBXGLEngine hook (`macho_problem(signed=True)`), and tests/ipod/test_gli_engine.py checks the
  built file.
- **The brick state is 2.x's.** A fresh 3.0 shows "Connect to iTunes" until a paired host sets the time,
  at every boot; `regress.py` now does the 2.x time set below 3.1 (`device_version`), not only on 2.x.

SpringBoard's GL on 3.0 is then 3.1.3's, the same log line for line on a boot (7E18-a and 7A341: one
`GLESGetEGLInterface`, two `GLESCreateGC`, a dozen offscreen draws): the display itself composites as on
3.1.3. An app's EAGL goes through the engine: Labyrinth 2 Lite (Legacy Store ipa 257098, min OS 3.0,
installed with the installd AppSync, launched by tapping its icon) renders its menu and the 3D maze
(`GLESBindView`, `present tally: ok=3600 failed=0`), where the stock engine left its window blank. One
host refusal in the app, `matrix-stack:glPushMatrix`, not SpringBoard's.

Gates: `regress.py --device <7A341> --checks boot,gles` PASS 3 of 3 (the 2.x leg's criteria with the
engine's marker, since 3.0 has no agent to launch GLTest: one hello, SpringBoard's GL contexts, live
host contexts, no refusals, the frames following a swipe, a Safari launch and the close); the second
boot after a guest-confirmed shutdown lit in 9 of 9 (6 `boot,persist`, 3 with Labyrinth run in boot 1).

Not done: helpers on 3.0 (it_agent and friends are still modern-linked, smoke #10), so no GLTest leg.

### Early AppSync (2026-09-29)

`contrib/appsync` now builds the armv6 dylib with classic loader metadata and r9 reserved.
2.x uses the built-in appsync-launch argument wrapper because Lockbot ignores the service
environment dictionary; 3.0 uses its installd job. The standalone system libmis remains stock.
Fresh 5F138 and 7A341 images install and visibly launch an ad-hoc UIKit test app; the
AppSync-off 5F138 control rejects it. See `contrib/appsync/README.md` for details.

### FMSS physical-state snapshots (2026-10-01)

VMState version 5 serializes the physical-page cache and erase map using QEMU's
standard tree serializer, including generated pages with no disk destination.
Version 4's certified empty streams remain accepted; uncertified older streams
and mismatched startup storage modes remain refused. Four actual FMSS board
qtests pass. This supersedes September 30's nonempty-state save refusal;
see [physical store policy](../research/nand-physical-store.md) for the remaining
FTL and exact-storage-generation boundaries. Live native 7E18 save/resume also passes guest files, clock, USB pairing,
continued GL presentation, new Wi-Fi HTTP requests and active stereo playback.
The final default regression passes 8/8. These probes do not certify every
firmware or preservation of already-open host network sockets.

### Common legacy helpers (2026-10-01)

Guest serial 13 exports one legacy-linked armv6 core helper set for 2.x, 3.0,
3.1.x and 4.x. The injected typing dylib is now signed by its build recipe;
unsigned copies killed SpringBoard on 2.x and left 3.0 at the Apple logo. The
agent and sblaunch select stock exported launch APIs, including 2.x's older
SBLaunchApplication. Fresh 2.1.1, 3.0, 3.1.3 and 4.2.1 tests pass **18/18 each**:
actual installed-app foreground launch, AFC file and app persistence across
two cold boots, generated identity, automatic activation, Home and PMU power-off.
This supersedes the historical “no helpers yet” notes above; text injection,
clipboard and older media services still require their own API tests. See
[qualification and evidence](../research/legacy-helper-abi.md).

### Bluetooth identity without an iBoot rewrite (2026-10-01)

The board no longer changes iBoot's `arm-io/uart3/bluetooth` string to uart1.
The modeled BCM4325 combo-chip OTP/vendor CIS and HCI identity supply the
prepared device's addresses through stock drivers. Real app-helper runs pass
18/18 on 2.1.1, 3.0, 3.1.3 and 4.2.1 with untouched iBoot code and literals:
serial/UDID/Wi-Fi/Bluetooth identity on both boots, automatic activation, Home,
AFC writes, installed-app foreground launch, cold persistence and power-off.
Evidence: `/private/tmp/ltm-n72-{211,30,313,421}-no-btpatch-session`.
The legacy normal-boot command-line data injection remains a separate
provisioning boundary; this change does not certify stock NVRAM propagation.

The companion chip-presence fix keeps BCM4325 enumerated when `wifi=off`;
that property only controls the optional host data bridge and is startup-only.
Four SDIO board qtests pass, including CMD5 enumeration, OTP identity and reset
with networking disabled. Native save/resume with no host network backend
passes serial/UDID/Wi-Fi/Bluetooth identity both before and after restore,
clock, USB pairing and guest file state on all four versions; 7E18 additionally
keeps its live GL scene presenting. The updated default regression passes 8/8
and now pairs with the expected UDID instead of a hardware-absence-derived one.
Evidence: `/private/tmp/ltm-n72-physical-combo-final-qtest.log`,
`/private/tmp/ltm-n72-{211,30,313,421}-physical-combo-snapshot`, and
`/private/tmp/ltm-n72-physical-combo-default-regress.log`.

## N72 unit ECID and stock DFU qualification (2026-10-01)

The machine accepts `ecid=0x...` as an immutable 42-bit board input. It encodes
that value in CHIPID word3 and the ECID bits of word4, preserving unrelated
fuses. Guest writes cannot change it; reset preserves it. Without this option,
existing model defaults and explicit fuse properties retain their behavior.
Light Touch supplies the per-unit, seed-derived ECID for new and legacy N72
bases; the emulator does not fabricate a USB descriptor or alter guest memory.

`tests/ipod/dfu-ibss.py` uses an all-erased private NOR and a private NAND
overlay to query stock SecureROM, upload an unmodified stock iBSS, and verify
the same nonzero ECID in both guest-generated USB descriptors. It requires the
emulator-only libirecovery transport adapter and preserves source storage.
The 5F138 qualification used ECID `0x98e452f953`. This is identity/DFU/iBSS
proof, not full restore proof. The test explicitly allows firmware USB
reinitialization to settle: immediate descriptor polling can return to DFU.
That transport/controller race remains open and must not be hidden by a fake
recovery event. Production-board CHIPID qtests pass 3/3 and the default native
7E18 regression passes 8/8 after this change.

An early probe used a private host-only copy of the 2.1.1 Restore.plist with
`SupportedProductTypes = [ProductType]` gets the existing idevicerestore past
its legacy metadata check. No guest firmware bytes were changed. One traced
run reached stock iBSS and uploaded the restore ramdisk, then failed before
DeviceTree upload. Current restore-client recovery commands also receive USB
stalls from 2.x iBSS. Neither stock restore nor physical NAND replacement is
qualified by these experiments.

### Restore-sized AES DMA (2026-10-01)

Unmodified 5F138 iBSS creates a 25,313,280-byte update ramdisk. The former
16 MiB AES INSIZE clamp decrypted only its prefix. The remaining ciphertext
included `/sbin/launchd`, causing the stock restore kernel to panic with
`Process 1 exec of /sbin/launchd failed, errno 8`. Physical RAM was present;
this was a crypto transfer-length error, not a RAM-map hole.

INSIZE now retains its 32-bit register value. Custom, legacy UID and N45
compatibility CBC DMA use at most 64 KiB of host scratch storage, preserving
CBC chaining and a partial final block. GID lookup/output also uses bounded
storage. Sanitized production-handler tests compare complete restore-sized
requests against independent CBC, including segmented DMA and legacy UID.
The default native 7E18 regression passes 8/8 on two cold boots.

A guest physical-memory read after stock iBSS decryption matches every byte
of the catalog-decrypted update ramdisk (SHA-256
`05795d76755420f7b5e60cfe0ff777dbc409f6c28d950efc85d1a70d98b6d6b6`).
The previous launchd panic no longer appears in the immediate diagnostic
capture. The first stock idevicerestore attempt then stalled on kernel USB enumeration;
full restore, physical NAND formatting and restored cold boot remain open.
The known DFU reconnection race still requires separate investigation.
Evidence: `/private/tmp/ltm-aes-restore-default`,
`/private/tmp/ltm-n72-ramdisk-aes-fixed`, and
`/private/tmp/ltm-n72-stock-restored-aes-settled`.

The SHA engine had the same register clamp. A production-handler regression
first reproduced truncated INSIZE readback, then verified a complete
restore-sized, guest-padded message against Python hashlib. SHA DMA now uses
64 KiB scratch chunks without truncating the register; continuation across
jobs and the raw engine's ignored partial block remain correct. Interrupt and
snapshot ownership tests pass, as does the separate default 7E18 native 8/8 run
(`/private/tmp/ltm-sha1-restore-default`). The new test is registered in the
explicit gate inventory; the earlier guest cache-input test's missing
registration was also repaired and its four tests pass.

A later unpatched stock 5F138 run, without watchdog suppression, has guest
processes `launchd` (PID 1) and `restored_update` (PIDs 11/12) after 60 seconds.
Names/PIDs were read from the kernel process list using offsets independently
confirmed in its proc accessor instructions. No restore-service protocol
response or physical restore completion is implied by process presence.
The corresponding trace is `/private/tmp/ltm-n72-kernel-processes`.

### Stock restore protocol and USB ownership (2026-10-01)

A real host bus reset/re-enumeration makes the stock 5F138 kernel expose PID
0x1293. `restored` replies over the actual mux interface with protocol 11,
HardwareModel N72AP, BoardID 0 and ChipID 0x8720. This old daemon does not
report UniqueChipID. Modern upstream idevicerestore consequently ignores it;
LukeZGD's existing legacy fork already supports that pre-iOS 3 response.

The private fork probe uses upstream `9e6eacc788d532b887b9b0883477d6b89c5a2841`
plus isolated fix `26314aa`: free the legacy ProductType value after both
comparisons, avoiding its measured use-after-free/double-free. The actual
compatibility function fails ASan before the fix and passes match, mismatch
and iPod1 cases with ASan/UBSan afterward. No tool was installed. The original
stock extracted IPSW directory now works without the SupportedProductTypes
workaround. Boot-only restore exits successfully with "Device is now in
restore mode" (`/private/tmp/ltm-n72-legacy-restored-stock`).

The reusable TCP USB bridge probes descriptors separately from configuration.
When the guest's address returns to zero, it uses real reset/enumeration bus
events. It hands the kernel connection to usbmuxd before selecting a recovery
configuration, under the same lock as requests; subsequent requests cannot
consume the relay's replies. Five registered ownership/enumeration tests pass.
The production bridge then reaches stock erase `restored`, which repeatedly
reports "Waiting for NAND (28)" on disposable empty page directories with
FMSS_PHYSICAL and FMSS_ERASE enabled. This is not an erase-restore pass.

Both full probes still use a research-only wait for the actual DFU manifest
bwPollTimeout. Rapid DFU polling remains unresolved; no delay was added to the
production bridge. Empty N72 pages still use synthetic clean-marker bytes;
physical flash formatting, restored cold boot and durable subsequent writes
are required before removing generated FTL relocation. Evidence:
`/private/tmp/ltm-n72-legacy-erase-physical-probe2`,
`/private/tmp/ltm-usb-reenumeration-unit.log`, and
`/private/tmp/ltm-usb-reenumeration-registry.log`.

### Measured stock FMSS sequencer parameter (2026-10-01)

The stock erase ramdisk's CPU writes D4C = 0x20011000. Its real reset/read
scripts read that parameter, OR their command bits and write FMCTRL0. The
model discarded the CPU write and stopped those scripts at the unsupported
D4C read. D4C now latches and reads back through MMIO and the sequencer;
reset clears it, FMSS VMState6 preserves it, and supported older streams
initialize it to zero. No guest address appears in the hardware model.

Sanitized actual-handler/script tests pass, including the stock parameter
sequence; real FMSS qtests pass 4/4 with reset and physical/generated snapshot
round trips. The IRQ fixture was stale against existing v5 state and now uses
its actual GLib validation helpers. The separate default native 7E18 two-boot
regression passes 8/8 (`/private/tmp/ltm-fmss-d4c-default`).

Full stock-script replay identifies subsequent unsupported D18/D28 reads and
opcode03 descriptor loads. Aborted programs still receive synthetic success
completion, and physical erase execution remains absent. This parameter
correction is not restore completion. The stock kernel also gates virgin NAND
formatting on nand-enable-reformat; actual kernel BootArgs must be read before
changing the legacy host client's restore argument policy.

### PHY reset during stock DFU handoff (2026-10-01)

Rapid stock 5F138 DFU handoff exposed a real hardware-boundary error: after
SecureROM asserted ORSTCON bit 0 and freed its USB queue, host SETUP packets
still performed DMA and raised interrupts. The ROM consequently followed an
invalid queue pointer and reset after a data abort. The PHY now drives a
physical-reset signal into the N45/N72 USB controller. Asserted reset rejects
bus transactions before DMA or interrupt generation, without clearing core
registers or already latched interrupts. Deassertion resumes traffic; migration
restores the signal from ORSTCON. No guest patch or settling delay is involved.

The final qtest suite passes 3/3, including N72 migration and N45 traffic
gating; the pre-fix binary fails the new gating assertion. Three rapid stock
DFU handoffs and native 5F138 boot/USB checks pass. The separate default 7E18
two-boot regression passes 8/8. Stock SecureROM through the restore ramdisk
also passes using the production bridge with no diagnostic settling wrapper
(`/private/tmp/ltm-n72-stock-no-delay-timeout1`). Descriptor polling retains
its one-second deadline so host disconnect notification arrives within the
stock client's timeout. This qualifies ramdisk handoff, not flash restore.

N45 whole-board migration separately fails because an inactive I2S stream
without host output saves a zero host voice rate that its loader rejects.
That failure remains separate; N45 migration and native host USB are not
claimed by this gate. K48 is not wired to the new PHY signal.

Actual stock erase-kernel BootArgs were captured as
`rd=md0 nand-enable-reformat=1 -progress `. The formatting flag is already
present; no legacy restore-client argument change is justified. Subsequent
FMSS script contracts, physical erase, cold boot and durable writes remain
unqualified.

### Stock FMSS page-count and chunk parameters (2026-10-01)

D18 is the stock request page count: sequencer reads now consume the existing
CPU latch. D28 is the guest-supplied number of 2048-byte chunks per page; it
now latches CPU writes and supports MMIO/sequencer reads, reset and VMState7
(older supported streams initialize the absent field to zero). The value is
not forced to the observed two chunks. Stock-driver disassembly establishes
these inputs; synthetic bounded fixtures exercise the observed loop/read
forms with several counts. No descriptor-load or erase opcode was added.

Actual-handler sanitizer tests pass; real FMSS qtests pass 4/4 including reset
and physical/generated snapshot roundtrips. The independent default native
7E18 two-boot regression passes 8/8, including filesystem health, durable
writes and audio (`/private/tmp/ltm-fmss-d18-d28-default`). Stock erase restore
and honest completion of unsupported scripts remain separate gates.

### Silent I2S migration (2026-10-01)

N45's host-output=false stream previously returned before initializing its
host voice rate. It consequently saved a zero rate and failed whole-board
migration despite valid guest stream state. Silent sinks now initialize a
logical rate. Legacy streams without a realized/active host voice normalize
a missing rate from the validated stream settings only after all ring, queued
PCM, pacing, FIFO and sample-rate checks pass. Valid guest TX/DMA activity is
not mistaken for host voice activity; invalid streams remain rejected.

The registered PHY model suite now includes N45 migration unconditionally
and passes 4/4; the pre-fix whole-board stream fails specifically in I2S.
Actual-source ASan/UBSan audio and alignment tests pass, including valid legacy
silent active state and malformed-state rejection. The independent default
7E18 native two-boot regression passes 8/8 (`/private/tmp/ltm-i2s-silent-default`).
This is silent-stream bookkeeping and migration compatibility, not new audio
hardware, native N45 guest suspend/wake or in-flight host USB qualification.

### FMSS observed register-copy instruction

Stock 5F138 restore programs issue opcode06 with immediate0 to copy a sequencer register. The model now supports this observed form, including copy-zero, self-copy and the bulk program's prior/current mask transfers. Nonzero immediate forms stop as unsupported; descriptor DMA, arithmetic forms and completion behavior are unchanged.

The instruction interpretation is independently corroborated by [lemonjesus's S5L8702 FMISS research](https://github.com/lemonjesus/S5L8702-FMISS-Tools/blob/70b45859af8807a7f841cf649564ce6638e1c112/Documentation.md). That research targets S5L8702, whereas these captured programs target S5L8720; the target's actual descriptor/mask dataflow supplies additional evidence. Reference tool code is GPL3 and its documentation/research CC BY-NC-SA4.0; no reference implementation or document text was copied into production.

The extracted actual-handler sanitizer fixture fails on the preceding model and passes with this correction. Real QEMU model tests observe exact register copies and unsupported-form rejection through guest RAM. This establishes register semantics only: full NAND operations and physical erase restore remain unsupported, and aborted sequencer completion is still a separate gap.

Actual-handler sanitizer tests pass; real FMSS qtests pass 5/5, including
register copies and rejection observed through QEMU guest RAM. The independent
default 7E18 two-boot native regression passes 8/8, including filesystem health,
persistence and audio (`/private/tmp/ltm-fmss-opcode06-default`). Full stock
flash restore remains unqualified.

### FMSS stock descriptor word loads

The production-bridge stock5F138 erase probe next stops on opcode03/imm0 at +0x88 in its bulk NAND script. The model now performs the observed instruction as a little-endian32-bit guest-address-space load. It rejects unsupported nonzero immediates before DMA and stops on QEMU memory transaction errors before consuming a fabricated descriptor or executing a later store.

Tests cover the four actual stock operand forms (r1←*r0, r2←*r0, r7←*r1, r0←*r1), distinct values and operands, zero/high-bit words, pointer increments and unchanged input memory. Sanitizer fixtures inspect DMA width/addresses and fail against the preceding model. Real QEMU qtests exercise RAM byte order and unassigned physical-address rejection. No disputed logical/shift instruction supplies the observation oracle.

Relevant independent factual ISA research: [S5L8702 FMISS descriptor loads](https://github.com/lemonjesus/S5L8702-FMISS-Tools/blob/70b45859af8807a7f841cf649564ce6638e1c112/Documentation.md). Its target differs from S5L8720; actual5F138 driver/script operand and pointer-array dataflow supplies target-specific corroboration. No licensed reference implementation or documentation text was copied.

Build and targeted gates: sanitizer actual-handler/script and IRQ tests pass; real FMSS qtests6/6. Logs `/private/tmp/ltm-nand-contract-agent/opcode03-{baseline,script,irq,build,qtest}.log`. The independent default native regression also passes, as recorded below.

This is descriptor DMA only. Other sequencer instructions, erase, blank-page representation and aborted-program completion remain separate gaps. No physical restore/coldboot qualification follows from these model tests.

Actual-handler sanitizer tests pass and real FMSS qtests pass 6/6, including
distinct descriptor operands, explicit little-endian words, increments,
unsupported forms and failed QEMU transactions. The independent default
7E18 native two-boot regression passes 8/8 (`/private/tmp/ltm-fmss-opcode03-default`).
No new completion, physical erase or full stock restore qualification follows.

### FMSS stock mask intersection

The stock 5F138 bulk erase script reaches opcode0A000004 with immediate zero
at +0xa8. The model now intersects the existing destination and source
registers for this form; nonzero immediates mask the source into the destination.
Actual S5L8720 driver/script dataflow corroborates the related
[S5L8702 FMISS research](https://github.com/lemonjesus/S5L8702-FMISS-Tools/blob/70b45859af8807a7f841cf649564ce6638e1c112/Documentation.md).
No reference implementation or documentation text was copied.

Actual-handler sanitizer tests fail on the preceding model and pass with the
correction. Real FMSS qtests pass 7/7, covering overlapping, disjoint, zero and
high-bit masks and distinct immediate operands. The independent default 7E18
native two-boot regression passes 8/8, including guest-confirmed shutdown,
persistence, graphics and audio (`/private/tmp/ltm-fmss-and-default`).

Other arithmetic, physical storage, erase and aborted-script completion are
unchanged. Physical restore and subsequent cold boot remain unqualified.

### Physical erased-page reads

Explicit FMSS_PHYSICAL reads now return FF main bytes and current stored
metadata for confirmed absent directory pages, valid packed holes and known
erased markers. The packed reader distinguishes holes from invalid addresses
and records. Generated-mode bytes remain unchanged. Non-ENOENT open failures,
invalid packed records and short records retain their existing fallback
policy; preserving that policy does not establish physical error fidelity.
The 64-byte stored spare and 12-byte guest metadata are a controller projection,
not a newly qualified raw OOB/ECC layout.

The maintained actual-source sanitizer baseline compiles and fails on the
exact physical FF assertion; the fixed test passes. Real FMSS qtests pass
12/12 including both storage formats/modes, actual ENOTDIR paths, marker
precedence and exact erased bytes across migration. Script/IRQ, generated
free-pool and persistence checks pass, including 18 fault cases and a
1,024-page bulk write. Persistence test declarations were repaired for the
pre-existing GTree cache types without weakening failure assertions.

Independent default 7E18 native two-boot regression passes 8/8
(`/private/tmp/ltm-fmss-physical-blank-default`). This qualifies generated-path
regression safety, not stock physical restore, ECC, bitwise programming or
honest sequencer completion.

### READ-ID selector host bounds

The interpreter now checks the populated-chip bound before shifting by the
trailing-zero count. QEMU ctz32(0) returns 32, so an empty selector previously
caused undefined host C behavior. This is a host robustness correction;
existing empty, multiple and unpopulated selections still return no chip.

The strict sanitizer baseline reproduces shift-by-32; corrected script and IRQ
fixtures pass. Real FMSS qtests pass 13/13, including both ID words for populated
CE0..3, empty/multiple selection and absent CE4. Independent default native
7E18 two-boot regression passes 8/8 with diagnostic-only unimplemented-operation
logging (`/private/tmp/ltm-fmss-ce-default`).

That logging confirms an existing completion shortcut in the normal boot path:
iBoot and XNU stop on opcode14/imm16, and a separate XNU script reads the
unmodeled D48 parameter. Boot still passes because these incomplete runs receive
deferred completion. Correct instruction/parameter contracts are required
before honest completion can pass native acceptance. No physical restore or
sequencer completion fidelity follows from the selector fix.


### FMSS sequencer research locations and remaining limits (2026-10-01)

Research addresses identify stock artifacts for reproducible tests; they are not
emulator runtime dispatch keys. The READ-ID programs are at file offset `0x25330`
in 7E18 iBoot and `0x25a60` in 8C148 iBoot, captured in
`tests/ipod/test_fmss_script.py`. Stock 5F138 kernel scripts are bulk VA
`0xc05f3970` / file `0x5ca970`, read VA `0xc05f3560` / file `0x5ca560`, and status
VA `0xc05f2520` / file `0x5c9520`.

The stock restore trace reaches opcode `0x14` immediate 16 in bulk `+0x128` and
read `+0x110`. Each splits a descriptor page field between the FMC address
registers. The model supports only immediate 16 with source bit 31 clear, where
logical and arithmetic right shifts agree. It stops other forms before later
stores; that is a limitation of the model, not evidence of hardware rejection.
The exact live descriptor word has not been captured, and shift signedness
remains unmeasured. Unit/qtest fixtures establish bounded interpreter behavior;
they do not establish successful NAND execution or restore completion.

Factual opcode research also comes from the independently documented
[S5L8702 FMISS tools](https://github.com/lemonjesus/S5L8702-FMISS-Tools/blob/70b45859af8807a7f841cf649564ce6638e1c112/Documentation.md),
which target S5L8702 rather than this board's S5L8720. That research explicitly
leaves right-shift signedness unresolved. Its documentation is CC BY-NC-SA 4.0
and tools are GPL-3.0; no reference code or documentation text was copied into
this implementation.

Further independent gaps remain: sequencer writes to Dxx parameter latches are
ignored; zero-immediate OR/SHL accumulator behavior is incomplete; instruction
fetch, opcode `0x11` stores and descriptor loads now use checked, explicit
little-endian AddressSpace transactions and stop on transaction errors. CPU-side NAND operations,
event waits and synthetic completion remain separate compatibility behavior.

Research QMP MMIO capture must distinguish implemented readback from default
zero: only D28 and D4C currently expose these parameter latches through CPU MMIO.
Zero reads of C04/D08/D0C/D10/D18/D1C/D20/D30 do not establish stored latch values.
Known Dxx CPU writes are not generally logged by FMSS_TRACE. Exact sequencer
operands need a separate diagnostic trace or host debugger; adding fake hardware
readback solely to inspect model state would be the wrong fix.

The bounded opcode14 correction passes strict actual-source sanitizer and IRQ
checks, real FMSS qtests14/14 and independent default native7E18 two-boot8/8
(`/private/tmp/ltm-fmss-opcode14-default`). Diagnostic-only tracing now reaches
unmodeled D34 reads at iBoot +0xe40 and kernel +0xda0; the separate D48 script
still stops at +0x30. This verifies progress through the earlier shift site,
not full ISA, parameter-bank, NAND operation or completion fidelity.

### Bounded FMSS command investigation

`FMSS_SCRIPT_TRACE=1` enables an observational, process-wide sequencer trace.
It records CPU parameter/start writes, the fetched instruction words and
program-relative PCs, controller writes, register-read operands, descriptor
read addresses and sequencer store addresses. `fmc_write_unmodeled` explicitly
identifies accepted controller writes beyond the current shadow, including
804/810. It does not add guest memory or MMIO transactions, change controller
results, supply data or qualify completion. Descriptor contents, controller
data-window values, auxiliary write values and store payloads are omitted.

An optional `FMSS_SCRIPT_TRACE_CSGENRC=0xa02` selector records only requests
whose current CSGENRC operation matches the specified hardware register value.
The selector is validated once, invalid values disable tracing with one message,
and nonmatching operations do not consume the record budget. Matching the stock
write mode captures later restore writes without flooding the budget with early
read retries; it does not select execution behavior or match firmware addresses.

The trace emits at most 16,384 records, followed by one truncation marker;
subsequent events are silent. Reset does not reset this diagnostic cap. Run
a disposable blank-flash restore from process start to capture the first
physical command path, with normal diagnostics recording the unsupported
D24 stop. D24 remains unsupported: permitting it before establishing the
physical spare producer would expose the previously demonstrated duplicate
spare overwrite. Trace success or program END is not a NAND completion gate.

`tests/ipod/test_fmss_script_trace.py` compiles the actual trace and sequencer
handlers under ASan/UBSan. It compares traced/untraced guest transaction
counts, results and output, verifies unsupported auxiliary writes are visible
and payloads are omitted, and checks the exact record cap/single marker.

### READ-ID FIFO transfer (2026-10-02)

READ-ID now owns actual serial bytes before FMC40=0x52/0x82 copies5/8 bytes to
FMC60/64. Reads expose the window; they do not fabricate a chip ID or clear
busy. Partial transfers preserve untouched window bytes, and missing/depleted
input remains busy. Actual7E18/8C148 scripts, sanitizer/model suites and all32
FMSS board qtests pass. The new missing-producer board test rejects the retained
pre-fix binary because it invents an ID before receiving bytes. The current
combined7E18 binary passes all8 native regression checks, including two
guest PMU-confirmed shutdowns and durable cold-boot persistence/fsck. This
qualifies the combined build, not an isolated FIFO binary. NAND FMC40=0xc1
transmission and ECC/OOB routing
remain unimplemented; this change does not claim physical restore completion.
