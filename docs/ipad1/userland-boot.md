# iPad 1 / 7B500: userland boot recipe (root mount → launchd → SpringBoard)

What the emulator needs the moment the 7B500 kernel says `BSD root: disk0s1`, and where it comes from.
Built by `imgtools/ipad1_rootfs.py`; consumed by `imgtools/ipad1_nand.py`. Everything lives under
`~/Developer/qemu-ios-files/ipad1/` (not the repo): inputs in `7B500/dec` and `hw2/`, outputs in `userland/`.

## Ready to boot (2026-09-26)

```
userland/system.img        1280 MiB HFSX, pristine 7B500 rootfs + 2 edits (below)
userland/data.img          1 GiB journaled HFSX "Data", /private/var skeleton + real activation record
userland/nand-userland/    ipad1_nand.py store of MBR + system + data; `ipad1_nand.py check` all green
hw2/lockdown/              /var/root/Library/Lockdown pulled off the real iPad (fetch-lockdown)
```

Boot it with the existing bundle and this store:

```
tests/ipad1/boot-smoke.py --nand ~/Developer/qemu-ios-files/ipad1/userland/nand-userland --seconds 180
```

`boot-smoke.py` already has the `bsd` (`BSD root:`) and `launchd` markers. The kernel bundle needs nothing new:
`ipad1_kboot.py` names partition 1 in `root-matching`, forces `debug-enabled`, and the default boot-args
`-v serial=3 debug=0x8` are enough because **no Mach-O on either volume is modified**: no
`amfi_allow_any_signature` / `cs_enforcement_disable`, no `ldid -S`, no shared-cache patch.

To regenerate from scratch (about 5 s for the volumes, about 1 min for the store):

```
imgtools/ipad1_rootfs.py fetch-lockdown          # once; needs the iPad on USB with iproxy 2323 -> 22
imgtools/ipad1_rootfs.py build                   # -> userland/system.img, userland/data.img
imgtools/ipad1_nand.py build --mbr hw2/rdisk0-head4M.bin --system userland/system.img \
                             --data userland/data.img --out userland/nand-userland
imgtools/ipad1_nand.py check userland/nand-userland --mbr hw2/rdisk0-head4M.bin --system userland/system.img
```

## What is on the volumes and why

**System volume = the IPSW rootfs, not the hw2 capture.** `hw2/rdisk0s1-system.img` is the jailbroken unit's
volume: `/Applications -> /private/var/stash/Applications` (so with a fresh data partition there would be no
Preferences.app, Safari, ...), Cydia-era daemons, fstab pointing at `disk0s2s1`. The decrypted
`7B500/dec/rootfs.dmg` (`Wildcat7B500.K48OS`, Apple_HFSX slice of a UDIF+APM image, 8 KiB allocation
blocks, 1000 MB) is what a restore lays down, and its catalog already carries Apple's uid/gid/modes. The tool
slices it out of the APM, grows it to partition 1's 327680 × 4 KiB (the MBR is the authority), moves the
alternate volume header to the new end, and mounts it once to make two edits:

| file | edit | why |
|---|---|---|
| `/private/etc/fstab` | `/dev/disk0s1 / hfs rw 0 1` + `/dev/disk0s2 /private/var hfs rw,nosuid,nodev 0 2` | stock is identical except `ro` root. `rw` is insurance: if the data mount fails, daemons still find a writable `/var` skeleton on the root volume instead of dying one by one. `--ro-root` keeps stock. `disk0s2` (not `disk0s2s1`) because `ipad1_nand.py` publishes the data partition as plain Apple_HFS 0xAF: no EncryptedMediaFilter, no `tprc` key block, no CDMA AES needed for the mount. |
| `/System/Library/LaunchDaemons/com.apple.SpringBoard.plist` | `EnvironmentVariables` += `CA_ENABLE_OGL=0`, `MBX2D_PAGE_FLIP=0`; `StandardOutPath` = `StandardErrorPath` = `/dev/console` | `userland-gl-display.md` §1.3/§1.5: OpenGLES never caches a failed `_eagl_init`, so without `CA_ENABLE_OGL=0` SpringBoard re-runs dlopen(GLEngine) + IOAcceleratorES + AppleMBXDevice matching on every render; `MBX2D_PAGE_FLIP=0` leaves one IOMFB page so the scaler-backed page copy (no CPU fallback) is never attempted. `/dev/console` is `crw--w--w-` on the real unit, so the `mobile` user can append and SpringBoard's own stderr lands on the serial log next to the kernel's. |

Plists are rewritten in place (same catalog record, still uid 0) in their original binary format. Optional
`--disable LABEL` sets `Disabled=true` on any other job; see "knobs" below.

**Data volume = what `mobile_obliterator` would create.** A fresh journaled HFSX "Data" volume
(`ipad1_nand.make_hfs_image`) seeded with a copy of the system volume's own `/private/var` skeleton (the
40-odd directories the restore/obliterate path copies: `db/launchd.db`, `db/timezone/localtime`, `log/asl`,
`mobile/Library/{Preferences,Caches,Keyboard,...}`, `mobile/Media/{DCIM,Photos}`, `run`, `tmp`, `msgs`,
`root/Library/Preferences`), then `hw2/lockdown/` copied to `/var/root/Library/Lockdown`. Ownership matches the
real iPad's `ls -ln /private/var`: `mobile/` and `ea/` are 501:501, everything else 0:0, patched into the
catalog offline (`build_nand.set_owner`) because the host mount is `noowners`. 1 GiB is plenty; SpringBoard
writes a few hundred KiB on first boot. `ipad1_nand.py` sizes partition 2 to whatever `data.img` is.

## What to expect on serial, and what each stage depends on

1. `BSD root: disk0s1, major 14, minor 1` — the kernel found the 0xAF partition 1 via `root-matching`
   (already the case with the self-formatted store). Root is mounted read-only by the kernel; launchd remounts
   per fstab.
2. launchd (`/sbin/launchd`, launchd-258 era) starts, runs `fsck`/`mount` for fstab, then loads
   `/System/Library/LaunchDaemons/*.plist` (62 jobs on 7B500). With `-v` its own console output is on serial.
   **First userland dependency on the model:** `/dev/disk0s2` must exist and mount, i.e. the IOP/FTL read
   path must serve LBAs 327789+ (the store has them; `check` verified the HFS header at partition 2).
3. Early daemons: `configd`, `notifyd`, `syslogd`, `securityd`, `lockdownd`, `mediaserverd`, `wifiFirmwareLoader`,
   `BTServer`, `CommCenter`, `locationd`, `installd`, `fairplayd.K48`, `securekeyvaultd.s5l8930x`... Ones that
   talk to missing hardware crash or spin; `ThrottleInterval`/`KeepAlive` respawn them every 5–10 s, which is
   noisy on serial but not fatal. There is no `getty` binary on 7B500 (`/etc/ttys` lists one, `/usr/libexec/getty`
   does not exist), so a serial shell has to come from a custom daemon later, as on the iPod.
4. `SpringBoard` (`UserName mobile`, KeepAlive). Its stderr is on serial via `/dev/console`. It needs
   `com.apple.CARenderServer` to come up (its own Mach service), `AppleCLCD` matching for the CA display
   (`H3CLCDDisplay::open`, so the display pipe + CLCD model from M4 must at least publish the service and
   1024×768; until then SpringBoard logs the missing display and retries), and `IOHIDSystem`/`AppleM68Buttons`
   for events (input is M5; SpringBoard runs without it).
5. Activation: `lockdownd` reads `/var/root/Library/Lockdown/activation_records/pod_record.plist` and
   `data_ark.plist` (`ActivationState = Activated` on the real unit). See open items: the record is bound to the
   real unit's identity, so expect `Unactivated` and the "Connect to iTunes" screen until the emulator reports
   the same serial/ECID/MACs. SpringBoard still runs either way.

## Knobs

- `--disable LABEL` (repeatable): sets `Disabled=true` on a launchd job. Start stock and read the serial log
  first; candidates once they are seen to spin are `com.apple.wifiFirmwareLoader`, `com.apple.BTServer`,
  `com.apple.CommCenter` (iPad Wi-Fi has no baseband but the job exists), `com.apple.locationd`,
  `com.apple.mediaserverd` (audio hardware), `com.apple.mobile.storage_mounter`.
- `--ro-root`: stock read-only root.
- `--data-size` (default `1g`), `--lockdown none` to build an unactivated data volume.
- Further SpringBoard environment or launchd keys: edit `SB_ENV` / `springboard_env()` in `ipad1_rootfs.py` and
  rebuild (5 s). Do not use `patch_launchd_env.py` here: it edits the iPod's page store in place and assumes a
  single-block file; here the plist is rewritten through a real mount so it can grow.
- Later Mach-O edits (a GLI shim at `GLEngine.bundle/GLEngine`, a guest agent in `/usr/local/bin`) go through
  the same mount step: add them in `build()`, `ldid -S` them, list them for `set_owner(..., 0, 0)`, and add
  `amfi_allow_any_signature=1 cs_enforcement_disable=1` to the kboot boot-args (`debug-enabled` is already
  forced by `ipad1_kboot.py`, which is the 3.2-specific prerequisite for AMFI to honour them).

## Open items

- **Activation identity.** `pod_record.plist` carries an AccountToken bound to the device: `lockdownd` compares
  it against the UDID, which on 3.x is SHA1(SerialNumber + ECID + WiFiAddress + BluetoothAddress). Known for
  Sam's unit: serial `EMU000000000` (IOPlatformSerialNumber), Wi-Fi MAC `02:00:00:00:00:01`
  (`local-mac-address`, also the tail of `platform-uuid` in nvram), platform UUID
  `00000000-0000-1000-8000-020000000001`. Not yet captured: ECID (`unique-chip-id`; the IOKit dump walked the
  IOService plane only) and the Bluetooth MAC. `ipad1_kboot.fill_dt` currently writes serial `QEMUIPAD1` and a
  made-up `unique-chip-id`; switch those to the real values (and give the Wi-Fi model the real MAC) when
  activation matters. The pair records in the same directory let a host `usbmuxd`/lockdown client talk without
  re-pairing once USB exists (M5).
- **SpringBoard stderr**: `/dev/console` redirect is untested on the emulator; if launchd refuses the path,
  the fallback is `/var/log/springboard.log` on the data volume, read back with `ipad1_nand`'s LBA reader.
- **`.GlobalPreferences.plist`** on the data volume is the empty stock one; language/region prompts (Setup) do
  not exist on 3.2, so no first-run wizard blocks the home screen.
- Nothing here has been booted yet. First serial log against a real-iPad boot log is the M2 exit criterion in
  `PLAN.md`.
