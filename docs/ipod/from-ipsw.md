# iPod touch 2G from a stock IPSW (manifest → device)

`imgtools/device.py create MANIFEST OUT` builds a device from declared inputs only: a sha1-pinned IPSW, its
keys page, a seed for a synthetic identity, and (iPod only) three sha256-pinned tool tarballs. It is the same
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
| 4.2.1 8C148 | complete (NOR, NAND, GLES check, AppSync, gid-blobs) | iBoot-931.71.16 runs and reads the NOR; `[NAND] findNandInfo: No NAND Detected`, recovery mode | 4.x NAND identification |
| 2.1.1 5F138 | complete (no shared cache: no AppSync, no GLES shim) | nothing on serial (bootrom → LLB → iBoot stops) | LLB rejects the IPSW iBoot |

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
cache off 0x40d76d4), installd job `com.apple.mobile.installd.plist`. The GLES check refused the shim:
`dispatch table differs from gli-dispatch-7E18.tsv at slot 441 (841 vs 822 slots)`, so the image keeps the
stock MBXGLEngine with software CoreAnimation (`CA_ENABLE_OGL=0`).

Boot: the emulated AES engine has no GID key, so it answered only KBAGs in its built-in 5F138/7E18 table and
exited on 4.2.1's first NOR image. The new `gid-blobs=FILE` machine option (the builder writes
`gid-blobs.bin` from each img3's KBAG and the keys page) fixed that. iBoot then prints its banner with the
synthetic serial and stops at `[NAND] findNandInfo:291 No NAND Detected` / `[FIL:INF] could not find NAND
config in the new NAND tables` → `root filesystem mount failed` → recovery. The emulated chip ID 0xb614d5ad is
still in 4.2.1 iBoot's table (0x250e4), but the entries have a new layout (id, 0, geometry... instead of
id, 0x100000ff, ...). A quick trial of 8-byte `{id, 0}` records per CE in `write_chip_info`
(hw/arm/ipod_touch_fmss.c:97) changed nothing; that trial was reverted. Next step: trace 4.2.1 iBoot's
ReadID/findNandInfo over the gdbstub to see what it reads and compares. Estimate: half a day to 2 days, then the kernel's
own FIL/FTL (xnu-1504 has the 4.x whimory) may need the same, plus whatever else the 4.x kernel touches.
Activation will block after that, as on 7E18.

### P3, 5F138: LLB → iBoot

2.1.1 needed pipeline fixes, all derived: no BuildManifest (component paths from Restore.plist and the board
name, `ipad1_fw.components`), the final partial AES block of 2.x img3 left in plaintext (detected from the
kernelcache's Adler-32 and applied to every component), NAND epoch 1, SHSH not wrapped, no direct iBoot
(2.x boots bootrom → NOR LLB), no shared cache (AppSync off in the manifest: 2.x's libmis patch is a fixed
offset, `patch_libmis.py`, so it is not used), firmware's own libncurses kept. With the synthesized NOR, the
serial port stays silent. With the old 2.1.1 NOR dump in its place (diagnostic only), iBoot-385.22 and xnu-1228.7.27
run and the boot stalls at the logo (lit=9852). The two NORs' LLBs are identical, so the difference is in what LLB
loads. build_nor.py already notes that the dump's iBoot differs from the IPSW's only in its RSA signature
(personalised). Next: compare the dump's iBoot/SysCfg/nvram with the synthesized ones one region at a time.
Estimate: a day for the NOR, unknown for the logo stall.

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
| fake Wi-Fi setup | not in the image (SystemConfiguration is configd's own runtime output) | none needed; `setup_networking.py` stays optional |
| guest services | it_agent, it_typein, sblaunch, sbdlicon, markers, sound defaults (bake-guest-tools.sh) | same script, run inside build_nand's mount (`ipod2g_device.bake`, :306), owners patched in the catalog |
| pasteboard | it_pbd binary present, job retired | not installed (the agent owns the clipboard) |
| sound defaults | set-sound-defaults.py | same |
| AppSync | cache MISValidateSignature (by symbol) + libappsync in installd | same script, contrib/appsync/patch-appsync-dylib.sh |
| GLES shim | MBXGLEngine shim, CA_ENABLE_OGL=1, MBX2D/auto off | same, gated by the @encode check against docs/ipod/gli-dispatch-7E18.tsv (:116); refusal → stock engine + software CA |
| shell + ssh | Cydia bootstrap files copied as uid 99, stock modes clobbered by `chmod 755`, sshd by overwriting ReportCrash.SafetyNet, host keys shared by every copy | files listed in imgtools/ipod2g-shell.txt taken from the three pinned tarballs, root-owned, tar modes, `/Library/LaunchDaemons/com.openssh.sshd.plist`, host keys generated per device |
| byte patches | none left on the default path: installd/SpringBoard are stock | none; see "emulator-side per-version code" |

### Emulator-side per-version code (left as is, all fail closed)

- `ipod_touch_inject_boot_args` (hw/arm/ipod_touch_2g.c:1511) checks 24 bytes of 7E18 iBoot and skips
  otherwise ("unknown iBoot; early argument injection skipped", seen on 8C148); the late boot-args write
  finds `boot_args` by signature on any build.
- IT_INJECT_LOGO (7E18 VAs) and IT_AMFI_ALLOW_TASKPORT (firmware profile, 7E18 only)
  are opt-in env knobs, off on the default path.
- 5F138 `iboot_boot_args_pa` (fmss) applies only without direct iBoot.
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
| SHSH wrap, direct iBoot | ProductVersion major ≥ 3 |
| NOR image set | stock order ∩ all_flash/manifest |
| kernelcache path in the volume | the decrypted iBoot's `/System/Library/Caches/com.apple.kernelcaches/...` string |
| kernelcache member | BuildManifest KernelCache / Restore.plist KernelCachesByPlatform |
| GL dispatch ABI | shared cache `__GLIFunctionDispatchRec` @encode vs docs/ipod/gli-dispatch-7E18.tsv |
| MISValidateSignature | shared-cache symbol table (appsync_cachepatch.py) |
| installd job name | whichever of the two known plists exists |
| volume size, Mod#, Regn | manifest (`volume_blocks`, `model_number`, `region_info`) |

## Fresh 7E18 image vs nand-current.new (tests/ipod/nand_manifest.py)

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
  (userland-boot.md: "Without an activation hook the device stops at Connect to iTunes").

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
