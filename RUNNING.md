## Running the iPod Touch 2G Emulator

This file contains the instructions on how to run the iPod Touch 2G emulator using QEMU.
Note that this is an experimental release and the functionality of the device is still limited.
Linux compatibility is currently unstable. Efforts are underway to improve it.

### Getting the Source Code

Clone the repository using the following commands:

```
git clone https://github.com/devos50/qemu-ios.git
cd qemu-ios
```

Now you can proceed with the rest of the instructions for building and running the emulator.

### Building QEMU

Make sure you have the required libraries installed to compile QEMU:

```
# On MacOS
brew install glib ninja meson pixman pkg-config sdl2 openssl@3

# On Linux (Ubuntu)
sudo apt install make ninja-build pkg-config libssl-dev libsdl2-dev libpixman-1-dev libpixman-1-0 libglib2.0-dev

# On Linux (Arch)
sudo pacman -S --needed make ninja pkgconf openssl sdl2 pixman glib2

# On Windows (MSYS2/mingw64)
pacman -S base-devel mingw-w64-x86_64-toolchain git python ninja mingw-w64-x86_64-glib2 mingw-w64-x86_64-pixman python-setuptools mingw-w64-x86_64-SDL2

```
Compile QEMU by running the following commands from the root directory:

```
mkdir build
cd build

# On Intel Macs
../configure --target-list=arm-softmmu --extra-cflags=-I/usr/local/opt/openssl@3/include --extra-ldflags='-L/usr/local/opt/openssl@3/lib -lcrypto'

# On Apple Silicon Macs
../configure --enable-sdl --target-list=arm-softmmu --disable-capstone --disable-pie --disable-slirp --disable-fuse --extra-cflags=-I/opt/homebrew/opt/openssl@3/include --extra-ldflags='-L/opt/homebrew/opt/openssl@3/lib -lcrypto'

# On Linux
../configure --enable-sdl --disable-cocoa --target-list=arm-softmmu --disable-capstone --disable-slirp --extra-cflags=-I/usr/include/openssl --extra-ldflags='-lcrypto' --disable-werror --enable-pie

# On Microsoft Windows (with MINGW64)
../configure --enable-sdl --disable-cocoa --target-list=arm-softmmu --disable-capstone --disable-slirp --disable-pie --extra-cflags=-I/mingw64/include/openssl --extra-ldflags='-L/mingw64/lib -lcrypto' --disable-stack-protector --disable-werror

make
```

Note that we’re explicitly enabling compilation of the SDL library which is used for interaction with the emulator (e.g., capturing keyboard and mouse events). Also, we only configure and build the ARM emulator.
We are also linking against OpenSSL as the AES/SHA1/PKE engines use some of the library’s cryptographic functions.
Remember to update the include and library paths to the OpenSSL library in case they are located elsewhere.
You can speed up the `make` command by passing the number of available CPU cores with the `-j` flag, e.g., use `make -j6` to compile using six CPU cores.
The compilation process should produce the `qemu-system-arm` binary in the `build/arm-softmmu` directory.

#### Troubleshooting the macOS build

On recent macOS (Sequoia/Tahoe) with an up-to-date Homebrew, a few things can trip up the build:

- **`configure` fails with `Ouch! ... found no usable distlib`.** QEMU builds a Python venv and needs `distlib` (or pip's vendored copy). Homebrew's newest Python may not ship it. Install a known-good Python and give it `distlib`:
  ```
  brew install python@3.12
  /opt/homebrew/opt/python@3.12/libexec/bin/python3 -m pip install --user --break-system-packages distlib
  ```
  Then re-run `../configure` with `--python=/opt/homebrew/opt/python@3.12/libexec/bin/python3` added to the flags above.
- **Build fails compiling `block/export/fuse.c`** (`incompatible function pointer types ... fuse_setattr`). Homebrew's `fuse-t`/`fuse3` headers don't match the current macOS SDK. FUSE isn't needed here — the Apple Silicon `configure` line above already passes `--disable-fuse`. If you configured without it, add it and reconfigure.
- Make sure `ninja` and `meson` are installed (they're in the `brew install` line above); older instructions omitted `meson`.

### Downloading the Required Files

We need a few files to successfully boot the iPod Touch emulator to the home screen, which I published [here](https://github.com/devos50/qemu-ios/releases/tag/n72ap_v1) for convenience. You can download all these files from here, and they include the following:
- The S5L8720 bootrom binary, which is started as the very first thing when booting.
- A NOR image that contains various auxillary files used by the bootloader. I provide some instructions on how to generate this image yourself [here](https://github.com/devos50/qemu-ios-generate-nor).
- A NAND image that contains the root file system. I provide some instructions on how to generate this image yourself [here](https://github.com/devos50/qemu-ios-generate-nand).

Download all the required files and save them to a convenient location. You should unzip the `nand_n72ap.zip` file, which contains a single directory named `nand`.

### Running the Emulator

We are now ready to run the emulator from the build directory with the following command:

```
./arm-softmmu/qemu-system-arm -M iPod-Touch,bootrom=<path to bootrom>,nand=<path to NAND directory>,nor=<path to NOR directory> -serial mon:stdio -cpu max -m 2G -d unimp -display sdl
```

The `nand` path is the directory that contains the `cs0`–`cs3` subdirectories (i.e. the `nand` folder produced by unzipping `nand_n72ap.zip`), and `nor` is the `nor_n72ap.bin` file. `-display sdl` opens the device screen; `-d unimp` logs accesses to unimplemented device registers and is safe to drop once things are working.

Interacting with the device (SDL window focused):

| Key | Button      |
| --- | ----------- |
| `H` | Home        |
| `P` | Power       |
| `=` | Volume up   |
| `-` | Volume down |

(These mappings are defined in `ipod_touch_key_event` in `hw/arm/ipod_touch_2g.c`.) Click and drag with the mouse to emulate touch input.

### USB device mode (appearing to the host as a real iPod)

Three machine options turn on USB device mode. All are off by default, so the
normal boot above is unaffected.

| Option | Meaning |
| --- | --- |
| `usb-attached=on` | Report a USB cable as present to the PMU. iOS leaves the entire USB device stack parked until it sees this. |
| `usb-tcp-addr=host:port` | Connect to a USB-over-TCP host bridge. Empty (the default) disables the link. |
| `usb-patch-mux-gate=on` | Patch the kernel so the USB stack goes on bus even though the PTP interface function never registers a driver. **Firmware-build-specific — verified only against 2.1.1 / build 5F138.** |

With a host bridge running (see `usbmuxd-qemu`, a fork of usbmuxd whose USB
backend speaks this transport), the emulated device appears to the standard
`libimobiledevice` tools:

```
./arm-softmmu/qemu-system-arm -M iPod-Touch,bootrom=<bootrom>,nand=<nand dir>,nor=<nor image>,\
    usb-attached=on,usb-patch-mux-gate=on,usb-tcp-addr=127.0.0.1:1235 \
    -serial mon:stdio -cpu max -m 2G -display sdl
```

```
$ USBMUXD_SOCKET_ADDRESS=127.0.0.1:27015 idevice_id -l
0f7085718094b779d89f56b4d62fd23f949897f9
$ USBMUXD_SOCKET_ADDRESS=127.0.0.1:27015 ideviceinfo
DeviceClass: iPod          ProductType: iPod2,1
ProductVersion: 2.1.1      BuildVersion: 5F138
...
```

#### The host bridge, and keeping the two halves in step

The host side is a fork of usbmuxd whose USB backend speaks this transport
instead of libusb: **[samhenrigold/usbmuxd](https://github.com/samhenrigold/usbmuxd), branch `qemu-backend`**.
Only the transport is replaced; the mux protocol itself is untouched upstream code.

The wire protocol between them is specified in **[docs/tcp-usb-protocol.md](docs/tcp-usb-protocol.md)**,
which is canonical for both repositories. The two sides exchange a version
handshake on connect and refuse to attach on a mismatch, so an out-of-step pair
fails at startup with a clear message rather than corrupting transfers.

Known-good pairs are recorded as matching tags in both repositories, so they
stay resolvable regardless of later rebases:

| qemu-ios tag | usbmuxd `qemu-backend` tag | Verified |
| --- | --- | --- |
| `usb-verified-2026-07-30` | `usb-verified-2026-07-30` | `idevice_id -l`, `ideviceinfo`, `idevicepair pair` |

`idevicepair pair` also succeeds. Note the guest needs roughly 100 seconds of
boot before it has programmed the USB core, so the host bridge should wait
before driving a USB reset.

If there are any issues running the above commands, please let me know by [opening an issue](https://github.com/devos50/qemu-ios/issues/new).

## Editing the guest filesystem offline

The emulator serves NAND pages straight out of `cs<N>/<page>.page` with no ECC or
spare-area validation, and the guest volume is a single unjournaled HFSX. So the
filesystem can be edited from the host without going through the emulator's
flash write path at all. `imgtools/editimg.py` does the whole round trip:

```
python3 imgtools/editimg.py --nand <page dir> --script <shell script>
```

It reassembles the volume into a flat image, attaches and mounts it read-write,
runs your script with `$MNT` pointing at the mount, unmounts, checks the result
with `fsck_hfs`, and writes only the changed blocks back. Nothing is written back
unless fsck passes, and it refuses to touch the golden image — always work on a
copy.

This is what makes offline app injection work. A bundle dropped into
`/Applications` is discovered and launches; the installation cache does not need
forging, because there is no separate data partition and `installd` rebuilds the
cache from a directory scan on every boot. Bundles under
`/var/mobile/Applications/<UUID>/` appear on the home screen but do not launch,
so use `/Applications`.

App Store binaries must be **decrypted** (`cryptid 0`) and, more importantly,
built against a 2.x SDK — check `DTSDKName`, not `MinimumOSVersion`. Several
period IPAs declare `MinimumOSVersion 2.0` but were built with the iOS 4 SDK and
silently fail to launch.

## Injecting code into SpringBoard, and getting output back

`imgtools/patch_launchd_env.py` edits a launchd job's plist inside the image:
`--set` adds `EnvironmentVariables` entries, `--set-key` sets top-level keys.

Setting `DYLD_INSERT_LIBRARIES` on SpringBoard works — dyld loads the library
immediately after the main executable and runs its initializers, which is the
mechanism a MobileSubstrate-style tweak relies on. (Injecting a library that
does something *useful* still needs an armv6 dylib, which current toolchains
cannot produce: `ld` refuses armv6, and armv7 output carries load commands that
2.1's dyld cannot parse.)

Because the device has no shell, the way to see a job's output is to redirect it
to a file and read that file back off the host:

```
python3 imgtools/patch_launchd_env.py --nand <copy> \
    --plist com.apple.SpringBoard.plist \
    --set DYLD_INSERT_LIBRARIES=/usr/lib/libstdc++.6.dylib \
    --set DYLD_PRINT_LIBRARIES=1 --set DYLD_PRINT_INITIALIZERS=1 \
    --set-key StandardErrorPath=/tmp/sbdyld.log \
    --set-key StandardOutPath=/tmp/sbdyld.log --apply

python3 imgtools/itdrive.py --nand <copy> --qmp 4510 --out <dir> \
    --boot-wait 105 --nandrw <overlay dir>
# then wait, and search the overlay:
grep -rl 'calling initializer' <overlay dir>
```

Three things make the difference between this working and returning nothing:

- **Use a path the job's user can write.** SpringBoard runs as `mobile`, so
  `/var/log` (root-owned, 0755) silently produces no file. `/tmp` works.
- **Wait at least three minutes** before searching. The guest buffers the write
  and only flushes it to flash on a periodic sync. Measured by polling a live
  overlay every 30 s: nothing at 150 s, present at 180 s. Searching at 60 s or
  140 s finds an overlay with only FTL and system pages in it, which looks
  exactly like the injection having failed.
- **Search the whole overlay, not a fixed page.** Which physical page the log
  lands on varies between runs.

The overlay is a normal copy-on-write directory, so the captured output survives
killing the emulator afterwards.
