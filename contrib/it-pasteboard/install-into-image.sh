#!/bin/sh
#
# Bake the pasteboard daemon into a NAND page image. Run through editimg.py,
# which sets $MNT to the mounted guest volume:
#
#     cp -Rc <image> <image>-pb
#     imgtools/editimg.py --nand <image>-pb --blocks 1835008 \
#         --script contrib/it-pasteboard/install-into-image.sh
#     imgtools/setowner.py --nand <image>-pb \
#         /usr/local:0:0:755 /usr/local/bin:0:0:755 \
#         /usr/local/bin/it_pbd:0:0:755 \
#         /System/Library/LaunchDaemons/com.qemu.it-pbd.plist:0:0:644
#
# --blocks is the volume size: 1835008 for the 7 GiB images, 128000 for the
# 500 MB ones.
#
# The setowner.py step is NOT optional and cannot be folded in here: editimg
# mounts `noowners` as an ordinary user, so `chown` fails, the resulting files
# are owned by 99:99, and launchd ignores a LaunchDaemon plist it does not see
# as root-owned -- silently. See README.md.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"

mkdir -p "$MNT/usr/local/bin"
cp "$HERE/it_pbd" "$MNT/usr/local/bin/it_pbd"
chmod 755 "$MNT/usr/local/bin/it_pbd" "$MNT/usr/local" "$MNT/usr/local/bin"

cp "$HERE/com.qemu.it-pbd.plist" \
   "$MNT/System/Library/LaunchDaemons/com.qemu.it-pbd.plist"
chmod 644 "$MNT/System/Library/LaunchDaemons/com.qemu.it-pbd.plist"

md5 "$HERE/it_pbd" "$MNT/usr/local/bin/it_pbd"
