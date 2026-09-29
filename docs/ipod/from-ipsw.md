# iPod touch 2G from a stock IPSW (manifest → device)

`imgtools/device.py create MANIFEST OUT` builds a device from declared inputs only: a sha1-pinned IPSW, its
keys page and a seed for a synthetic identity; no third-party tarballs (the iPod no longer gets a shell). It is the same
orchestrator, manifest format and `device.lock.json` as the iPad (`ipad1_device.py` is now the k48ap board
module; its CLI is unchanged). The iPod board is `imgtools/ipod2g_device.py`.

```
imgtools/device.py create manifests/ipod2g-7E18.json OUT        # ~35 s
tests/ipod/fresh-device.sh manifests/ipod2g-7E18.json OUT      # create + the default regression tier
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
| 2.1.1 5F138 | complete (no AppSync, GLES shim or modern guest helpers) | SecureROM → LLB → iBoot → kernel → stock SpringBoard, Connect to iTunes | activation; optional helpers need a 2.x-compatible build |

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
Fix (hw/arm/ipod_touch_fmss.c `fmss_run_script`): run the guest's program at start, with FMCMD 0x90
returning the selected CE's ID from FMDATA; each build's own program lays out its own records. The flash
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

**The one-shot** (`imgtools/ipod2g_keybag.py`, run by `ipod2g_device.py` for manifests with
`options.data_protection`, i.e. 8C148). As on the iPad, the IPSW's own (Update) restore ramdisk, a private
copy with `it_keybag` (contrib/it-keybag, armv6 build `build-ipod.sh`: the iPod volume is one, disk0s1,
data at `/private/var`) as `restored_external`, boots as `md0` so the root is a SecureRoot (the normal
4.2.1 DT already carries `secure-root-prefix = md`). The difference is the handoff: everything iBoot loads
is a signed img3 (its `boot-ramdisk` NVRAM path loads type `rdsk` to 0x0c000000 and validates it), so a
modified ramdisk cannot come through iBoot. iBoot boots the device normally with `rd=md0` in its command
line; at the kernel's entry (LC_UNIXTHREAD pc, MMU off) a gdbstub breakpoint adds what iBoot's restore
path adds: the ramdisk at topOfKernelData, a `RAMDisk` (pa, len) entry in a spare `MemoryMapReserved`
slot of chosen/memory-map, an empty chosen/root-matching, topOfKernelData moved past it. `it_keybag`
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

Fresh 8C148 device (`device.py create manifests/ipod2g-8C148.json OUT --activation-hook ...`, lock
`derived.gles = "shim MBXGLEngine-8C148"`): `regress.py --device OUT --checks boot,gles` PASS, home screen lit=285214,
GLTest magenta 0.141 / cyan 0.281 / yellow 0.141, no unimplemented slot. Host log: one SpringBoard
`GLESGetEGLInterface`/`GLESCreateGC` pair, `[gles] host GL up`, `swap: framebuffer ID ... signalling the main
display`, then GLTest's `GLESBindView` and `present tally: ok=600 failed=0`. 7E18 with the new shim staged
(`regress.py --stage-gles-shim`, default tier): 8/8, same GLES colours.

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
| identity | real unit's (NOR) | `ipod2g_device.identity` (:84): serial and MACs from `ipad1_kboot.synth_identity`, battery serial from the seed, Mod#/Regn from the manifest; UDID SHA1(serial+Wi-Fi+BT) |
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
| GLES shim | MBXGLEngine shim, CA_ENABLE_OGL=1, MBX2D/auto off | same, the one `contrib/it-gles/MBXGLEngine`, which reads the firmware's dispatch layout at load (`ipod2g_device.gli_engine` only logs what it will find); 2.x (no shared cache) → stock engine + software CA |
| shell + ssh | Cydia bootstrap files copied as uid 99, stock modes clobbered by `chmod 755`, sshd by overwriting ReportCrash.SafetyNet, host keys shared by every copy | **none**: no freeze, OpenSSH or OpenSSL; guest services are stock lockdown services plus it_agent v2 (docs/ipod/guest-services-plan.md), marker `.lt-guest-tools-v3` |
| web proxy / CA trust | itproxy/ittrust run over SSH | the iPad's PAC baked into the en0 Wi-Fi service (`ipod2g_device.install_web_proxy`); CA by a MCInstall profile at run time |
| byte patches | none left on the default path: installd/SpringBoard are stock | none; see "emulator-side per-version code" |

### Remaining emulator compatibility behavior

- hw/arm/it_iboot.c (board-agnostic; the iPod machine calls it after staging iBoot) finds by
  pattern, in any iPod touch 2G iBoot (2.1.1 .. 4.2.1, pinned by tests/ipod/test_iboot_literals.py):
  the normal-boot command-line literal it redirects at the staged `boot-args` string (the restore
  command line `rd=md0 nand-enable-reformat=1 -progress`, its single literal, the word before it,
  which Thumb code must load; else "unknown iBoot", nothing written); the build's security epoch
  (the floor its epoch helper applies to the chip ID fuse field: 1/2 on 2.x, 3 on iBoot-596 = 3.0,
  4 from iBoot-636 on), which the SYSIC model returns in POWER_ID[31:24] in place of the LLB's
  latch, so miu_init's "Epoch Mismatch" panic no longer keys on one build; and the 2.x
  gBootArgs.commandLine buffer for the NAND-boot data write. The late boot-args write finds
  `boot_args` by signature (kernel base 0xC0000000 or 0x80000000) on any build.
- 3.0 (7A341): the baked helpers (it_agent, it_typein DYLD_INSERTed into SpringBoard, sblaunch, it_prefs)
  are linked for the 3.1+ dyld; 3.0's refuses LC_DYLD_INFO_ONLY like 2.x, so `ipod2g_device.py` omits them
  below 3.1 (stock SpringBoard, no guest package) until a legacy-linked set exists. 3.0 has no dyld shared
  cache (it arrived with 3.1), so `options.appsync` must be off (`patch-appsync-dylib.sh` patches the cache;
  3.0 would need a symbol-located patch of libmis.dylib itself) and the GLES shim is skipped (stock engine).
- The obsolete fixed-address logo thunk is removed along with the DeviceTree thunk.
- The research-only IT_AMFI_ALLOW_TASKPORT kernel patch and its address overrides
  have been removed; guest integration uses the existing boot-args and AppSync path.
- The legacy command-line data write (without direct iBoot) discovers its buffer
  from iBoot's own literal references to `gBootArgs.commandLine = [%s]`; absent,
  ambiguous and out-of-range matches cause no write. No fixed build address remains.
- The BCM4325 model's Wi-Fi MAC is a fixed value from the original unit (hw/arm/ipod_touch_sdio.c:307, :1326),
  so it does not follow the synthetic identity's `wifiaddr`. A `wifi-mac` machine option would fix that
  (a model change; not done).

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

The S5L UART acknowledgement mode now applies to every boot strategy. Previously
it was selected only for direct iBoot; SecureROM boots used Exynos acknowledgement
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

`device.py create manifests/ipod2g-5F138.json OUT --activation-hook .../lt-activation
--activation-hook-arg=--experimental-legacy` builds (hook applied, daemon re-signed, aes-uid=engine). The
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
connect. This is host-protocol behaviour, so the emulator needs no change.

Measured: `idevicepair pair` then `idevicedate -c` logs `toggle_brick_state: Disabling the brick state (time
interval)`. On the same boot, a home press and unlock swipe reach the home screen (first-run Edit Home Screen
tip; Dismiss works). The cleared state persists: the next boot goes lock screen -> home with no host action.
The host has to set the time once per fresh device, for example next to the app's existing TimeZone set
over lockdown.
