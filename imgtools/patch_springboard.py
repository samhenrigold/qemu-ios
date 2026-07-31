#!/usr/bin/env python3
"""Defeat SpringBoard's launch-time signer-identity TRUST gate (blocker 3).

Independent of the install gate (blocker 2). On tapping an icon,
-[SBIconController launchIcon:] launches the app ONLY if
-[SBApplication applicationSignatureState] returns 2 (trusted); otherwise it
raises SBAppProfileNotTrustedAlertItem (state 1) or SBAppCannotBeOpenedAlertItem
(state 0, "cannot be opened").

applicationSignatureState returns 2 only for a system app
(_isSystemApplication & 0x10) or an app whose signer identity
MobileInstallationCopySignerIdentityTrust reports trusted. A decrypted app
installed under /var/mobile/Applications is a user app whose signer identity
failed to load, so it returns 0 -> "cannot be opened". This is why an identical
decrypted binary launches from /Applications (system, state 2) but not from
/var/mobile/Applications.

Minimal fix: make applicationSignatureState (entry va 0x00027b44, ARM) return 2
unconditionally, so every app is treated as trusted and launches:

    ldr r3,[pc,#0x68] (0xE59F3068) ; push {...} (0xE92D40B0)
      ->  mov r0, #2 (0xE3A00002)  ; bx lr (0xE12FFF1E)

Re-sign with ldid -S afterward (SpringBoard is Apple-signed; the patched page's
CodeDirectory hash must match or the kernel faults it in as INVALID PAGE).

Usage:
    python3 patch_springboard.py --file <SpringBoard> [--dry-run]
    # or as an editimg script with $MNT set
"""
import argparse, os, struct

REL_PATH = "System/Library/CoreServices/SpringBoard.app/SpringBoard"
TEXT_VMADDR = 0x1000
ENTRY_VADDR = 0x00027B44
OFF = ENTRY_VADDR - TEXT_VMADDR              # 0x26b44
ORIG = (0xE59F3068, 0xE92D40B0)              # ldr r3,[pc,#0x68] ; push {r4,r5,r7,lr}
NEW = (0xE3A00002, 0xE12FFF1E)               # mov r0,#2 ; bx lr


def patch(path, dry_run=False):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    cur = (struct.unpack_from("<I", data, OFF)[0], struct.unpack_from("<I", data, OFF + 4)[0])
    if cur == NEW:
        print("already patched at off 0x%x (%s)" % (OFF, path)); return False
    if cur != ORIG:
        raise SystemExit("unexpected bytes 0x%08X,0x%08X at off 0x%x (expected 0x%08X,0x%08X)"
                         % (cur[0], cur[1], OFF, ORIG[0], ORIG[1]))
    print("applicationSignatureState found at va 0x%x / off 0x%x" % (ENTRY_VADDR, OFF))
    if dry_run:
        print("--dry-run: not writing"); return False
    struct.pack_into("<I", data, OFF, NEW[0])
    struct.pack_into("<I", data, OFF + 4, NEW[1])
    with open(path, "wb") as f:
        f.write(data)
    print("patched -> mov r0,#2; bx lr : applicationSignatureState now always trusted")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    target = a.file or (os.path.join(os.environ["MNT"], REL_PATH) if os.environ.get("MNT") else None)
    if not target:
        raise SystemExit("give --file or set $MNT")
    patch(target, dry_run=a.dry_run)


if __name__ == "__main__":
    main()
