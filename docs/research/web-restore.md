> Status: research, superseded by `../ipad1/iboot.md` and `tests/ipad1/restore-smoke.py` (stock restore over emulated USB works).

I have gathered enough concrete evidence. Let me compile the report as my final answer without further low-level binary carving.

# Feasibility: Producing iPad 1 (iOS 3.2 / 7B367) NAND by running Apple's own restore inside the emulator

## 0. Bottom line up front

**Realistic path: YES, but only by re-implementing the host side yourself, NOT by running stock `idevicerestore`/iTunes.** The restore ramdisk (`restored_external` + `asr`) is the standard, well-documented mechanism, and driving it does not require Apple as long as the guest's bootrom/PKE accepts unsigned images (which your fork already forges via `forge-sigcheck`). The one hard external dependency in the *stock* host tools is TSS/SHSH, and for build_major ≤ 8 (3.2 = major 3) the stock tools do **not** contact Apple at all — they gate on a local `.shsh` file that you can trivially fabricate because the guest never verifies it. So the blocker everyone associates with A4 restores (per-ECID SHSH from Apple) **does not apply to your emulated device**, because the check lives in the bootrom/iBoot you already neutered, not in the host tool.

The catch is that `restored_external` talks to the host over **usbmux port `0x1234` (ASR) + the `com.apple.mobile.restored` lockdown-less service on port `0xf27e`/62078**, so your usbmux fork must bridge *restore-mode* USB (a different USB configuration than the mux interface it currently bridges), and DFU/recovery-mode USB (control transfers to PID 0x1227/0x1281) must be modeled by the QEMU S5L8930 USB-OTG device. That is the real engineering cost, not TSS.

---

## 1. What the iOS 3.2 iPad 1 restore does, step by step

### 1.1 Confirmed artifacts (from your extracted IPSW)
Source: `/tmp/scratchpad/fw/7B367/BuildManifest.plist`, `Restore.plist`, and img3 tag dumps I ran.

- **Two BuildIdentities**: Erase (`RestoreRamDisk = 018-7226-009.dmg`) and Update (`018-7225-009.dmg`). `ApBoardID 0x02`, `ApChipID 0x8930`, `ApSecurityDomain 0x01`, `CPID 35120 (0x8930)`, `BDID 2`, `Platform s5l8930x`, `BoardConfig k48ap` (`Restore.plist`).
- **NOR chain to flash** (all `IsFirmwarePayload`): `LLB.k48ap.RELEASE.img3` (illb), `iBoot.k48ap.RELEASE.img3` (ibot), `DeviceTree.k48ap.img3` (dtre), `applelogo/batterycharging0/1/full/low0/1/glyphcharging/glyphplugin/needservice/recoverymode-768x1024` glyphs, plus a `manifest` file listing them. Path prefix `Firmware/all_flash/all_flash.k48ap.production/` (`BuildManifest.plist`).
- **DFU stage**: `Firmware/dfu/iBSS.k48ap.RELEASE.dfu` (ibss, 108932 B), `iBEC.k48ap.RELEASE.dfu` (108932 B). Both are img3 with `KBAG` (encrypted) — decryption keys public.
- **RestoreKernelCache = `kernelcache.release.k48`** (krnl, img3, 4.9 MB), **OS filesystem = `018-7223-007.dmg`** (asr payload, UserOS).
- **All img3 keys are public** for 7B367 via `https://api.ipsw.me/v4/keys/ipsw/iPad1,1/7B367` (I fetched them; e.g. RestoreRamdisk key `31e7ecd9…a106` iv `9c05…7fe1`; iBSS key `eb3c9e…1bea`; kernelcache key `8c80ca…c146`). Each img3 carries `KBAG` + `SHSH` + `CERT` tags (verified by tag dump).

### 1.2 The wire sequence (from `idevicerestore` sources I pulled + iPhone Wiki)
Legacy path (`build_major <= 8`), reconstructed from `idr_idevicerestore.c`, `idr_recovery.c`, `idr_dfu.c`, `idr_restore.c`, `libirecovery.c`:

1. **DFU mode** (USB PID `0x1227`, `IRECV_K_DFU_MODE`). Host uploads iBSS via DFU control transfers: `irecv_usb_control_transfer(0x21, 1, …)` in 0x800-byte packets with a DFU CRC/suffix `ac 05 00 01 55 46 44 10`, terminated with `0x21,1,0,0` then status polls `0xa1,3` / `0xa1,5`, then `0x21,4` (reset). (`libirecovery.c:3788-3941`.) For 3.x, `dfu_send_component(iBSS)` then device re-enumerates.
2. **iBEC**: For `build_major <= 8` the flow sends iBSS in DFU, device reconnects in **recovery mode** (PID `0x1281`, `IRECV_K_RECOVERY_MODE_2`), then iBEC is sent and run. Recovery-mode uploads use **bulk EP 0x04** (`0x8000` packets) preceded by control `0x41,0` (`libirecovery.c` `irecv_send_buffer`, recovery branch).
3. **iBoot recovery USB command protocol** (control `0x40, b_request` for text commands, `libirecovery.c:3476-3545`): `setenv auto-boot false`, `saveenv`, `setpicture`/`bgcolor`, `getenv ramdisk-size`, `ramdisk`, `devicetree`, `bootx`. Ramdisk/DeviceTree/KernelCache are each uploaded then activated (`recovery_send_component_and_command`, `recovery.c:199-528`). Restore boot-args set by iBEC path: **`rd=md0 nand-enable-reformat=1 -progress -restore`** (`idr_recovery.c:128`) — `nand-enable-reformat=1` is the flag that authorizes the FTL/VFL reformat.
4. **Restore mode**: kernel boots the ramdisk, brings up its own USB device stack (PTP-class interface, class `0xff` sub `0x50` proto `0x43` — see `restored_external.c:96-101` in nabla-c0d3), and `com.apple.mobile.restored` listens on **port `0xf27e` (62078)** (`libimobiledevice/src/restore.c:303`).
5. **`restored` handshake**: host `QueryType` → device replies type + `RestoreProtocolVersion` (`limd restore.c:168-219`); host sends `StartRestore` with `RestoreOptions` and `RestoreProtocolVersion` (`limd restore.c:370-387`).
6. **`restored_external` drives**: writes GPT, creates `disk0s1` (System) + `disk0s2` (Data), wipes effaceable keys, invokes `/usr/sbin/asr` for the rootfs, flashes NOR, creates system keybag, updates NVRAM boot-args. (Confirmed ramdisk contents below + iPhone Wiki "IPhone Restore Procedure" / axleos part 4.)
7. **Message loop** — device emits `DataRequestMsg` with a `DataType`; host answers (`idr_restore.c:4864-5101`, `restore_handle_data_request_msg`):
   - `SystemImageData` → `asr` payload over **usbmux port 12345** (`asr.c`; ASR plist protocol: `Command`/`OOBData`/`Payload`, `ASR_PAYLOAD_PACKET_SIZE 1450`, `FEC` params, `-source asr://localhost:12345 -target /dev/disk0s1 -erase -noprompt --chunkchecksum --puppetstrings`).
   - `KernelCache` → `restore_send_component`.
   - `NORData` → `restore_send_nor`: sends `LlbImageData` + an array of the all_flash firmware images, read from the `manifest` file (`idr_restore.c:1532-1700`).
   - `RootTicket`/`NORData`/`BasebandData` etc.; **Wi-Fi iPad has no baseband** (BuildManifest has `BasebandFirmware` entries but the ICE2 06.15.00 fls in the ramdisk is for the 3G iPad; for iPad1,1 Wi-Fi `restore_send_nor`/BBUpdate is a no-op — `UpdateBaseband` can stay but there is no modem to answer).
8. **`ReceivedFinalStatusMsg`** → reboot to NAND.

### 1.3 3.2 restore ramdisk contents (I decrypted `018-7226-009.dmg` and walked the HFS+ catalog)
Decrypted with the public key/iv; it is a **raw HFS+ volume** (`H+`, blockSize 4096, 2559 blocks ≈ 10.5 MB), not UDIF. Catalog listing (my `hfslist.py`) shows the driver programs:
- `/usr/local/bin/restored_external`
- `/usr/sbin/asr`, `/usr/sbin/fdisk`, `/usr/sbin/nvram`
- `/sbin/newfs_hfs`, `/sbin/fsck_hfs`, `/sbin/mount_hfs`, `/sbin/mount`, `/sbin/launchd`, `/bin/launchctl`
- `/usr/local/bin/ioflashstoragetool` (the NAND/IOFlashStorage low-level tool), `/usr/local/bin/bbupdater`, `BBUpdaterExtreme`, `PhoneToolExtreme` (baseband — irrelevant for Wi-Fi iPad)
- `/usr/local/share/restore/options.plist` (245 B), `PASS.png`, `imeisv_svn.plist`
- `/private/etc/rc.boot` (Mach-O), `System/Library/AppleUSBDevice/USBDeviceConfiguration.plist` (defines the restore-mode USB config), IOKit/IOUSBDeviceFamily/IOHIDFamily plugins.

(Note: the on-disk file *data* is stored in the HFS+ volume; extraction of individual fork bytes needs the volume block map, which my quick catalog walker returns as zero-length for these entries — the file inventory is authoritative, the byte extraction script needs the fork/extent decode I did not finish. This is a scratch-tool limitation, not a data problem.)

### 1.4 NAND/NOR kexts the guest must satisfy (from decrypted `kernelcache.release.k48`, LZSS-decompressed, string scan)
Confirms exactly which drivers the restore kernel loads and thus what your S5L8930 model must emulate well enough to `open`:
- **NAND/FTL**: `com.apple.iokit.IOFlashStorage`, `AppleNANDFTL`, `IOFlashPartitionScheme`, `IOFlashController`, `IOFlashMedia`, `IOFlashStorageDevice/Partition`, `IOFlashNVRAM`, `com.apple.driver.AppleNANDFirmware`, **`AppleS5L8920XIOPFMI`** (the H2FMI/IOP flash controller — note it's the 8920X variant name even on 8930), `EncryptedBlockStorage`.
- **NOR / image3**: `AppleImage3NORAccess` (+ `-nand`, `UserClient`), `IOFlashNVRAM`.
- **Crypto/effaceable**: `com.apple.iokit.IOCryptoAcceleratorFamily`, `AppleS5L8900XCrypto`, effaceable storage (via `AppleEffaceableStorage`, referenced in nabla-c0d3 ramdisk_tools). EMF/effaceable keys are **created on-device** by `restored_external`/the NAND stack during format (the `CREATE_SYSTEM_KEY_BAG` op = 49 in `idr_restore.c:59`), not supplied by the host.
- Restore progress ops enumerated in `idr_restore.c:41-59`: `WAIT_FOR_STORAGE=11`, `CREATE_PARTITION_MAP=12`, `CREATE_FILESYSTEM=13`, `RESTORE_IMAGE=14`, `VERIFY_RESTORE=15`, `CHECK/MOUNT_FILESYSTEM=16/17`, `FLASH_NOR=19`, `UPDATE_BASEBAND=20`, `FINALIZE_NAND=21`, `MODIFY_BOOTARGS=26`, `LOAD_KERNEL_CACHE=27`, `PARTITION_NAND_DEVICE=28`, `WAIT_FOR_NAND=29`, `CREATE_SYSTEM_KEY_BAG=49`.

---

## 2. Signing: does 3.2 iPad 1 restore require Apple's TSS?

**No — not for your emulated device, and not for the stock host tools either at build_major 3.**

Two independent reasons:

**(a) The stock host tool skips Apple entirely for major ≤ 8.** In `idevicerestore.c:get_tss_response` (line 2349) and the Chronic-Dev era `get_shsh_blobs` (line 1325): `if ((client->build_major <= 8) || (client->flags & FLAG_CUSTOM)) { …check for local shsh file… }`. It looks for `shsh/<ECID>-<product>-<version>.shsh` (gzipped bplist/xml). If present it uses it and **never calls `tss_request_send`**. So a "local TSS emulation" is built in: drop a `.shsh` at that path. The stitching code `img3_stitch_component`/`img3_replace_signature` (`idr_img3.c:252-451`) takes ECID+SHSH+CERT elements from that blob and splices them into each img3.

**(b) The guest never checks the blob.** Your PKE `forge-sigcheck=on` already makes iBoot's final A×1 RSA conversion accept a forged signature (`docs/ipod-pke.md`), and iBoot's own strings confirm the escape hatch exists natively: `image validation failed but untrusted images are permitted` / `development-cert` / `production-cert` / `goldcertid` (from decrypted `iBoot.k48ap.bin` strings, `dec/iboot.str:799-841`). So whatever `.shsh` you fabricate (even self-signed / all-zero SHSH) will be accepted because the checker is neutered. You do not need real Apple blobs, saved blobs, TinyUmbrella, or a TSS server.

**Availability of real 3.2 blobs (for completeness, not needed):** iPad1,1 3.2 has **not been signed by Apple since ~2010**; current signed version is 5.1.1 (idevicecentral signing status). Cydia's TSS cache (`cydia.saurik.com/TSS/controller?action=2`, historically redirected from `gs.apple.com` via hosts file to IPs like 74.208.10.249) primarily saved 3G[S] 3.0/3.1 blobs; the 2009 saurik writeup shows **no iPad coverage**, and the iOS 6.0-6.1.2 cache was later corrupted. TinyUmbrella could only save blobs while Apple was still signing, which for iPad1,1 3.2 is long past. **Do not pursue saved-blob or fake-TSS-server paths** — they are both unnecessary (given (a)+(b)) and mostly unavailable.

**idevicerestore flags that matter here:**
- `-c/--custom` → `tss_enabled = 0`, prevents any signing attempt, uses `Restore.plist` instead of BuildManifest (`idevicerestore.c:696-698`). But custom mode expects a pre-personalized/decrypted custom IPSW.
- `-s/--server URL` → override TSS URL (you could point at a localhost stub, but (a) means you won't need it for 3.2).
- `-e/--erase`, `-R/--restore-mode`, `-T/--ticket PATH` (feed a ready AP ticket; used by qemu-t8030's forged-ticket flow), `-x/--exclude` (skip NOR/baseband — "legacy devices"), `-z/--no-restore` (stop after ramdisk boot — useful for bring-up), `-k/--keep-pers` (dump personalized components for debugging), `-P/--plain-progress`, `-i/--ecid`.
- **There is no `--no-restore-fr` and no `--use-pwndfu`/`-p` beyond `--pwn` (limera1n DFU) in this codebase.** `--pwn` (`-p`) only puts a limera1n device in pwned DFU; `limera1n_is_supported` is A4-capable but you don't need it since your PKE already forges checks. The query's `-p --use-pwndfu` refers to **ipwndfu**, a separate tool, not idevicerestore.

**TSS request format (if you ever stub it):** `tss.c` builds a plist with `@APTicket`, `@BBTicket`, `@HostPlatformInfo`, `@VersionInfo` (`libauthinstall-…`), `@UUID`, `ApECID`, `ApChipID`, `ApBoardID`, `ApSecurityDomain`, `ApProductionMode`, `UniqueBuildID`, and every Manifest entry; POSTs XML to `https://gs.apple.com/TSS/controller?action=2` with `User-Agent: InetURL/1.0`, success detected by `MESSAGE=SUCCESS`, blobs returned per-entry as `{Blob, Path}` (`chronic_tss.c:300-584`, `libtatsu tss.c`). A localhost stub returning `MESSAGE=SUCCESS` with dummy Blobs is straightforward but, again, unnecessary at major 3.

---

## 3. Can the ramdisk programs be driven by a custom host script instead of idevicerestore?

**Yes, and this is the recommended approach for a controlled emulator bring-up.** The restore-mode protocol is fully documented in open source and is just plist-over-usbmux:

- **`restored` service** (`com.apple.mobile.restored`, port 62078): `restored_query_type`, `restored_start_restore(options, protocol_version)`, `restored_send/receive` plists, `restored_reboot`, `restored_goodbye` (`libimobiledevice/src/restore.c`). Messages are `{MsgType: DataRequestMsg|ProgressMsg|StatusMsg|BBUpdateStatusMsg|ReceivedFinalStatusMsg, …}`.
- **DataType handlers** you must answer: `SystemImageData`, `KernelCache`, `NORData` (→ `LlbImageData` + firmware array), `RootTicket`, `BuildIdentityDict`, `FDRTrustData`, `BasebandData` (`idr_restore.c:4864-5101`).
- **StartRestore options** for the legacy (non-macOS) branch are enumerated verbatim in `idr_restore.c:5570-5600` and Chronic-Dev `restore.c:1577-1631`: `BootImageType=User/UserOrInternal`, `CreateFilesystemPartitions=1`, `DFUFileType=RELEASE`, `DataImage=0`, `FirmwareDirectory=.`, `FlashNOR=1`, `KernelCacheType=Release`, `NORImageType=production`, `RestoreBundlePath=/tmp/Per2.tmp`, `SystemImage=1`, `SystemImageType=User`, `SystemPartitionPadding={8:80,16:160,32:320,64:640}`, `UpdateBaseband=0`, `AutoBootDelay=0`, `RestoreBootArgs=<the rd=md0 … string>`.
- **ASR** protocol (`asr.c`): connect usbmux port 12345, exchange `Command`/`Payload`/`OOBData` plists, stream the decrypted `018-7223-007.dmg` filesystem in 128 KiB chunks with per-chunk checksums.

A ~500-line Python driver over your usbmux fork can implement `restored` + `asr` and feed the four DataTypes from the extracted IPSW. `nabla-c0d3/iphone-dataprotection/ramdisk_tools/restored_external.c` is a **complete open re-implementation of the device side** (usbmux init via `IOUSBDeviceControllerLib`, `plist_server.c`, `remote_functions.c`) — invaluable as protocol reference and it targets exactly this era (its README lists "iPad 1" as supported, armv7 ramdisks).

---

## 4. What could NOT be confirmed / open risks

- **Exact `options.plist` (245 B) and `rc.boot` contents** of the 3.2 ramdisk: file inventory confirmed, but my scratch HFS walker returned zero-length forks (needs proper extent decode); byte-level extraction not completed. Get these by finishing fork extraction or mounting the decrypted volume.
- **Whether iPad1,1 Wi-Fi restore issues a `BasebandData`/`UpdateBaseband` request at all**: the ramdisk contains ICE2 06.15.00 baseband fw + bbupdater (that's the 3G iPad's modem), and api.ipsw.me reports baseband `06.15.00` for 7B367 — so a Wi-Fi-only guest must be prepared to either satisfy or safely no-op a baseband step. `-x/--exclude` exists to skip it. Not verified against a real 7B367 restore log.
- **`RestoreProtocolVersion` value for 3.2**: `restored_query_type` returns it dynamically; the exact integer for 7B367 (iOS 5-era code uses ≥ 9; 3.2 is lower) was not pinned from a log. Read it live from the guest.
- **S5L8930 USB-OTG + DFU/recovery register model in your fork**: your `hw/arm/` tree has `ipod_touch_usb_otg.c`/`tcp_usb.c` for the **8720**; there is **no S5L8930 machine** and no evidence DFU/recovery-mode USB (control-transfer command protocol, PIDs 0x1227/0x1281) is modeled. `docs/tcp-usb-protocol.md` bridges only the **AppleUSBMux** interface (class/subclass/proto matched in `usbmuxd/src/usb-qemu.c:find_mux_interface`); restore mode presents a **different USB configuration** (`USBDeviceConfiguration.plist` in the ramdisk, PTP-ish class 0xff/0x50/0x43), which the bridge does not currently select. This is the principal new work.
- **iBoot-817.28 / S5L8930 bootrom image**: not present in the IPSW (bootrom is on-chip). Your PKE/forge model was validated on S5L8720/8900; the 8930 PKE geometry and SHA1-DigestInfo forging must be re-validated. Not assessed here.
- **prior-art QEMU forks** `teknogeek/iemu`, `danzatt/QEMU-s5l89xx-port` (hw/s5l8930*.c, s5l8930_h2fmi.c, s5l8930_iop.c) exist locally as S5L8930/H2FMI references but I did not audit whether they model restore-mode USB; they predate a working restore and almost certainly do not.

---

## 5. Recommended path (concrete)

1. **Do NOT use TSS/SHSH from Apple, saved blobs, or a fake TSS server.** At build_major 3, fabricate a local `shsh/<ECID>-iPad1,1-3.2.shsh` (or run with `-c`) and rely on your PKE `forge-sigcheck` so the guest accepts the stitched/forged images. (Refs: `idr_idevicerestore.c:2355`, `iboot.str:799`.)
2. **Prefer a custom Python `restored`+`asr` driver** over stock idevicerestore for deterministic bring-up (use `restored_external.c` and `idr_restore.c` as the protocol spec). Fall back to real `idevicerestore -e -c -R` once USB is solid.
3. **The gating engineering task is USB, not signing**: model S5L8930 USB-OTG well enough for (a) DFU control-transfer upload of iBSS/iBEC, (b) recovery-mode bulk upload + text command protocol, (c) a second USB configuration in restore mode so the mux/`restored`/ASR endpoints enumerate; extend the `tcp_usb` bridge + usbmux fork to select the restore config and expose ports 62078 and 12345.
4. **NAND**: because `restored_external` formats via the real FTL/VFL/YAFTL over `AppleS5L8920XIOPFMI`/H2FMI, your S5L8930 H2FMI + NAND model must support **program/erase**, not just read — this is what lets you *avoid* synthesizing FTL structures (the whole point). openiBoot's `ftl-yaftl/yaftl.c` (`ftl_yaftl_write_single_page:2396`, `YAFTL_Flush:620`) and `vfl-vsvfl/vsvfl.c` are the on-device format algorithm reference; `nabla-c0d3/python_scripts/nand/{yaftl,vsvfl,vfl}.py` are the host-side readers to validate the produced image.

**Net assessment:** the restore-by-emulation approach is sound and is the correct way to get authentic NAND content; TSS is a non-issue for 3.2; the cost is a faithful S5L8930 USB-OTG/DFU/recovery model plus a writable H2FMI/NAND path, after which either a small custom driver or `idevicerestore -e -c` over your usbmux fork will run Apple's own `restored_external`/`asr` to completion.