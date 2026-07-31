#!/usr/bin/env python3
"""AppSync-equivalent: force libmis.dylib's MISValidateSignature to return success.

This is the low-risk form of "ship your own libmis". The install-time validation
in MobileInstallation calls the IMPORTED symbol _MISValidateSignature, exported by
/usr/lib/libmis.dylib. That symbol is a thin wrapper:

    _MISValidateSignature:            (thumb, va 0x33a27c58 / file off 0x2c58)
      b580  push {r7,lr}
      af00  add  r7, sp, #0
      2200  movs r2, #0
      f7ff.. bl   MISValidateSignatureAndCopyInfo   ; (path, opts, NULL)
      bd80  pop  {r7,pc}

We overwrite the first two halfwords with a bare "return 0":

      2000  movs r0, #0
      4770  bx   lr

so every caller in the process - MobileInstallation's verify_executable gate
included - gets success, and a decrypted (cryptid 0) bundle installs. This edits
ONLY MISValidateSignature; all 284 other libmis exports (incl.
MISValidateSignatureAndCopyInfo used elsewhere) are byte-for-byte unchanged, so no
consumer loses a symbol. Because libmis loads by ordinary two-level-namespace
linking (NOT DYLD_INSERT), a bad edit could only affect libmis's own consumers,
never abort an unrelated host like lockdownd.

After patching, re-sign with `ldid -S` so the CodeDirectory page hashes match the
patched bytes (amfi_allow_any_signature forgives the invalid top-level signature
but the kernel still hash-checks each page on fault-in). Verify with cdverify.py.

Usage:
    python3 patch_libmis.py --file <libmis.dylib> [--dry-run]
    # or as an editimg script with $MNT set (patches $MNT/usr/lib/libmis.dylib)
"""
import argparse, os, struct, sys

REL_PATH = "usr/lib/libmis.dylib"
TEXT_VMADDR = 0x33A25000
PATCH_VADDR = 0x33A27C58
PATCH_OFF = PATCH_VADDR - TEXT_VMADDR       # 0x2c58
ORIG = 0xAF00B580                            # push {r7,lr}; add r7,sp,#0  (LE halfwords b580 af00)
NEW = 0x47702000                             # movs r0,#0; bx lr           (LE halfwords 2000 4770)


def patch(path, dry_run=False):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    cur = struct.unpack_from("<I", data, PATCH_OFF)[0]
    if cur == NEW:
        print("already patched at off 0x%x (%s)" % (PATCH_OFF, path)); return False
    if cur != ORIG:
        raise SystemExit("unexpected bytes 0x%08X at off 0x%x (expected 0x%08X)"
                         % (cur, PATCH_OFF, ORIG))
    print("MISValidateSignature wrapper found at va 0x%x / off 0x%x" % (PATCH_VADDR, PATCH_OFF))
    if dry_run:
        print("--dry-run: not writing"); return False
    struct.pack_into("<I", data, PATCH_OFF, NEW)
    with open(path, "wb") as f:
        f.write(data)
    print("patched -> movs r0,#0; bx lr : MISValidateSignature now returns success")
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
