> Archived 2026-09-28: the iPod backport plan; the backport is done (LightTouchMac `docs/STATUS.md`, "iPod touch 2G emulation" and "Guest tools without SSH").

# Backporting the iPad 1 line to the iPod touch 2G (3.1.3 / 7E18)

Branch `ipod-backport`, forked from `ipad1` at 7d1446fd5c. `git log ipad1 ^ipod_touch_2g` has ~360
commits, but main already contains a scrubbed ipad1 up to 2026-09-27 17:20 (64e7a08c50). Most of those
commits are on main under other hashes, so most shared code is already in the iPod build. The
table groups the commits by what they touch.

Classes: **a** = already applies to the iPod (shared code, or already on main/this branch); **b** =
iPad-only by nature; **c** = applicable to the iPod and not yet ported. Everything was measured on
nand-current.new through throwaway overlays, and neither image was written. No guest-side change
was needed, so no private test image was built.

## Audit

| Item | Class | Evidence | Status / commit |
|---|---|---|---|
| A4 CDMA AES: blocks straddling page segments (4852551a77, KAT it-cctest) | b→**c (iPod analog, worse)** | 3.1.3's `AppleS5L8900XAES` (register sequence traced with `IT_AES_DEBUG`, protocol read from the 2.1.1 kext's disassembly, where it is identical) uses 0x28/0x2c as the input segment, 0x20/0x24 as the output segment, 0x18 as the total, KEYLEN bit 0 as encrypt, 0x10 as the IRQ enable, status bits 2/4 to ask for more segments, GO=3 to resume, and a 0x08 pulse to reset. The model decrypted 0x20 into 0x28 regardless of direction and had no IRQ. Guest KAT (it_cctest armv6): 21 of the first 512 cases wrong (every 16-byte-aligned AES case >= 1440 B), then a hang at 8 KiB, because the process waited on IRQ 0x27 forever. First fix attempt: kernel abort NULL+0x20, because enabling the IRQ fired the previous polled request's done bit. Modeling the 0x08 reset fixed that | **3eb6b188d9** (merged into ipad1 as 29f86fd4ee). KAT 652/652, `tests/ipod/test_aes.py` segmented case vs OpenSSL, new `tests/ipod/cctest_guest.py`. UID/GID ops are deliberately unchanged, because keys derived from them protect keychain items on existing images |
| TLS 1 KiB record mitigation reverted in it-webproxy (8a933dbd49) | a (verified) | 342 KB over native TLS 1.0 with 16 KiB records passes 3/3 on this tree **and** on main's shipping binary (old AES model). 3.1.3's SecureTransport doesn't take /dev/aes_0's aligned path, so the AES bug hit apps calling CCCrypt directly, not HTTPS | test extended, **296f26c643** |
| GL shim `gliBindViewES(NULL)` must unbind the old drawable (0518e55565) | a | The iPod's `mbxshim.c` `GLESBindView` already calls `ca_detach_view` for NULL, which presents any acquired buffer and then calls `vt[2]` unbind (7E18 `_DetachTexture`). No change needed | n/a |
| iPod app failures from the nand-current.new pass | not emulator bugs | Found with syslog capture (next row). **Cube Runner (rc333), iTransitBuddy, Super Monkey Ball (rc333):** the executable is mode 0644 in the zip; stock installd keeps it, and launchd logs `posix_spawn(...): Permission denied`. The well-packed Cube Runner (Clutch, 0755) and Monkey Ball (f53ed…, 0777) both PASS. **Tap Tap Revenge 3 Boost:** its "armv6" slice is byte-identical to the armv7 slice (Thumb-2), so the ARM1176 raises SIGILL (a broken crack). **KP by Bing:** `DTSDKName = iphoneos4.1`, which 3.1.3's SpringBoard refuses ("Unknown application display identifier"). **Magneto:** needs `_kCLErrorHeadingFailure` (3.2) | documented; a real iPod behaves the same |
| iPad app-compat: syslog-verified launch, fresh idevicesyslog right before the launch (a249b6c234, 3fd2d968b9, c591aa172e) | c | The iPod pass had "no crash log" for 4 apps with no cause recorded | **4380bc9f25**: `--launch-stages` captures syslog and appends the guest's reason (posix_spawn, unknown bundle, Symbol not found, signal, exception). app_ab tables show it |
| Stale IOSurface texture refresh for ES 2.0 programs (796b094f69) | a | Shared `gles-host.c`. The ES1 path (the only one on MBX) already refreshed enabled targets | on main |
| Live GL state save/restore, snapshots on GL (acc5e8e9d7, 441d2423ec, 3821ac9a6f) | a | Shared host code; `tests/ipod/test_snapshot_guest.py` saves a live GL scene | on main |
| GL call batching (644b255f64) | c, declined | The mbxshim hook exists behind `-DGLES_BATCH`, but the iPad measured 48 → 49 fps (20b0479ec6). A shim rebuild isn't worth that | not ported |
| Unimplemented GL slots named in the host log (ff5fdc850a) | a | mbxshim's `w()` already goes to QEMU's stderr via `GLES_OP_LOG`, and regress reads qemu.log. The check's "(no log source carried the slot trace)" note was stale | wording fixed, **f62cd8a178** |
| CA texture uploads BGRA 8888_REV, Apple row bytes (d4e074b334); ES1.1 gap fills | a | shared host | on main |
| Reinstalling an identical IPA keeps the stale SBApplication (3.2 `-[SBApplicationController loadApplications]`, docs/ipad1/app-compat.md) | a (does not reproduce) | 3.1.3, Harness: install then reinstall then launch → frontmost; install, launch, Home, reinstall, launch → frontmost. Stock 3.2 behavior, not 3.1.3's (at least not for Harness). The Mac app is unchanged | documented |
| regress Boot: a default overlay starts empty (6ca11894c2) | a | `tests/ipod/regress.py` already `rmtree`s and recreates `--out/overlay` on every run; app_ab uses one out dir per pair | already so |
| boot-smoke `--overlay/--unlock/--powerdown/--no-rescan`, tearcheck, respcheck, animfps, gl-drive (tests/ipad1) | b (as written) | Bound to the K48 panel, the ipad1 kboot and the checkpoint store. The iPod gate already covers boot → unlock → powerdown → reboot → fsck (regress boot/applaunch/persist/fsck) and snapshots (test_snapshot_guest) | not ported. An iPod tearcheck would need an iPod capture driver (hours, not minutes) |
| Orientation: iPad upright mislabeled as upside-down (eb5d4e5c58, 9a465322bb, f08defc9f9) | a (iPod correct) | Harness tilt on this tree with accel-pose upright: roll 0 → y = -1.14 (Portrait), 180 → y = +1.16, 90 → x = -1.18, -90 → x = +1.16, all UIKit-correct. The iPad's cause was its flipped mount (`mount-flipped`, set only by ipad1.c) | n/a |
| Location: it_prefs restarts locationd once Wi-Fi is up; SDIO `bssid` property (fb02c372f0, 815a309f3f, deaae73c73) | b (for now) | 3.1.3's locationd is **Skyhook-only** (`CLDaemonSkyhookLocationModel`, `https://iphone-maps.apple.com/shwps/v1/wps2`), with no Apple Location Server path, so the iPad's ALS responder can't answer it and the iPod has no Wi-Fi location. The restart matters only once a Skyhook WPS (XML) responder exists. The `bssid` property is in the shared SDIO model already | not ported; needs a Skyhook responder first |
| Keyboard: Shift/Option keycodes (e80cc3116b) | a | On main (2752fed89b). The iPod key handler consumes `Q_KEY_CODE_SHIFT(_R)` (`kbd_shift`, `ipod_touch_2g.c`). Option has no role on the iPod path | on main |
| AppSync: symbol-located shared-cache patch (0dd50242c2, `imgtools/appsync_cachepatch.py`) | c → hand to ipod-ipsw | On stock 7E18 `dyld_shared_cache_armv6` it resolves `_MISValidateSignature` by LC_SYMTAB to **0x1750ef8**, `80b500af`, which is exactly the iPod's hand-found patch site (README-appsync.md / `qemu-ios-files/ssh/patch-cache.sh`) | verified. Builder change left to ipod-ipsw |
| AppSync dylib into installd for 3.1.3; iPod recipe restores stock installd/SpringBoard (e94b811c75, 5d6ec5699f, ae2c7cb939) | a | nand-current.new is built this way (docs/ipod/nand-current-new-verification.md) | on this branch |
| ffmpeg guard, patched-FFmpeg configure, ccninja (bfa79184e7, 2f01ffa4de, 2f067ecc61, 8b8d36f923) | a | build/test infrastructure | on main |
| exynos4210_uart full FIFO reads back empty (e4b44b0862); iPod LCD composes from plane registers (5fdfbfa462); MIPI-DSI lane-aware (a0b69a5d03); pasteboard channel (78c47db1f0) | a | shared models | on main |
| regress wifi needs the kernel console (6020123399, cfa0c31fad) | a | iPod harness | on main |
| A4 SoC: IOP/NAND HLE, CDMA, H2FMI, PKE, SHA-1 @0x80100000, PMGR, display pipe/DP_FLAGS, D1815, LTC4099, TCA6408, HDQ gauge, AK8973 compass, Mikey/I2S/mic, BCM4329-over-IOP, USB host + CCK keyboard, USB Ethernet/it_ethlink, real iBoot, battery, sleep/wake | b | S5L8930/K48 hardware | — |
| iPad images: kboot/nand/rootfs/seal/device builders, manifests, 7B367 and 8C148 firmwares, activation, it_msmquiet, it_notip, BTServer off, PAC, GLI shim (3.2 GLEngine) | b | 3.2/4.x userland | — |

Per-change regression (`tests/ipod/regress.py --qemu build/qemu-system-arm --base-nand nand-current.new`):
- Baseline, ipad1 at 7d1446fd5c: 7/8, with `audio` failing ("Harness did not start stereo PCM"). Rerun alone it passed, so it's a flake.
- After 3eb6b188d9 (AES): 8/8. Also 12 parallel boot-only runs, 6 on this build and 6 on main's binary, all passed at load ~25.
- After 4380bc9f25 and 296f26c643 (harness only): app_ab over 7 IPAs, and the TLS guest test passes.
- After f62cd8a178: `--checks gles` PASS with the new wording. Final HEAD: 8/8.
- The iPad isn't affected: none of these touch code the ipad1 machine instantiates, so `tests/ipad1/fresh-device.sh` wasn't needed.

Four app_ab boots once sat on the boot logo (lit=9852) in one batch. Both binaries passed the
side-by-side boot A/B above, so this was transient. The likely cause is another agent's app_ab runs
using the same default port bases (1600+10k): regress's `free_port` check races with other
processes binding.

## Notes for ipod-ipsw

The "device from declared inputs" plan now belongs to the ipod-ipsw agent. These are the inputs
behind nand-current.new (lineage `nand-agent-v4`) that aren't stock 7E18, as met in this audit:

- **Boot chain:** `ios3/iBoot.bin` loaded directly (`IT_DIRECT_IBOOT`); `ios3/nor_7E18.bin`, whose SysCfg (Mod#, Regn, SrNm, Batt) was edited by `patch_syscfg.py` and whose SHSH is wrapped under the emulated UID (`build_nor.py`); `bootrom_240_4`; the GID-KBAG plaintext table in `hw/arm/ipod_touch_aes.c` (per build).
- **AppSync:** the shared-cache `MISValidateSignature` patch is currently at a hand-found offset (0x1750EF8, `ssh/patch-cache.sh`). `imgtools/appsync_cachepatch.py` finds the same site by symbol on stock 7E18 and can replace it. Also `/usr/lib/libappsync.dylib` plus `DYLD_INSERT_LIBRARIES` in `com.apple.installd.plist` (`contrib/appsync/patch-appsync-dylib.sh`).
- **GL:** `MBXGLEngine` is replaced by `contrib/it-gles/MBXGLEngine` (the shim has no `LC_CODE_SIGNATURE`).
- **Baked guest tools** (`imgtools/bake-guest-tools.sh`): it_agent, the typing bridge, sblaunch, launch helpers, and the ownership repair that `noowners` mounts need.
- **Volume:** grown to 1,835,008 blocks (`grow_volume.py`/`patch_gpt.py`). `patch_gpt.py` imports ftlmap from a hard-coded `<since-removed worktree>/imgtools` path, which should be `dirname(__file__)`. The lineage also carries pairing/activation state in `/private/var` from its prepared history.
- **Obsolete byte patches** (no longer on nand-current.new): `patch_springboard.py` (0x17D1C), installd 0x9F34/0x605C, and the 2.1.1-only `patch_codesign_gate.py`/`patch_libmis.py`. `patch_launchd_env.py` and `patch_syscfg.py` are structural (name/record lookups) and don't depend on offsets.

## What's left

- A Skyhook WPS responder, if the iPod should get Wi-Fi location. After that, check whether 3.1.3's locationd also starts before Wi-Fi and needs the it_prefs-style restart.
- An iPod tearing/latency driver equivalent to tests/ipad1/tearcheck.py and respcheck.py, if wanted.
- Give app_ab per-run port bases distinct from other agents' (or a lock), so parallel batches can't share ports.
