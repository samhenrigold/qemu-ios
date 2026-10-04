#!/bin/sh
# Build an isolated libirecovery with QEMU socket transport; no system install.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?usage: build.sh OUTPUT_DIRECTORY}
revision=93c117c29b1f6669bc4ceca8b84e1df06449fe33
mkdir -p "$out"
for part in src/libirecovery.c include/libirecovery.h; do
    curl -fL "https://raw.githubusercontent.com/libimobiledevice/libirecovery/$revision/$part" -o "$out/$(basename "$part")"
done
python3 "$here/patch.py" "$out/libirecovery.c"
# pkg-config supplies the installed libusb and libimobiledevice-glue paths.
clang -dynamiclib -std=gnu11 -DPACKAGE_VERSION='"qemu-93c117c"' \
    -I"$out" $(pkg-config --cflags libusb-1.0 libimobiledevice-glue-1.0) \
    "$out/libirecovery.c" $(pkg-config --libs libusb-1.0 libimobiledevice-glue-1.0) \
    -Wl,-install_name,@rpath/libirecovery-1.0.5.dylib \
    -Wl,-compatibility_version,7.0.0 -Wl,-current_version,7.2.0 \
    -o "$out/libirecovery-1.0.5.dylib"
