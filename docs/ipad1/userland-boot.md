# iPad 1 / 7B500: first userland attempt (root mount → launchd → SpringBoard)

What the emulator needs the moment the kernel says `BSD root: disk0s1`, how it was built, and what to watch on
serial. Built by `imgtools/ipad1_rootfs.py`, consumed by `imgtools/ipad1_nand.py`. Everything is under
`~/Developer/qemu-ios-files/ipad1/` (`FILES` below); nothing here has been booted yet.

## Ready to boot (2026-09-26): two stores

| store | base | boot-args | when |
|---|---|---|---|
| `FILES/userland/nand-pristine` | IPSW `rootfs.dmg` (`Wildcat7B500.K48OS`), nothing jailbroken | `-v serial=3 debug=0x8` (kboot default) | **first boot target** |
| `FILES/userland/nand-jb` | captured jailbroken partition + real `/var/stash` | `-v serial=3 debug=0x8 amfi_allow_any_signature=1 cs_enforcement_disable=1` | when sshd/bash/Cydia are wanted (needs a network first) |

Both pass `ipad1_nand.py check` (3.4 GiB each; MBR from the unit, system at LBA 63, data at LBA 327789 as
plain Apple_HFS 0xAF). Their inputs sit next to them: `FILES/userland/{pristine,jailbroken}/system.img`,
`data.img`, `unsigned-machos.txt`.

```
tests/ipad1/boot-smoke.py --nand-clone FILES/userland/golden-pristine --seconds 240   # golden is read-only; the IOP mmaps the store MAP_SHARED, so never boot it in place
```

The `bsd` (`BSD root:`) and `launchd` markers already exist in the test. Kernel bundle prerequisites are all in
`ipad1_kboot.py` now: `root-matching` names partition 1, `chosen/debug-enabled = 1`, the real unit's identity
(serial `EMU000000000`, MLB, ECID `0x0000000001` = 1, die-id), and the `sgx` node disabled
(9ed863257d).

Why the AMFI flags on the jailbroken store only: `sshd`, `bash`, Cydia, Substrate and ~180 GNU tools are
`ldid -S` signed (CodeDirectory, no CMS blob), which `AMFI_vnode_check_signature` (0xc03b0718) rejects unless
`+0x68 allow_any_signature` is set. Page hashes are intact, so nothing is re-signed. `AMFI::start` reads the
flags only when `PE_i_can_has_debugger()` is true, i.e. DT `debug-enabled != 0`, which kboot forces (7B500
global VA 0xc02787b8 / PA 0x402787b8 if a DRAM poke is ever needed). Every Apple daemon and app on both
volumes carries an Apple signature, so a boot without the flags still reaches SpringBoard; only the jailbreak
additions fail with `AMFI: Invalid signature`. Not needed: `amfi_get_out_of_my_way`,
`amfi_unrestrict_task_for_pid`, the `mac_proc_check_get_task{,_name}` patches (0xc01d4614 / 0xc01d46ac; only
for launching injected apps), `rd=`.

### Regenerate (about 15 s per base for the volumes, about 1 min per store)

```
imgtools/ipad1_rootfs.py fetch                          # once: FILES/hw2/stash (116 MB), FILES/hw2/lockdown; iPad on USB
imgtools/ipad1_rootfs.py build --base pristine          # -> FILES/userland/pristine/{system,data}.img
imgtools/ipad1_rootfs.py build --base jailbroken        # -> FILES/userland/jailbroken/...
imgtools/ipad1_nand.py build --mbr FILES/hw2/rdisk0-head4M.bin --system FILES/userland/pristine/system.img \
                             --data FILES/userland/pristine/data.img --out FILES/userland/nand-pristine
imgtools/ipad1_nand.py check FILES/userland/nand-pristine --mbr FILES/hw2/rdisk0-head4M.bin --system FILES/userland/pristine/system.img
```

(`build` prints the matching `ipad1_nand.py` line; the jailbroken store is `nand-jb`.)
`ipad1_rootfs.py --selfcheck` runs on every invocation: APM slicing, plist edits, owner rule, signature classifier.

## What is on the volumes and why

### system.img

**pristine**: the Apple_HFSX slice of `7B500/dec/rootfs.dmg` (UDIF + APM; 127995 × 8 KiB blocks = 1000 MB),
grown to partition 1's 1280 MiB. `hdiutil resize` stops one 4 KiB sector short with 8 KiB blocks, so the tool
pads the file and moves the alternate volume header to the new end − 1024. Apple's catalog uid/gid/modes are
untouched; `/Applications` holds the 17 stock apps.

**jailbroken**: `hw2/rdisk0s1-system.img`, already partition-sized (163840 blocks, 76925 free). It symlinks
`/Applications`, `/usr/libexec`, `/usr/share`, `/usr/include`, `/usr/lib/pam`, `/Library/{Ringtones,Wallpaper}`
into `/private/var/stash`, its own `/private/var` holds only `db`, and `/Library/LaunchDaemons` adds
`com.openssh.sshd` and `com.saurik.Cydia.Startup`. Apple's `/System/Library/LaunchDaemons` (62 jobs) is
byte-identical to the IPSW's.

Edits, identical for both bases, through one read-write mount, no Mach-O touched, plists rewritten in place in
their binary format so the catalog record and uid 0 survive:

| edit | value | why |
|---|---|---|
| `/private/etc/fstab` | `/dev/disk0s1 / hfs rw 0 1` / `/dev/disk0s2 /private/var hfs rw,nosuid,nodev 0 2` | pristine said `ro` root + `disk0s2`; captured said `rw` + `disk0s2s1` (the EncryptedMediaFilter subslice). `ipad1_nand.py` publishes the data partition as plain 0xAF, so `disk0s2` mounts with no 0x89B key or `tprc` block. `rw` root is insurance for a failed data mount (`--ro-root` for stock). |
| `.../com.apple.SpringBoard.plist` | `EnvironmentVariables` += `CA_ENABLE_OGL=0`, `MBX2D_PAGE_FLIP=0`; `StandardOutPath`/`StandardErrorPath` = `/dev/console` | GL doc §1.3/§1.5: a failed `_eagl_init` is never cached, so without `CA_ENABLE_OGL=0` SpringBoard redoes dlopen(GLEngine) + IOAcceleratorES + AppleMBXDevice matching on every render; `MBX2D_PAGE_FLIP=0` leaves one IOMFB page so the scaler-only page copy is never attempted. `/dev/console` is `crw--w--w-` on the unit, so `mobile` can append and SpringBoard's stderr rides the serial console. |
| `--disable LABEL` | `Disabled=true` (searched in `/System/Library` and `/Library` LaunchDaemons) | none applied by default; see knobs. |

**sshd (jailbroken only)**: `/Library/LaunchDaemons/com.openssh.sshd.plist` is kept as is: inetd-style
(`Sockets` service `ssh`, `sshd -i`), `Program /usr/libexec/sshd-keygen-wrapper` (in the stash), host keys in
`/etc/ssh`, root password `alpine`. Needs a network, i.e. M5's USB + usbmuxd/iproxy or an emulated NIC; until
then launchd just holds the socket. No agent job was added: a guest agent needs armv7 Mach-Os built under the
3.2 constraints in `hw2-regs/README-native-code-on-3.2.2.md` (dylib, `-marm`, no `LC_MAIN`) and a transport.

**MobileSubstrate (jailbroken only)** is injected via `/private/etc/launchd.conf`
(`bsexec .. /usr/bin/cynject 1 .../SubstrateLauncher.dylib`); `cynject` is known broken on the unit
(`MSHookProcess() failed`) and will fail the same way here, harmlessly. `--disable com.saurik.Cydia.Startup`
covers the Cydia job; `launchd.conf` can be emptied with one more line in `build()` if it spins.

### data.img: what `mobile_obliterator` would create

Fresh journaled HFSX "Data" (`ipad1_nand.make_hfs_image`), 2 GiB (real p2 is 14 GB; any size ≤ that works,
`ipad1_nand.py` sizes the partition to the image). Seeded with:

1. `/private/var` skeleton from the IPSW rootfs (for the jailbroken base it is sliced into a private temp
   image, since that volume's own `/private/var` is just `db`): `db/launchd.db`, `db/timezone/localtime`,
   `log/asl`, `mobile/Library/{Preferences,Caches,Keyboard,...}`, `mobile/Media/{DCIM,Photos}`, `run`, `tmp`,
   `msgs`, `root/Library/Preferences`, ...
2. jailbroken only: `/private/var/stash` from the real iPad (116 MB: `Applications` with the 17 stock apps +
   Cydia, `libexec` with all 51 Apple daemons plus OpenSSH's and Substrate's helpers, `share`, `include`,
   `pam`, `Ringtones`, `Wallpaper`).
3. `/private/var/root/Library/Lockdown` from the real iPad: `activation_records/pod_record.plist`,
   `data_ark.plist` (`ActivationState = Activated`), `device_{private,public}_key.pem`, three `pair_records`.

Ownership as on the unit (`ls -ln /private/var`): `mobile/`, `ea/` 501:501, everything else 0:0. The host
mount is `noowners`, so the catalog is patched offline afterwards (`build_nand.set_owner`; 64 records
pristine, 3562 jailbroken).

### unsigned-machos.txt

Classifier: every Apple 7B500 binary carries a CMS slot (0x10000) in its signature SuperBlob, even an 8-byte
empty one (`launchd`, `mediaserverd`); `ldid -S` output has CodeDirectory + empty requirements and no CMS
slot. Pristine: 8 `none`, all inert (7 `AppleMultitouchSPI*.kext/AppleMultitouch` firmware payloads, one
`.o`), 0 `adhoc`. Jailbroken: the same plus `libgmalloc.dylib`, and 188 `adhoc` (OpenSSH, bash, coreutils,
gzip, tar, apt/dpkg, Cydia, Substrate, `MSUnrestrictProcess`, ...). `ipad1_rootfs.py report DIR` runs the
classifier over any tree.

## Serial checklist, in order

1. `BSD root: disk0s1, major 14, minor 1` — root via `root-matching` (partition 1), as with the
   self-formatted store today.
2. launchd lines. It runs `fsck`+`mount` for fstab: expect `/dev/disk0s2` to be probed. **First model
   dependency:** the IOP/FTL read path must serve partition 2's LBAs (327789–852032); `check` confirmed the
   HFS header there. If `/private/var` fails to mount everything below still starts on the rw root skeleton,
   but on the jailbroken store `/var/stash` is missing and `lockdownd`, `installd`, ... are `No such file`: that
   symptom means "data partition", not "launchd". On the pristine store the difference is only that
   `/var/mobile` is the read-only-volume copy.
3. The 62 (+2) jobs loading. Watch for `AMFI: Invalid signature but permitting execution` (jailbroken store,
   expected), and `Exited with exit code:` / `Throttling respawn:` storms (5–10 s) from hardware-facing
   daemons: `wifiFirmwareLoader`, `BTServer`, `CommCenter`, `locationd`, `mediaserverd`, `iapd`, `accessoryd`,
   `fairplayd.K48`, `securekeyvaultd.s5l8930x`. Noisy, not fatal.
4. `SpringBoard` stderr on the console. Expect it to come up as `mobile` (`HOME=/var/mobile` exists), publish
   `com.apple.CARenderServer`, `PurpleSystemEventPort`, `com.apple.springboard.*`, look for `AppleCLCD`
   (`H3CLCDDisplay::open`; until M4's display pipe publishes it with 1024×768 it logs no display and keeps
   running), and not touch GLES (`CA_ENABLE_OGL=0`, no `sgx`). A crash loop here (KeepAlive,
   `ThrottleInterval 5`) is the M2 exit signal to diff against the unit's log.
5. `lockdownd`: activation state from `pod_record.plist`; see open items. `Unactivated` still shows the lock
   screen once there is a display.
6. `sshd` (jailbroken): nothing until a network exists; a `Sockets` bind failure line is expected.

## Knobs

- `--base pristine|jailbroken` (default pristine); `--rootfs`/`--stash`/`--lockdown` override a base's inputs.
- `--disable LABEL` (repeatable): candidates once seen spinning are the daemons in step 3 and
  `com.saurik.Cydia.Startup`. Start stock and read the log first.
- `--data-size`, `--lockdown none`, `--ro-root`.
- Later Mach-O additions (GLI shim at `GLEngine.bundle/GLEngine`, guest agent) go through the same mount in
  `build()`: `ldid -S` them (preserve entitlements when replacing an Apple binary, see `README-appsync.md`),
  list them for `set_owner(..., 0, 0)`, and add the two AMFI boot-args to the pristine store too.
- Do not use `patch_launchd_env.py` here; it edits the iPod's page store in place and assumes a single-block
  plist.

## Not determined / open

- **Activation identity.** `pod_record.plist`'s AccountToken is bound to the unit; `lockdownd` checks it
  against the UDID = SHA1(serial + ECID + Wi-Fi MAC + Bluetooth MAC) on 3.x. All four are known now: serial
  `EMU000000000`, ECID 1 (`0x0000000001`), Wi-Fi MAC `02:00:00:00:00:01` (`local-mac-address`,
  also the tail of nvram `platform-uuid` `00000000-0000-1000-8000-020000000001`), Bluetooth MAC
  `02:00:00:00:00:02`; die-id 2233827018196609712. kboot carries serial/MLB/ECID/die-id since 571f433ec3.
  **The MACs are not placed anywhere yet — check where iOS 3.2 reads the Wi-Fi/BT addresses (NVRAM vs DT
  vs the chip):** on this unit `local-mac-address` is a property on an IOService node (the IOKit dump), and
  nvram has only `platform-uuid`; whether the DT `wlan`/`bluetooth` nodes carry them, or `lockdownd`'s
  `WiFiAddress` comes from the Broadcom driver reading OTP, decides whether activation can validate before
  M5's Wi-Fi/BT models exist. Until then expect `Unactivated`.
- **`/dev/console` redirect** is untested on this launchd; if it refuses, fall back to a file under `/var/log`
  and read it back through the store.
- **No serial login shell** exists regardless of `/etc/ttys`: there is no `getty` on 7B500. A shell needs
  sshd + network or a custom daemon (M5).
- **Whether launchd retries** a data partition that appears late (IOP FMI init racing launchd) was not
  checked; if `/private/var` is missing in the log while the FTL is fine, look at ordering.
- The `AppleMultitouch` plug-in blobs and `libgmalloc.dylib` are flagged `none` by the classifier; not executed
  at boot, not investigated further.
