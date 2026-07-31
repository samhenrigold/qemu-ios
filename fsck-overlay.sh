#!/bin/bash
# Compose base NAND + overlay into a flat HFS+ volume and fsck it.
#   fsck-overlay.sh <tag>
# Uses the allocation-block -> (cs, page) mapping of the generated image:
#   r, cs = divmod(block + 3, 4); eb = 2*(r//256) + 2 + (r%2)
#   page  = eb*128 + (r%256)//2
# The pristine composition fscks clean, so a dirty result means the persisted
# writes are landing on the wrong physical pages.
set -u
TAG="$1"
D="/private/tmp/claude-501/-Users-shg-Developer-qemu-ios/ebb367f9-df80-473b-a8c5-d32a64cc0728/scratchpad"
IMG="$D/vol_$TAG.img"

python3 - "$IMG" <<'EOF'
import os, sys
os.chdir(os.path.expanduser('~/Developer/qemu-ios-files'))
out_path = sys.argv[1]
def loc(b):
    r, cs = divmod(b + 3, 4)
    eb = 2*(r//256) + 2 + (r % 2)
    return cs, eb*128 + (r % 256)//2
used = 0
with open(out_path, 'wb') as out:
    for b in range(128000):
        cs, p = loc(b)
        f = 'ovl-nand/cs%d/%d.page' % (cs, p)
        if os.path.exists(f):
            used += 1
        else:
            f = 'nand/cs%d/%d.page' % (cs, p)
        out.write(open(f, 'rb').read(4096))
print("overlay pages inside the volume:", used)

# how many persisted pages fall outside the filesystem's block space
import glob
total = len(glob.glob('ovl-nand/cs*/*.page'))
print("persisted pages total:", total, "-> outside the volume:", total - used)
EOF

DEV=$(hdiutil attach -nomount -readonly -noverify "$IMG" 2>/dev/null | grep -o '/dev/disk[0-9]*' | head -1)
if [ -z "$DEV" ]; then echo "attach failed"; exit 1; fi
fsck_hfs -n "$DEV" 2>&1 | grep -v "Can't open /dev/rdisk"
hdiutil detach "$DEV" >/dev/null 2>&1
