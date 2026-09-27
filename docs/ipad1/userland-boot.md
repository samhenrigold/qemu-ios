# iPad 1 / 7B500: first userland attempt (root mount → launchd → SpringBoard)

What the emulator needs the moment the kernel says `BSD root: disk0s1`, how it was built, and what to watch on
serial. Built by `imgtools/ipad1_rootfs.py`, consumed by `imgtools/ipad1_nand.py`. Everything is under
`~/Developer/qemu-ios-files/ipad1/` (`FILES` below); nothing here has been booted yet.

## Ready to boot (2026-09-26): two stores

| store | base | boot-args | when |
|---|---|---|---|
| `FILES/userland/nand-pristine` | IPSW `rootfs.dmg` (`Wildcat7B500.K48OS`), nothing jailbroken | `-v serial=3 debug=0x8` (kboot default until 2026-09-27; the default now adds the AMFI pair) | **first boot target** |
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

**golden-pristine** (the read-only store the app and tests clone) is the pristine store plus the guest
helpers `it_pbd` (pasteboard) and `it_ethlink` (USB Ethernet link) (docs/ipad1/guest-services.md):

```
contrib/ipad1-guest/build.sh                            # -> build/ipad1-guest/{it_pbd,it_ethlink}
imgtools/ipad1_rootfs.py build --base pristine
imgtools/ipad1_rootfs.py bake FILES/userland/pristine   # helpers + their com.qemu.* jobs, root-owned
imgtools/ipad1_nand.py build --mbr FILES/hw2/rdisk0-head4M.bin --system FILES/userland/pristine/system.img \
                             --data FILES/userland/pristine/data.img --out FILES/userland/golden-pristine
chmod -R a-w FILES/userland/golden-pristine
```

The helpers are ldid-signed, so they need the AMFI boot-args. Those are the default in `ipad1_kboot.py`
and therefore in `7B500/k48-kboot.bin`, which is a stock kernel with no patch (`--usb-eth-link` is the
old fallback). `7B500/k48-kboot-noamfi.bin` keeps the old
`-v serial=3 debug=0x8` bundle.
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

- **Activation identity (resolved 2026-09-27).** `pod_record.plist`'s AccountToken binds `SerialNumber`,
  `ProductType` and `UniqueDeviceID` = `<unit UDID>`, which is exactly
  `SHA1("EMU000000000" + "02:00:00:00:00:01" + "02:00:00:00:00:02")`: serial + Wi-Fi MAC + Bluetooth MAC,
  lowercase, colon-separated, **no ECID/IMEI** on a Wi-Fi iPad (brute-forced over the orderings and formats;
  nothing else matched). iBoot puts the two MACs (syscfg `WMac`/`BMac`) into DT `arm-io/sdio` and
  `arm-io/uart3/bluetooth` `local-mac-address` (6 bytes each, zero in the IPSW DT; the real unit's IORegistry
  shows them on the `sdio` and `bluetooth` nubs), and `ipad1_kboot.MACS` now fills both. Serial/MLB/ECID/die-id
  were already in kboot (571f433ec3). The data-volume records need no change.
- **`/dev/console` redirect** is untested on this launchd; if it refuses, fall back to a file under `/var/log`
  and read it back through the store.
- **No serial login shell** exists regardless of `/etc/ttys`: there is no `getty` on 7B500. A shell needs
  sshd + network or a custom daemon (M5).
- **Whether launchd retries** a data partition that appears late (IOP FMI init racing launchd) was not
  checked; if `/private/var` is missing in the log while the FTL is fine, look at ordering.
- The `AppleMultitouch` plug-in blobs and `libgmalloc.dylib` are flagged `none` by the classifier; not executed
  at boot, not investigated further.

## USB: behave like an iPad on a Mac (no deep sleep)

iOS only reports external power and disables idle sleep once a USB host has *configured* the device:
`AppleD1815PMUPowerSource` wants >= 500 mA from `function-usb_500_100`, which AppleSynopsysOTGDevice reports
after SET_CONFIGURATION. An unconfigured guest therefore deep-slept a few minutes after SpringBoard (the
arbitrator's power-state-0 path logs "USB cable detached", then "System Sleep" / "pmu go hib"). Two ways to
be configured:

- **No bridge (default):** the OTG model's built-in host (`hw/arm/ipod_touch_usb_otg.c`, `synopsys_host_*`)
  does what usbmuxd-qemu does — reset, enumdone, descriptors, SET_ADDRESS, SET_CONFIGURATION of the
  configuration with the AppleUSBMux interface, the string reads, the mux version request, then polls the
  bulk IN pipe — so the guest charges and stays awake; nothing is connected on the host side.
- **usbmuxd-qemu bridge:** real enumeration plus lockdown/usbmux (`idevice_id -l` sees it):

```
~/Developer/usbmuxd-qemu/usbmuxd/src/usbmuxd -f -v -S 127.0.0.1:27015 -P NONE -C <conf dir>   # listens on 1235 for QEMU
qemu-system-arm -machine ipad1,kboot=...,nand=...,usb-tcp-addr=127.0.0.1:1235 ...
USBMUXD_SOCKET_ADDRESS=127.0.0.1:27015 idevice_id -l
```

Machine properties: `usb-tcp-addr=host:port` (unset: `IT_USB_TCP`, else the built-in host) and
`usb-cable=on|off` (default on). The cable can be pulled and replugged at runtime with
`qom-set /machine usb-cable false` / `true` over QMP: the LTC4099's usb_det level flips, the PMU raises
charger0's vector (event F bit 0) and the "usb" event, the power source logs `AppleUSBCableType Detached` /
`USBHost`, and the OTG drops or redials the bridge link (usbmuxd reaps and re-enumerates) or restarts the
built-in host.
