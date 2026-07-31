#!/bin/bash
# Full acceptance test: boot twice on the same overlay.
#   two-boot-test.sh <tag> [seconds-per-boot]
# Boot 2 must show 0 "INVALID PAGE" and reach SpringBoard.
set -u
TAG="$1"
SECS="${2:-130}"
D="/private/tmp/claude-501/-Users-shg-Developer-qemu-ios/ebb367f9-df80-473b-a8c5-d32a64cc0728/scratchpad"
OVL="$HOME/Developer/qemu-ios-files/ovl-nand"

rm -rf "$OVL"; mkdir -p "$OVL"

for n in 1 2; do
  echo "########## $TAG boot $n ##########"
  "$HOME/Developer/wt-nand/run-nand.sh" "$D/$TAG-b$n.serial" "$SECS"
  echo "INVALID PAGE: $(strings "$D/$TAG-b$n.serial" | grep -c 'INVALID PAGE')"
  echo "configd lines: $(strings "$D/$TAG-b$n.serial" | grep -c configd)"
  echo "overlay pages: $(find "$OVL" -name '*.page' | wc -l | tr -d ' ')"
  echo "erased blocks: $(find "$OVL" -name '*.erased' | wc -l | tr -d ' ')"
  sips -s format png "$D/$TAG-b$n.serial.ppm" --out "$D/$TAG-b$n.png" >/dev/null 2>&1 \
    && echo "shot: $D/$TAG-b$n.png"
done
echo "########## $TAG done ##########"
