# iPad 1 / 7B500: first userland attempt (root mount → launchd → SpringBoard)

What the emulator needs the moment the kernel says `BSD root: disk0s1`, how it was built, and what to watch on
serial. Built by `imgtools/ipad1_rootfs.py`, consumed by `imgtools/ipad1_nand.py`. Everything is under
`~/Developer/qemu-ios-files/ipad1/` (`FILES` below); nothing here has been booted yet.

## Ready to boot (2026-09-26)

```
FILES/userland/nand-userland/    ipad1_nand.py store: MBR + system.img + data.img; `check` all green (3.4 GiB)
FILES/userland/system.img        the captured jailbroken 3.2.2 system partition, 1280 MiB HFSX, three edits
FILES/userland/data.img          2 GiB journaled HFSX "Data": /var skeleton + /var/stash + Lockdown dir
FILES/userland/unsigned-machos.txt   197 Mach-Os with no Apple signature (188 ldid ad-hoc, 9 unsigned)
FILES/hw2/stash, FILES/hw2/lockdown  pulled off the real iPad by `ipad1_rootfs.py fetch`
```

### Boot-args for the first attempt

```
-v serial=3 debug=0x8 amfi_allow_any_signature=1 cs_enforcement_disable=1
```

- `-v serial=3 debug=0x8`: `ipad1_kboot.DEFAULT_BOOT_ARGS`, kernel console on UART0.
- `amfi_allow_any_signature=1 cs_enforcement_disable=1`: required with this system image. `sshd`, `bash`,
  Cydia, Substrate and 180-odd GNU tools are `ldid -S` signed (CodeDirectory, no CMS blob), which
  `AMFI_vnode_check_signature` (0xc03b0718) rejects unless `+0x68 allow_any_signature` is set; page hashes
  are valid, so nothing needs re-signing. Both flags are read in `AMFI::start` only when
  `PE_i_can_has_debugger()` is true, i.e. DT `chosen/debug-enabled != 0` — `ipad1_kboot.fill_dt` already forces
  it to 1 (and the 7B500 global is VA 0xc02787b8 / PA 0x402787b8 if it ever needs the iPod-style DRAM poke).
  `amfi_get_out_of_my_way=1` is the bigger hammer (skips validation entirely); not needed.
- Not needed yet: `amfi_unrestrict_task_for_pid`, the `mac_proc_check_get_task{,_name}` patches
  (0xc01d4614 / 0xc01d46ac; only for launching injected apps), `rd=` (root-matching names partition 1).

Run it: `tests/ipad1/boot-smoke.py --nand FILES/userland/nand-userland --seconds 240 --args "<the line above>"`
(`--args` rebuilds `k48-kboot.bin`; the `bsd` and `launchd` markers are already in the test).

### Device-tree edit for the first attempt: drop `sgx`

`ipad1_kboot.DeviceTree` cannot delete a node (iBoot-style in-place edit), so neutralise the match instead, in
`fill_dt`:

```python
dt.set("arm-io/sgx", "compatible", "none")      # was 'sgx,s5l8930x\0sgx,s5l8920x' (26 bytes; "none" fits)
```

With no `compatible` match, `IMGSGX535` never loads, `IOAcceleratorES` is never published, and OpenGLES'
`_eagl_init` falls through cleanly (userland-gl-display.md §1.5). Leave `vxd`/`venc` alone for now; they only
matter for video. This edit is not made by the tools yet — it belongs in `ipad1_kboot.py`, which I did not touch.

### Regenerate (about 15 s for the volumes, about 1 min for the store)

```
imgtools/ipad1_rootfs.py fetch                       # once; iPad on USB, iproxy 2323 -> 22 (ipad-ssh.sh)
imgtools/ipad1_rootfs.py build                       # -> FILES/userland/{system,data}.img, unsigned-machos.txt
imgtools/ipad1_nand.py build --mbr FILES/hw2/rdisk0-head4M.bin --system FILES/userland/system.img \
                             --data FILES/userland/data.img --out FILES/userland/nand-userland
imgtools/ipad1_nand.py check FILES/userland/nand-userland --mbr FILES/hw2/rdisk0-head4M.bin --system FILES/userland/system.img
```

`ipad1_rootfs.py --selfcheck` runs on every invocation (APM slicing, plist edits, owner rule, signature classifier).

## What is on the volumes and why

### system.img: the captured jailbroken partition, patched through a mount

`hw2/rdisk0s1-system.img` is already exactly partition 1 (163840 × 8 KiB HFSX, 76925 blocks free), so no
resize. It is a jailbroken volume: `/Applications`, `/usr/libexec`, `/usr/share`, `/usr/include`, `/usr/lib/pam`,
`/Library/{Ringtones,Wallpaper}` are symlinks into `/private/var/stash`, `/private/var` itself holds only `db`,
and `/Library/LaunchDaemons` has `com.openssh.sshd` and `com.saurik.Cydia.Startup`. Apple's
`/System/Library/LaunchDaemons` set is byte-identical to the IPSW's (62 jobs; SpringBoard job included).

| edit | value | why |
|---|---|---|
| `/private/etc/fstab` | `/dev/disk0s1 / hfs rw 0 1` / `/dev/disk0s2 /private/var hfs rw,nosuid,nodev 0 2` | captured fstab named `disk0s2s1` (the EncryptedMediaFilter subslice). `ipad1_nand.py` publishes the data partition as plain Apple_HFS 0xAF, so `disk0s2` mounts without the 0x89B AES key or a `tprc` block (`ipad1_nand.py` would also patch this in the page stream; the volume is now already right). `rw` root as on the captured unit; `--ro-root` for stock. |
| `.../com.apple.SpringBoard.plist` | `EnvironmentVariables` += `CA_ENABLE_OGL=0`, `MBX2D_PAGE_FLIP=0`; `StandardOutPath`/`StandardErrorPath` = `/dev/console` | GL doc §1.3/§1.5: a failed `_eagl_init` is never cached, so without `CA_ENABLE_OGL=0` SpringBoard redoes dlopen(GLEngine) + IOAcceleratorES + AppleMBXDevice matching on every render; `MBX2D_PAGE_FLIP=0` leaves one IOMFB page so the scaler-only page copy is never attempted. `/dev/console` is `crw--w--w-` on the unit, so `mobile` can append and SpringBoard's stderr rides the serial console next to the kernel's. |
| `--disable LABEL` | `Disabled=true` (searched in `/System/Library` and `/Library` LaunchDaemons) | none applied by default; see knobs. |

Plists are rewritten in place in their original (binary) format, so the catalog record and Apple's uid 0
survive; nothing new is created on this volume and no Mach-O is modified.

**sshd**: `/Library/LaunchDaemons/com.openssh.sshd.plist` is kept: inetd-style (`Sockets` service `ssh`,
`sshd -i`), `Program /usr/libexec/sshd-keygen-wrapper` (in the stash), host keys in `/etc/ssh` (regenerated
2026-09-26 on the unit), root password `alpine`. It needs a network interface, i.e. M5's USB + usbmuxd/iproxy
or an emulated NIC; until then launchd just holds the socket. No agent job was added: the guest agent needs
armv7 Mach-Os built with the 3.2 SDK constraints in `hw2-regs/README-native-code-on-3.2.2.md` (dylib, `-marm`,
no `LC_MAIN`) and a transport, both M5 work.

**MobileSubstrate** is active on this image through `/private/etc/launchd.conf`
(`bsexec .. /usr/bin/cynject 1 .../SubstrateLauncher.dylib`), and `cynject` is known broken on the unit
(memory note: `MSHookProcess() failed`). It will fail the same way in the emulator and is harmless; if it
turns out to spin, `--disable com.saurik.Cydia.Startup` handles the Cydia job and `launchd.conf` can be
emptied with one more line in `build()`.

### data.img: what `mobile_obliterator` would create, plus the stash

Fresh journaled HFSX "Data" (`ipad1_nand.make_hfs_image`), 2 GiB (the real p2 is 14 GB; any size ≤ that
works, `ipad1_nand.py` sizes the partition to the image). Seeded with:

1. `/private/var` skeleton from the IPSW rootfs (`7B500/dec/rootfs.dmg`, sliced out of its APM into a temp
   image): `db/launchd.db`, `db/timezone/localtime`, `log/asl`, `mobile/Library/{Preferences,Caches,...}`,
   `mobile/Media/{DCIM,Photos}`, `run`, `tmp`, `msgs`, `root/Library/Preferences`, ... The jailbroken volume's own
   `/private/var/db` is layered on top.
2. `/private/var/stash` from the real iPad (116 MB: `Applications` with the 17 stock apps + Cydia, `libexec`
   with all 51 Apple daemons plus OpenSSH's and Substrate's helpers, `share`, `include`, `pam`, `Ringtones`,
   `Wallpaper`).
3. `/private/var/root/Library/Lockdown` from the real iPad: `activation_records/pod_record.plist`,
   `data_ark.plist` (`ActivationState = Activated`), `device_{private,public}_key.pem`, three `pair_records`.

Ownership as on the real unit (`ls -ln /private/var`): `mobile/`, `ea/` 501:501, everything else 0:0. The
host mount is `noowners`, so the catalog is patched offline afterwards (`build_nand.set_owner`: 3562 records).

### unsigned-machos.txt

Classifier: Apple 7B500 binaries all carry a CMS slot (0x10000) in the signature SuperBlob, even an 8-byte
empty one (`launchd`, `mediaserverd`); `ldid -S` output has CodeDirectory + empty requirements and no CMS
slot. Result on this image: 188 `adhoc` (OpenSSH, bash, coreutils, gzip, tar, apt/dpkg, Cydia, Substrate,
`MSUnrestrictProcess`, ...) and 9 `none` (7 `AppleMultitouchSPI*.kext/AppleMultitouch` plug-in blobs, which
are firmware payloads not executables, `libgmalloc.dylib`, an `.o`). Everything in `/System/Library`,
`/usr/lib`, `/sbin`, the stashed `libexec` daemons and the 17 stock apps is Apple-signed. So
`amfi_allow_any_signature=1` covers exactly the jailbreak additions; a boot without it still reaches
SpringBoard (all of launchd's Apple jobs exec) but `sshd`, `bash` and Cydia's startup fail with
`AMFI: Invalid signature`.

## Serial checklist, in order

1. `BSD root: disk0s1, major 14, minor 1` — root found via `root-matching` (partition 1). Same as the
   self-formatted store today.
2. launchd banner / `launchd: ...` lines. It runs `fsck`+`mount -vat nonfs` for fstab: expect
   `/dev/disk0s2` to be probed. **First model dependency:** the IOP/FTL read path must serve partition 2's
   LBAs (327789–589932); `ipad1_nand.py check` confirmed the HFS header there. If `/private/var` fails to
   mount, everything below still starts (rw root skeleton), but `/var/stash` is missing and `lockdownd`,
   `installd`, ... are `No such file`: that symptom means "data partition", not "launchd".
3. `com.apple.launchd` loading the 62 + 2 jobs. Watch for exec failures: `AMFI: Invalid signature but
   permitting execution` (expected for sshd/bash with the boot-args), `Exited with exit code:` /
   `Throttling respawn:` storms (5–10 s) from hardware-facing daemons: `wifiFirmwareLoader`, `BTServer`,
   `CommCenter`, `locationd`, `mediaserverd`, `iapd`, `accessoryd`, `fairplayd.K48`, `securekeyvaultd.s5l8930x`.
   Noisy, not fatal.
4. `SpringBoard` stderr on the console (via `/dev/console`). Expect it to:
   - come up as `mobile` with `HOME=/var/mobile` (skeleton is there),
   - publish `com.apple.CARenderServer`, `PurpleSystemEventPort`, `com.apple.springboard.*`,
   - look for `AppleCLCD` (`H3CLCDDisplay::open`): until M4's display pipe publishes it with 1024×768,
     it logs no display and keeps running,
   - not touch GLES (`CA_ENABLE_OGL=0`, no `sgx`).
   A crash loop here (KeepAlive, `ThrottleInterval 5`) is the M2 exit signal to diff against the real
   unit's log.
5. `lockdownd`: activation state from `pod_record.plist`; see open items. `Unactivated` still shows the
   lock screen once there is a display.
6. `sshd`: nothing until a network exists; a `Sockets` bind failure line is expected without one.

## Knobs

- `--disable LABEL` (repeatable): candidates once seen spinning are the daemons in step 3 and
  `com.saurik.Cydia.Startup`. Start stock and read the log first.
- `--rootfs FILES/7B500/dec/rootfs.dmg --stash none`: the pristine IPSW volume instead (no jailbreak, no
  sshd, nothing ad-hoc signed, `/Applications` real). Boots with the plain default boot-args.
- `--data-size`, `--lockdown none`, `--ro-root`.
- Later Mach-O additions (GLI shim at `GLEngine.bundle/GLEngine`, guest agent) go through the same mount in
  `build()`: `ldid -S` them (preserve entitlements when replacing an Apple binary, see `README-appsync.md`),
  list them for `set_owner(..., 0, 0)`; the boot-args above already cover them.
- Do not use `patch_launchd_env.py` here; it edits the iPod's page store in place and assumes a single-block
  plist.

## Not determined / open

- **Activation identity.** `pod_record.plist`'s AccountToken is bound to the unit; `lockdownd` checks it
  against the UDID = SHA1(serial + ECID + Wi-Fi MAC + Bluetooth MAC) on 3.x. Known: serial `EMU000000000`,
  Wi-Fi MAC `02:00:00:00:00:01` (`local-mac-address`, also the tail of nvram `platform-uuid`
  `00000000-0000-1000-8000-020000000001`). Not captured: ECID (`unique-chip-id`; the IOKit dump walked the
  IOService plane only) and the Bluetooth MAC. `ipad1_kboot.fill_dt` writes serial `QEMUIPAD1` and a made-up
  ECID, so expect `Unactivated` until those are the real values.
- **`/dev/console` redirect** is untested on this launchd; if it refuses, fall back to a file under `/var/log`
  and read it back through the store.
- **launchd and `/etc/ttys`**: there is no `getty` on 7B500, so no serial login shell exists regardless of
  `ttys`; a shell needs sshd + network or a custom daemon (M5).
- **Whether `mount -vat nonfs` retries** a data partition that appears late (IOP FMI init after launchd
  starts) was not checked; if `/private/var` is missing in the log while the FTL is fine, look at ordering.
- The 7 `AppleMultitouch` plug-in blobs and `libgmalloc.dylib` are flagged `none` by the classifier; they are
  not executed at boot and were not investigated further.
