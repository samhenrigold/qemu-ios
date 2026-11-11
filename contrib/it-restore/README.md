# Native N42 restoration tools

Build with the shared legacy toolchain and a 32-bit iOS SDK:

```sh
ARMV6_SDK=/path/to/iPhoneOS6.0.sdk ./build.sh /path/to/guest-tools
```

`it_restore` applies or verifies an explicitly supplied, read-only A6DI stream
through the original guest disk driver. `it_initialize` then formats Data with
the original `newfs_hfs -P`, creates the system keybag through MobileKeyBag,
copies the stock System `/private/var` seed and FirmwareKit's prepared changes,
and verifies the latter after unmount/remount. It halts only after native flush.
Both operate on the VM's fixed disk nodes; neither writes host devices.

FirmwareKit prepares the temporary original restore ramdisk and the seed under
`/usr/local/share/qemu/data-seed`. That seed must not contain an existing device
keybag or filesystem journal. Stock large assets remain on System and are copied
natively, rather than duplicated in the ramdisk. No helper is installed as a
user application or used to modify the normal System filesystem.

The initial preparation path is intended for **new, disposable media only**.
It formats Data. Existing devices use `it_restore` without initialization.
The default entitlement template is source-authored and grants disk-device
access; it is qualified for native MKB initialization on fresh N42 media.
`RESTORE_ENTITLEMENTS` remains a build-time diagnostic override. The emulator supplies only the
bounded restore transport; OS preparation policy stays in FirmwareKit.

The tools call the matching native restore and System libraries during
preparation. The optional MobileGestalt worker generates each device's cache
from those libraries. FirmwareKit stages and validates the resulting records.

For 12A365, 13A344 and 14G61, the restore ramdisk's MobileGestalt variant cannot answer all
normal-system capability questions. `build.sh` also builds `it_mobilegestalt`
from our source; keep it beside `it_initialize`. FirmwareKit validates the
matching native build/library and stages the worker in the temporary ramdisk.
After native keybag creation and Data seeding, the initializer temporarily
mounts Data at the read-only System's `/private/var`, then executes the worker
with that System as its filesystem root. Its original dyld, shared cache and
MobileGestalt frameworks generate the authentic cache, including capabilities.
No System library is replaced and no unavailable-answer shim is installed.

The worker applies FirmwareKit's planned booleans without changing the native
opaque cache bytes. Its executable and plan are removed from Data before the
initializer restores the mount layout and verifies both cache and seeds after
unmount/remount. This stays in the same preparation boot; no previous initialized
disk, cache, Apple executable bundle or normal-boot cache repair is an input.

The worker alone uses `mobilegestalt-entitlements.plist`, which adds the normal
developer `get-task-allow` entitlement. iOS 9 otherwise ignores its
`DYLD_SHARED_REGION=private` setting and fails to map the matching System cache
inside the preparation root. FirmwareKit checks this entitlement in the actual
worker before staging an iOS 9 or 10 restore. The original loader, cache and loading
sequence remain unchanged; no cache aliases or delayed-root workaround is
needed. The worker is removed before normal boot, and the other restore tools
retain their existing disk-access entitlement template.

14G61 uses the native `systemgroup.com.apple.mobilegestaltcache` container under
Data's `containers/Shared/SystemGroup`, with root ownership, rather than the older
mobile cache path. Generation and remount verification select that path for this
exact build. FirmwareKit also includes the staged initializer and cache worker in
the temporary restore kernel's existing support-file signing inventory, after
validating their signed pages. This does not change the normal System volume.

The temporary 14G61 worker creates only that known Data container and uses the
original container library's cached-directory lookup. That native path requires
the `containermanagerd.fb_check` readiness semaphore while the regular XPC
container service is unavailable. The worker creates it only when absent,
removes its own semaphore immediately after the native cache writer returns,
and leaves an existing service's semaphore alone. Directory ownership/type and
the original API's returned path must match before generation proceeds. An exit
handler also cleans up on failure. FirmwareKit requires the source-built
worker's container-bootstrap marker before accepting a 14G61 restore.

On 14G61, native first-boot `MobileGestaltMigrator` calls `_MGWriteCache` and
replaces the initialization cache. FirmwareKit's qualified first-boot recipe
preserves the already generated cache and shared setup record across the two
relevant migrators. It seeds fresh SystemGroup metadata so container validation
retains those directories. This replaces the earlier post-migration sealing
route; the first ordinary boot performs native migration and reaches Home.

The new SystemGroup cache is root-owned **0644**: its native reader,
`MobileGestaltHelper`, runs as mobile. Leaving the temporary file at 0600 makes
the helper reject and regenerate the cache, discarding the planned answer.
Legacy mobile-owned caches remain 0600. FirmwareKit requires the worker's
`it_mobilegestalt_readable_cache_v1` marker for 14G61, rejecting old tools before
initialization. Synthetic tests exercise the atomic writer under a restrictive
umask and retain the existing replay metadata checks.

`it_reapply_mobilegestalt(log, planPath)` shares initialization's plan validation
and application code, but operates on the cache generated by native migration
instead of generating another cache. It preserves that file's owner, group and
mode as well as its UUID, opaque data and unrelated answers. Synthetic host
tests cover those invariants. This primitive remains available for preparation
replay; the normal 14G61 recipe no longer needs a userland sealing boot or a
runtime completion hook.
