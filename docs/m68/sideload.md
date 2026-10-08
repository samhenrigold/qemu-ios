# Unofficial apps on iPhone OS 1.0

iPhone OS 1.0 (1A543a) had no SDK, no App Store, no installd and no code signing. An app is a bundle
in `/Applications`, SpringBoard lists whatever it finds there, and the 2007 jailbreak community
installed apps by copying bundles in. This page covers the same thing on the emulated M68: building an
app against 1.0's own frameworks, putting it on the device, and debugging it.


## Building: `contrib/hello-2007`

```
ARMV6_SDK=<iPhoneOS3.1.3.sdk> contrib/hello-2007/build.sh ROOT [OUT]      # OUT/Hello.app, OUT/Tilt.app
```

ROOT is a host copy of the 1.0 root filesystem (the IPSW's decrypted rootfs, mounted or copied). Both apps
are Objective-C written the way 2007 apps were. The interfaces are declared by hand from the firmware's class
metadata in `uikit1.h` (class-dump headers, in effect), and a `UIApplication` subclass is passed to
`UIApplicationMain`.

- **Hello**: `UIWindow`, `UINavigationBar`, `UITextLabel` and `UIAlertSheet` from the private 1.0 UIKit,
  with GraphicsServices fonts and CoreGraphics colors. Tapping the bar button or the background shows an
  alert and counts.
- **Tilt**: a ball that rolls with the phone. The `UIApplication` subclass overrides
  `acceleratedInX:Y:Z:`, which is all 1.0's UIKit needs to start sending raw accelerometer events
  (`_requestAccelerometerEventsIfNeeded` looks for the override). A `UIView` subclass draws a target and
  the ball in `drawRect:` through `UICurrentContext()` and CGContext calls, with a low-pass filter on
  the samples. The ball is green when the phone is level. On the emulator, tilt it from the host with
  QMP: `qom-set /machine accel-pose flat`, then `accel-roll` and `accel-pitch` in degrees.
  Measured: flat reads x 0.00 y 0.00 with the ball centered and green; roll 12 and pitch -15 move it
  off-center and orange. Flat reads z -0.43: 1.0's own driver scale (1/128 g per
  count) on the LIS302DL's 18 mg counts, what a real phone on 1.0 reads (README, notes for guest software).

| Step | What | Why |
|---|---|---|
| Link stubs | `machotool tbd ROOT /path/in/root OUT.tbd` (`contrib/armv6-toolchain/machotool.c`) writes a `.tbd` (install name, versions, exported symbols) for each library the app uses | There was never an SDK. Linking against the firmware's own binaries means two-level binding names only symbols 1.0 has. Modern `ld` and `nm` refuse the 1.x binaries themselves (`LC_PREBIND_CKSUM`), so `machotool tbd` reads the symbol table directly |
| Compile | `cc6 ... -fobjc-runtime=macosx-fragile-10.5` (armv6, `-marm`, LEGACY_LINK `-ffixed-r9`) | 1.0's runtime is the fragile ABI: an `__OBJC` segment, classes referenced as `.objc_class_name_X`. clang still emits that ABI for ARM |
| Link | `link6 -execute` with the stubs, LEGACY_LINK=1 (non-PIE, `crt1old.c`, `machotool mkold --legacy`) | What 1.x dyld and libSystem take (contrib/armv6-toolchain/README.md) |
| Initializers | `main` looks up `__dyld_make_delayed_module_initializer_calls` through its own `__DATA,__dyld` section and calls it | 1.x dyld (the Tiger one) defers library initializers until crt1 asks for them. libobjc's initializer registers the image callback that fixes class and selector references, so without this call the first message crashes in `objc_msgSend` on a class-name string. `crt1old.c` is shared with plain-C helpers that run on 2.x and 3.x, where adding a `__dyld` section also changes how dyld orders initializers, so it does not make this call |
| Subclass ivars | A UIKit class the app subclasses is declared at its real instance size (UIApplication 12 bytes, UIView 28, read from the class metadata) | The fragile ABI fixes a subclass's ivar offsets at compile time, right after the declared superclass. Declared empty, HelloApp's ivars overwrote UIApplication's own and taps stopped reaching the app. class-dump headers had the ivars for this reason |

The bundle is `Hello` (non-PIE armv6, entry at crt1old's `start`), `Info.plist` (CFBundleExecutable,
CFBundleIdentifier, CFBundleName; the stock apps' keys), `PkgInfo` and a 57x57 `icon.png` (drawn by
`icon.c`). It needs no signature.

## Installing: `imgtools/sideload1x.py` (retired)

`imgtools/sideload1x.py` is retired, with no replacement yet; see git history at 5508b504b8. It was run as
`imgtools/sideload1x.py --nand DEV/nand --overlay OVL Hello.app [More.app ...] [--remove Old.app]`.

The script wrote into the overlay the emulator boots with (`nand-overlay=OVL`) and never into the
device's base. It works on a device fresh from FirmwareKit and on one that has been booted, as long as
that device was powered off from the guest. It reads the system volume through the legacy FTL's own
context, mounts it read-write on the host, copies the bundles into `/Applications`, removes the files
macOS leaves behind and checks the volume with `fsck_hfs -fn`. Each logical page that changed is written
over the physical page the FTL maps it to, keeping that page's spare area. The FTL's context is left as it
was, so on the next boot the guest reads the new data where it expects the old. About a minute for a 512
MiB volume.

The FTL context is openiBoot's s5l8900 `FTLCxt`, the same structure FirmwareKit's `N45NAND.ftlMeta` writes.
The script reads these parts of it:

- **Where the context is.** The context moves: the FTL takes new context blocks from its free pool. The
  current context block is the virtual block whose first page has a context type (0x43-0x4F) and the lowest
  usnDec. Its last written page must be the context itself (type 0x43); anything else means an unclean
  shutdown, and the script refuses.
- **Map.** The logical-to-virtual block map is four 0x46 pages, listed at +0x38.
- **Logs.** The log table is at +0x1A4, as vbn and lbn per entry. The per-log page offsets are pages
  listed at +0x110.
- **Pages.** A logical page goes to its log block's page when the log holds it, otherwise to
  `map[lbn]` at the same offset. A virtual page maps to bank and page by N45NAND's striping, with
  virtual block 0 at physical block 201. The overlay's `blk<N>.erased` markers are honored.

Measured on the M68: Hello was sideloaded, then the device was booted, Hello was used, and the device
was powered off with `system_powerdown`. The guest had moved its context to virtual block 3 and had eight
log blocks open. Hello2 was then sideloaded into that overlay, and the next boot showed both icons and
launched Hello2.

- **The journal is put back.** macOS rewrites the HFS journal header for 512-byte blocks when it mounts
  the volume. 1.x adopts the header's block size and then fails I/O on its 2048-byte pages: on the first
  try this was `mkdir` EINVAL in configd and "Could not setup local comms port" in lockdownd, followed by an
  unactivated phone. The script snapshots the journal info block and the journal before mounting and
  restores both afterwards, as FirmwareKit's `VolumeMount` does.
- **Owners.** The host mount ignores ownership, so the bundle's files are owned by uid 501. 1.0 runs apps
  as root and the modes are 755/644, so nothing on 1.0 cares.
- **fsck findings 1.x left behind are kept.** A folder that 1.0's kernel creates has no
  HasFolderCount flag, which modern `fsck_hfs` reports. The script runs fsck before and after the edit
  and refuses only new findings.

Boot as usual with the overlay (`KEEPOVL=1 RUN=... tools/boot.py` keeps it). The icon appears after the
stock apps on the home screen, labeled with the bundle's directory name (1.0 ignores CFBundleName).
A tap launches the app.

## Debugging it

The app is an ordinary process to the gdbstub tools (docs/guest-debug.md): `xnu-break Hello
"-[HelloApp sayHello]"` stops in its code, and `xnu-images --sysroot ROOT` brings in UIKit's symbols.
Add the host copy of the executable with `target modules add .../Hello.app/Hello` followed by `target
modules load --file Hello --slide 0` (non-PIE). The initializer crash above was found that way. A
breakpoint on `exception_triage` filtered to `Hello` stopped at the fault, and the thread's saved user
state (thread + 0x1b4 on 1.0: r0-r12, sp, lr, pc, cpsr, fsr, far) put the PC in `objc_msgSend + 20`, with
the receiver's "isa" reading `NSAu`.

## Debts

1. A device that was not powered off cleanly needs FTL_Restore's scan (the newest copy of every
   logical page) before it can be read. The script refuses such a device; boot it and power it off
   from the guest.
2. Owners stay uid 501. HFSPlusVolume.setOwner (FirmwareKit) or a catalog patch would make them 0:0, and
   that will matter if an app is meant to ship a LaunchDaemon.
3. When the ringer volume HUD comes up at boot (docs/m68/README.md, debt 2), it covers the app as well.
4. The iPod touch 1G (N45, 3A101a, eight banks) uses the same layout, and the tool installs there
   (the volume checks clean). SpringBoard 1.1 still shows nothing it does not know:
   `-[SBIconModel _addItemsToIconList:fromPath:withTags:]` keeps only display identifiers in
   `-[SBPlatformController allowedDisplayIdentifiers]`. That is a list compiled into SpringBoard per
   platform: M68/N82/simulator get the iPhone set, N45 gets its 12 apps plus com.apple.DemoApp. Borrowing
   an unused identifier does not help, because the N45 list has none (`com.apple.mobilenotes` was tried
   and stays hidden). Showing an unofficial app on 1.1 means changing SpringBoard, which this tool does
   not do.
