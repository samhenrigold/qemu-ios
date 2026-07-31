#!/bin/bash
# Boot with only a subset of a saved overlay in place, to find which persisted
# page makes the next boot fail.
#   bisect-overlay.sh <saved-overlay-dir> <first-index> <last-index> <tag>
# Indices are into the sorted list of .page files in the saved overlay.
# Prints whether the boot reached userland (configd) and whether it rebooted.
set -u
SRC="$1"; LO="$2"; HI="$3"; TAG="$4"
D="/private/tmp/claude-501/-Users-shg-Developer-qemu-ios/ebb367f9-df80-473b-a8c5-d32a64cc0728/scratchpad"
OVL="$HOME/Developer/qemu-ios-files/ovl-nand"

rm -rf "$OVL"; mkdir -p "$OVL"
n=0
(cd "$SRC" && find . -name '*.page' | sort) | while read -r f; do
  if [ "$n" -ge "$LO" ] && [ "$n" -le "$HI" ]; then
    mkdir -p "$OVL/$(dirname "$f")"
    cp "$SRC/$f" "$OVL/$f"
  fi
  n=$((n+1))
done
echo "$TAG: staged $(find "$OVL" -name '*.page' | wc -l | tr -d ' ') pages (idx $LO..$HI)"

"$HOME/Developer/wt-nand/run-nand.sh" "$D/$TAG.serial" 170 >/dev/null 2>&1
echo "$TAG: configd=$(strings "$D/$TAG.serial" | grep -c configd) reboot=$(strings "$D/$TAG.serial" | grep -c 'MACH Reboot') invalid=$(strings "$D/$TAG.serial" | grep -c 'INVALID PAGE')"
