> Status: research, superseded by `../ipad1/guest-services.md` (the AppSync interposer dylib).

Now the report.

# iPad1,1 / iOS 3.2 (7B367) unsigned-code feasibility — enforcement chain, gatekeepers, and recommended plan

## 1. Kernel/AMFI code-signature enforcement chain (RELEASE_ARM_S5L8930X, xnu-1504.2.27, AMFI-68)

**The decisive finding: the AMFI boot-args do NOT work by themselves. They are master-gated on `debug-enabled`.**

AMFI kext = `AppleMobileFileIntegrity-68`, prelinked into the kernelcache at load addr `0xc03ae000`, matches `IOResources`/`IOBSD` (`$SP/dec/kc.strings:19127`; prelink dict `com.apple.driver.AppleMobileFileIntegrity` v1.0.2, `_PrelinkExecutableLoadAddr 0xc03ae000`, `AppleSecurityExtension=true`, in the persisted prelink-plist dump `tool-results/bd52or0zk.txt` @ kc.strings:40337).

Boot-arg string VAs (computed from `$SP/dec/kernelcache.k48.mach` segment map):
- `amfi_unrestrict_task_for_pid` = 0xc03b9650
- `amfi_allow_any_signature` = 0xc03b96a4
- `amfi_get_out_of_my_way` = 0xc03b96f0
- `cs_enforcement_disable` = 0xc03b9708
- `"cs_enforcement disabled by boot-arg"` = 0xc03b9724; `debug-enabled` = 0xc02144a8

**AMFI::start (VA 0xc03afe64)** — source `$SP/kx/MFI.dis:1863-2000`:
```
c03afea4  ldr r3,[pc,#0x14c]      ; = 0xc01d17c1  (PE_i_can_has_debugger, thumb)
c03afea6  blx r3
c03afea8  cmp r0,#0
c03afeaa  beq 0xc03aff32          ; <-- if PE_i_can_has_debugger()==0, SKIP entire block
c03afeb2  ldr r0,=... "amfi"      ; PE_parse_boot_argn (r4 = 0xc01d0ef1)
c03afeb8  "amfi_unrestrict_task_for_pid" -> if set: str #1,[r5,#0x64]   (c03afed6)
c03afed8  "amfi_allow_any_signature"     -> if set: str #1,[r5,#0x68]   (c03afef6)
c03afef8  "amfi_get_out_of_my_way"       -> if set: str #1,[r5,#0x6c]   (c03aff16)
c03aff18  "cs_enforcement_disable"       -> if set: str #1,[r5,#0x70]   (c03aff2e)
c03aff32  (skip target: r4 = PE_parse_boot_argn ptr; flags stay 0)
```
The four enforcement flags live in the AMFI singleton at `+0x64/+0x68/+0x6c/+0x70` and are initialized to 0 at c03afe94-c03afea0. If the gate is not taken they remain 0.

**The gate function `PE_i_can_has_debugger` (0xc01d17c0)** — source `$SP/kx/kernel.dis:743958-743970`:
```
c01d17c0  cbz r0,0xc01d17d6
c01d17c2  ldr r2,=0xc02787b8      ; &debug_enabled (global)
c01d17c4  ldr r3,[r2]
...        returns *0xc02787b8 (and, if arg!=NULL and debug_enabled!=0, stores *0xc0267200 into *arg)
```
So it returns the global **`debug_enabled` @ 0xc02787b8**.

**`debug_enabled` is loaded verbatim from the DeviceTree `chosen/debug-enabled` property** — source `$SP/kx/kernel.dis:744135-744170` (PE machine-init):
```
c01d196c  ldr r1,=0xc02144a8 "debug-enabled"
c01d1974  bl  0xc01d0ae0          ; DTGetProperty(chosen,"debug-enabled",&ptr,&len)
c01d1978  cmp r0,#1 / beq 0xc01d19ae
c01d19ae  (len clamped to 4)
c01d19b8  ldr r0,[sp]             ; property data ptr
c01d19ba  ldr r1,=0xc02787b8      ; &debug_enabled
c01d19bc  blx 0xc0064578          ; memcpy(&debug_enabled, data, 4)
```
The same global also gates the base-kernel `debug=` boot-arg (`$SP/kx/kernel.dis:c023b618`: `PE_parse_boot_argn("debug") && debug_enabled` else assert/zero), confirming `debug-enabled` is the master switch for all debug/enforcement boot-args, not just AMFI.

**Exec-time consumption** — `AMFI_vnode_check_signature` (VA 0xc03b071c), source `$SP/kx/MFI.dis:2870-2910`:
```
c03b0726  ldr r3,[r5]            ; AMFI singleton (*0xc03c26b8)
c03b072a  ldr r3,[r3,#0x6c]      ; amfi_get_out_of_my_way -> if set, skip validation (branch 0xc03b075e = allow)
c03b0738  bl  0xc03afb90         ; validate CD hash of the vnode
c03b073e  cbnz r0 -> invalid
c03b0744  ldr r3,[<singleton>,#0x68] ; amfi_allow_any_signature
c03b0746  cbz r3,0xc03b075a      ; ==0 -> DENY (return the error)
c03b0748  IOLog "AMFI: Invalid signature but permitting execution\n"  (VA 0xc03b9a60)
c03b074e  return 0 (allow)
```
So: an invalidly-signed/ad-hoc/patched binary execs **only if `+0x68` (amfi_allow_any_signature) is set**, which **only** happens if `debug-enabled != 0`. `+0x6c` (get_out_of_my_way) bypasses validation entirely.

**Conclusion for Q1:** `cs_enforcement_disable=1`/`amfi_allow_any_signature=1`/`amfi_get_out_of_my_way=1`/`amfi_unrestrict_task_for_pid=1` **do fully relax code-sig enforcement for executables, dylibs and dlopen'd bundles — but only when `PE_i_can_has_debugger()` is true, i.e. DT `chosen/debug-enabled != 0`.** On a stock production boot the DeviceTree ships `debug-enabled = u32 0` (`$SP/dec/dt.txt:32`; also `production-cert=0`, `secure-boot=0`, `development-cert=0` at dt.txt:21/24/34), iBoot fills these from the CHIPID production fuse, and the boot-args are then **silently ignored**. **This is the new obstacle absent from the 3.1.3 path** (the qemu-ios iPod tree sets no `debug-enabled` anywhere — `grep` across `hw/` is empty — which is why 3.1.3's xnu-1357 AMFI honors `amfi_allow_any_signature=1` with just the DRAM boot-args write; the `PE_i_can_has_debugger` gate is an AMFI-68/xnu-1504-era addition. *Not independently verified against the 3.1.3 AMFI binary — inferred from the working 3.1.3 setup carrying no debug-enabled handling.*)

**Delivery of boot-args:** iBoot-817.28 has `boot-args` (iboot.strva 0x5ff21ea8) and `gBootArgs.commandLine = [%s]` (0x5ff23e4c) and writes the chosen certs/`debug-enabled` (iboot.strva 0x5ff23f6c/0x5ff23f90/0x5ff23fb8). I **could NOT** cleanly disassemble iBoot-817's `debug-enabled` derivation — the literal-pool region in `$SP/bootchain/iboot.dis` is mis-decoded as data and llvm-objdump did not annotate the `movw/movt` refs, so **whether clearing the production fuse alone makes iBoot write `debug-enabled=1`, and whether iBoot overwrites a DT-edited value after DT load, is UNCONFIRMED.** For the 3.1.3 iPod, qemu-ios does not use NVRAM boot-args at all: 7E18 iBoot heap-panics on NOR boot-args (`hw/arm/ipod_touch_2g.c:1114`, `:1482`), so it injects the string directly into the live `boot_args.CommandLine` at +0x38 by scanning DRAM for the struct signature (`rev==1, virtBase==0xC0000000, physBase==0x08000000`) on a repeated early timer before AMFI latches (`ipod_touch_2g.c:1130-1343`; the default string `amfi_allow_any_signature=1 cs_enforcement_disable=1` is set in `contrib/run-ipod-touch.sh:306`, `tests/ipod/regress.py:347`, `docs/capabilities.md:60`). A NOR NVRAM `boot-args=` writer also exists (`hw/arm/ipod_touch_nor_spi.c:29-70`) and an FMSS default (`hw/arm/ipod_touch_fmss.c:856`).

**Smallest patch set for 3.2 (recommended, mirrors 3.1.3 mechanics):**
- **Force the gate.** iPad DRAM base = 0x40000000, kernel `virtBase=0xC0000000`/`physBase=0x40000000` ⇒ VA→phys slide **0x80000000** (`$SP/research/ref-a4-board.md:17,45`, `ref-iemu.md:64-72`; KERNStart 0x80000000). So `debug_enabled` global VA `0xc02787b8` = **phys `0x402787b8`**. Two independent ways:
  1. **DeviceTree edit** (cleanest, no kernel patch): set `chosen/debug-enabled = u32 1` in the decrypted DeviceTree we already control. *Risk: iBoot may overwrite it from the fuse after load — unconfirmed.*
  2. **Kernel-global write** (guaranteed): on the same early repeated timer used for boot-args, write `0x00000001` to phys `0x402787b8` after the kernel copies DT→global (machine-init) and before AMFI::start. Equivalently patch `PE_i_can_has_debugger` @0xc01d17c0 to `movs r0,#1; bx lr`, or NOP the `beq` at `0xc03afeaa`. The global write is smallest and also re-enables the `debug=` arg.
- **Inject boot-args** `amfi_allow_any_signature=1 cs_enforcement_disable=1` into `boot_args.CommandLine` (+0x38), scanning DRAM `0x40000000..0x40800000` for `virtBase==0xC0000000 && physBase==0x40000000` (adapt the 3.1.3 scanner's physBase constant from 0x08000000).
- **Interaction with the forged iBoot chain:** qemu-ios already forges the Image3 signature/personalization checks and can forge the CHIPID production/security fuses (`hw/arm/ipod_touch_chipid.c:9-41`: `IT_DEV_MODE` clears production bit 5 → CPFM 0x01; `IT_INSECURE_MODE` also clears security-domain bit 2 → CPFM 0x00). Running "dev-mode" is an alternative route to `debug-enabled=1` **iff** iBoot derives it from the fuse (unconfirmed). Because the forge is independent, the debug-enabled global write does not conflict with it.

## 2. Userland gatekeepers on a modified rootfs

- **Kernel page-hash check still runs even with amfi_allow_any_signature.** As on 3.1.3, `amfi_allow_any_signature` forgives an *invalid top-level* signature but the kernel hash-checks each code page on fault-in; a patched page whose CodeDirectory hash no longer matches faults as INVALID (`imgtools/patch_libmis.py:29-32`, `imgtools/patch_codesign_gate.py:26-30`). **Every on-disk Mach-O we edit must be re-signed with `ldid -S`** to recompute CD page hashes (verified by `imgtools/cdverify.py`). Standalone SpringBoard must keep its 8 stock entitlements (`ldid -S` alone strips them and breaks keychain/iTunes messaging — `imgtools/README-appsync.md:41-46`).
- **`MISValidateSignature` is inside the shared cache on 3.2**, not a standalone dylib. `/usr/lib/libmis.dylib` is present in `$SP/fw/7B367-dec/dyld_shared_cache_armv7` (string confirmed). Same as 3.1.3 (`README-appsync.md:27`: 3.1.3 patched `dyld_shared_cache_armv6` off `0x1750EF8`, `80b500af→00207047` = `MISValidateSignature` returns success). For 3.2 the equivalent site is in `dyld_shared_cache_armv7` (offset must be re-derived; the 2.1.1 standalone `patch_libmis.py` @ va 0x33a27c58 and the 3.1.3 offset do NOT transfer). This gate only matters for **installing** apps via `MobileInstallation`/`installd`; not needed just to boot a modified system rootfs.
- **dyld dlopen/bundle signature checks route through the same AMFI vnode hook** — there is no separate dyld enforcement on this era; a re-signed on-disk dylib/bundle loads once `amfi_allow_any_signature` is set. The GLES shim is a bundle load (see §3), so it is covered by the AMFI relaxation + `ldid -S`.
- **launchd does not verify daemon signatures itself on 3.2.** Shared-cache strings show only lockdown/XPC-style `"request not signed"`/`"The client is not signed."`, no launchd exec-time signature gate — launchd execs and AMFI gates at exec. So a re-signed unsigned daemon (e.g. the guest agent) launches. *Not exhaustively verified.*
- **AMFI MachServices / task-port gate.** On 3.1.3 qemu-ios found `amfi_allow_any_signature` is insufficient for SpringBoard to obtain a spawned app's task-port; it patches the MAC hooks `mac_proc_check_get_task_name` (VA 0xc01ab2a0) and `mac_proc_check_get_task` (VA 0xc01ab200) in kernel memory to `movs r0,#0; bx lr` (`hw/arm/ipod_touch_2g.c:1140-1216`, env `IT_AMFI_ALLOW_TASKPORT`). **I could NOT locate the 3.2 VAs for these two functions** (need base-kernel xref; the 3.1.3 addresses are for xnu-1357 and must not be reused). This gate matters for *launching injected third-party apps*, less so for booting to the home screen with system apps.
- **Sandbox kext + sandboxd are present and prelinked** (`com.apple.security.sandbox` v51.2, `Sandbox.kext`, in the prelink dump; `sandboxd` in rootfs). **SpringBoard is not sandboxed** on this era, and the GLES shim runs in-process talking to emulated GPU MMIO (not IPC), so a sandbox profile is not expected to block it. **UNVERIFIED for mediaserverd** — I did not extract/parse the SpringBoard/mediaserverd sandbox profiles; if a host-forwarding path uses a Mach service, that would need a profile check.
- **Activation / lockdown.** SpringBoard links `/usr/lib/liblockdown.dylib` and reads `kLockdownActivationStateKey`/`kLockdownBrickStateKey` (`$SP/dec/SpringBoard` strings). The read-only 3.2 rootfs contains **no** `/private/var/root/Library/Lockdown` activation record (only `/System/Library/Lockdown/{Checkpoint.xml,FactoryActivation.pem,...}` templates), and there is an `Applications/DataActivation.app`. Without an activation record, 3.2 SpringBoard shows the "Connect to iTunes" activation screen. **qemu-ios has NO in-emulator activation handling** — `grep` for `ActivationState`/`data_ark`/`activation_record`/`/var/root/Library/Lockdown` across the repo finds nothing. **I could NOT find how the 3.1.3 build satisfies activation**; the working 3.1.3 image is a *prepared* golden NAND (`nand-agent-v4` from "Light Touch", `README-appsync.md:3`) that is almost certainly **pre-activated during original off-tree image prep** (activation record baked into the NAND), not activated by any repo mechanism. The 3.2 plan must reproduce this: bake an `ActivationState=Activated` lockdown record into `/private/var/root/Library/Lockdown/` (data_ark) in the rootfs, or serve lockdownd activation over the emulated USB path (`docs/networking.md:479-490`: lockdownd on port 62078). This is an open work item.

## 3. What the 3.2 rootfs build must contain/alter for first boot without restore

- **fstab.** Stock 7B367 fstab is two-partition (`$SP/fw/7B367-dec/fstab`):
  ```
  /dev/disk0s1 / hfs ro 0 1
  /dev/disk0s2 /private/var hfs rw,nosuid,nodev 0 2
  ```
  The 3.1.3 builder collapses this to a **single read-write root** because the synthetic NAND is one HFSX volume with no data partition: `imgtools/build_nand.py:69-70` (`FSTAB_RW = "/dev/disk0s1 / hfs rw 0 1\n"`), applied at `:333-336` (`--no-fstab` keeps the stock two-partition form). Do the same for 3.2.
- **Data partition is not required for first boot.** The 3.2 read-only rootfs already ships a `/private/var` skeleton — `db/timezone/localtime`, `log/asl`, `log/kernel.log`, `mobile/Library/Preferences/.GlobalPreferences.plist`, `mobile/.forward`, `msgs/bounds`, `root/Library/Preferences/{.GlobalPreferences,com.apple.stackshot}.plist` (volume `Wildcat7B367.K48OS`, via `$SP/tools/hfslist.py --grep private/var`). With the single-rw-root fstab, `/private/var` is just a directory on the system volume, so a missing/empty data partition is tolerated and `restored_external` (normally populates `/private/var` at restore) is not needed. (The restore ramdisk carries `restored_external`, `asr`, `ioflashstoragetool` — `$SP/fw/7B367-dec/rd_*`, `rd_options.plist SystemPartitionSize=1000` — but that is the restore path, which the golden-image approach bypasses.)
- **Keybag/EMF.** Because everything is on one plaintext HFSX volume and there is no separate encrypted data partition, the 3.2 build operates keybag/EMF-free exactly as 3.1.3 does (no `/private/var` class-key protection to satisfy).
- **SpringBoard launchd environment.** Job = `com.apple.SpringBoard` (`$SP/fw/7B367-dec/com.apple.SpringBoard.plist`, MachServices incl. `com.apple.CARenderServer`). QuartzCore honors `CA_ENABLE_OGL`, `CA_DISABLE_RENDER`, `CA_AUTO_FLUSH`, `CA_DISABLED_EXTENSIONS`, `CA_DISABLE_WORKAROUNDS` (`$SP/fw/7B367-dec/QuartzCore.bin` strings). The 3.1.3 bake sets `CA_ENABLE_OGL=LK_ENABLE_OGL='1'` and edits `DYLD_INSERT_LIBRARIES` in this job via `imgtools/patch_launchd_env.py`/`bake-guest-tools.sh:67-85`. Editing the launchd plist bytes needs no re-sign (plist, not Mach-O), but the plist must still fit its one 4096-byte allocation block (`patch_launchd_env.py:143`).

## 4. Recommended combination + guest-side modification list with signing implications

**Emulator-side (no image edits, mirrors 3.1.3):**
1. Boot-args string `amfi_allow_any_signature=1 cs_enforcement_disable=1` injected into `boot_args.CommandLine`+0x38 (DRAM scan for `virtBase=0xC0000000, physBase=0x40000000` over `0x40000000+`).
2. Force `debug-enabled`: write `0x00000001` to phys `0x402787b8` on the early repeated timer (guaranteed), and/or set `chosen/debug-enabled=1` in the DeviceTree (clean, but confirm iBoot doesn't overwrite). **This is the mandatory addition over the 3.1.3 recipe.**
3. If launching injected third-party apps: patch 3.2's `mac_proc_check_get_task{,_name}` prologues to `movs r0,#0; bx lr` (VAs TBD — not found).

**Guest-side (in the rootfs image) and signing implications:**
| Modification | File | Re-sign? |
| --- | --- | --- |
| Single-rw-root fstab | `/private/etc/fstab` | No (plain text) |
| Replacement GLES engine (armv7 gli* shim) | `/System/Library/Frameworks/OpenGLES.framework/GLEngine.bundle/GLEngine` (loaded by `OpenGLES` via the `__GLIFunctionDispatchRec`/`GLESGetEGLInterface` dispatch — `$SP/fw/7B367-dec/OpenGLES.bin` strings; OpenGLES 1.6.6, `OpenGLES.Info.plist`; SGX driver `IMGSGX535GLDriver` v38.10, `IMGSGX535GLDriver.Info.plist`) | **Yes — `ldid -S`** (AMFI hash-checks the bundle at load; unsigned/mismatched pages fault) |
| SpringBoard launchd env (`CA_ENABLE_OGL=1`, `DYLD_INSERT_LIBRARIES`, optionally `CA_DISABLE_RENDER`) | `/System/Library/LaunchDaemons/com.apple.SpringBoard.plist` | No (plist) |
| Guest agent + typing/inject dylibs | `/usr/local/bin/*`, `/usr/lib/*.dylib` | **Yes — `ldid -S`**, and root-owned (uid 0) or iOS won't exec (`bake-guest-tools.sh:26-32`) |
| Activation record (open item) | `/private/var/root/Library/Lockdown/` (`ActivationState=Activated`) | plist/record; no Mach-O re-sign |
| Extra LaunchDaemon for the agent | `/System/Library/LaunchDaemons/com.qemu.*.plist` | No (plist) |
| (Install path only) MISValidateSignature stub | `dyld_shared_cache_armv7` (offset TBD) + `installd` + SpringBoard `applicationSignatureState` | shared-cache patch (no standalone re-sign); standalone SB re-sign preserving 8 entitlements |

Any binary edited on disk (patched or foreign) needs `ldid -S` + `cdverify.py`; the emulator's `amfi_allow_any_signature` covers the invalid top-level signature, `debug-enabled=1` unlocks that flag, and the re-sign covers per-page hashes. All owner/permission repair from `bake-guest-tools.sh`/`setowner.py` applies (files created in a `noowners` host mount land uid 99 and won't exec).

## Explicitly NOT found / unverified
- iBoot-817.28's exact `debug-enabled` derivation and whether it overwrites a DT-edited value (iboot.dis literal pools mis-decoded; `movw/movt` refs not annotated). → recommend the kernel-global write as the guaranteed path.
- 3.2 kernel VAs for `mac_proc_check_get_task` / `mac_proc_check_get_task_name` (task-port gate for launching injected apps).
- The exact 3.2 `dyld_shared_cache_armv7` offset of the `MISValidateSignature` stub and the 3.2 `installd`/`applicationSignatureState` byte sites (only the 2.1.1/3.1.3 sites are documented in `README-appsync.md`).
- How the working 3.1.3 image satisfies activation — no repo mechanism exists; presumed pre-activated in off-tree golden-image prep. No activation record in the 3.2 read-only rootfs.
- Whether 3.1.3's xnu-1357 AMFI has the `PE_i_can_has_debugger` gate (inferred absent because the 3.1.3 setup carries no `debug-enabled` handling; not disassembled).
- mediaserverd/SpringBoard sandbox profile contents (not parsed) — assumed not to block the in-process, MMIO-based GLES shim.