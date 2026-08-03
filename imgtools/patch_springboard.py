#!/usr/bin/env python3
"""Defeat SpringBoard's launch-time signer-identity TRUST gate.

Independent of the install gate in installd. On tapping an icon,
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

Minimal fix: make applicationSignatureState return 2 unconditionally.

**The address differs per OS version and so does the instruction set.** 2.1.1's
SpringBoard is ARM; 3.1.3's is THUMB, at an entirely different offset. Hardcoding
one of them is why this script silently had no target on 3.1.3 for a long time --
it raised "unexpected bytes" and looked like a corrupt image rather than a wrong
address. Both are listed below and the right one is chosen by matching the actual
prologue bytes, so an unrecognised build fails loudly instead of patching a
random offset.

On 3.1.3 this gate is reached only for apps that installd registered; it is the
LAUNCH half of AppSync and the installd patches are the INSTALL half. You need
both, plus the MISValidateSignature patch inside dyld_shared_cache_armv6 (there
is no on-disk libmis.dylib on 3.1.3).

Re-sign with `ldid -S` afterward -- SpringBoard is Apple-signed and the patched
page's CodeDirectory hash must match or the kernel faults it in as INVALID PAGE.

To re-derive the offset for a build not listed here, find
applicationSignatureState in __objc_classlist with imgtools/objc.py; the method
list stores a THUMB entry point with the low bit set, so mask it off before
subtracting __TEXT vmaddr.

Usage:
    python3 patch_springboard.py --file <SpringBoard> [--dry-run]
    # or as an editimg script with $MNT set
"""
import argparse, os, struct

REL_PATH = "System/Library/CoreServices/SpringBoard.app/SpringBoard"
TEXT_VMADDR = 0x1000

# name, file offset, original bytes, replacement bytes
VARIANTS = [
    # 2.1.1, ARM: ldr r3,[pc,#0x68] ; push {r4,r5,r7,lr}
    #         ->  mov r0,#2         ; bx lr
    ("2.1.1 (ARM)", 0x26B44,
     bytes.fromhex("68309fe5b0402de9"),
     bytes.fromhex("0200a0e31eff2fe1")),
    # 3.1.3, THUMB: push {r4-r7,lr} ; add r7,sp,#12
    #           ->  movs r0,#2      ; bx lr
    ("3.1.3 (THUMB)", 0x17D1C,
     bytes.fromhex("f0b503af"),
     bytes.fromhex("02207047")),
]


def patch(path, dry_run=False):
    with open(path, "rb") as f:
        data = bytearray(f.read())

    for name, off, orig, new in VARIANTS:
        cur = bytes(data[off:off + len(orig)])
        if cur == new:
            print("already patched: %s at off 0x%x (%s)" % (name, off, path))
            return False
        if cur != orig:
            continue

        print("applicationSignatureState: %s at off 0x%x" % (name, off))
        if dry_run:
            print("--dry-run: not writing")
            return False
        data[off:off + len(new)] = new
        with open(path, "wb") as f:
            f.write(data)
        print("patched -> always trusted. Now re-sign:  ldid -S %s" % path)
        return True

    # Say what was actually there for each candidate -- "no variant matched" on
    # its own sends people looking for a corrupt image when the real answer is
    # usually an OS version this table does not know about yet.
    detail = "  ".join(
        "%s: off 0x%x has %s (want %s)"
        % (name, off, bytes(data[off:off + len(orig)]).hex(), orig.hex())
        for name, off, orig, _ in VARIANTS)
    raise SystemExit(
        "no known applicationSignatureState prologue in %s\n  %s\n"
        "If this is a different build, re-derive the offset with "
        "imgtools/objc.py and add a variant above." % (path, detail))


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
