#!/bin/bash
# Automated bisect: find the smallest prefix of the saved overlay that makes the
# next boot fail. Assumes prefix 0..LO-1 is good and 0..HI is bad.
set -u
SRC="$1"; LO="$2"; HI="$3"
while [ "$LO" -lt "$HI" ]; do
  MID=$(( (LO + HI) / 2 ))
  OUT=$("$HOME/Developer/wt-nand/bisect-overlay.sh" "$SRC" 0 "$MID" "bs-0-$MID" | tail -1)
  echo "$OUT"
  if echo "$OUT" | grep -q "configd=0"; then HI="$MID"; else LO=$((MID+1)); fi
  echo "  -> narrowed to $LO..$HI"
done
echo "FIRST BAD PREFIX ENDS AT INDEX $LO"
(cd "$SRC" && find . -name '*.page' | sort | sed -n "$((LO+1))p")
