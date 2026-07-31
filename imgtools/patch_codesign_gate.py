#!/usr/bin/env python3
"""Enable USB `ideviceinstaller install` of decrypted apps (install AND launch).

The AppSync-equivalent for the iPod Touch 2G emulator (iPhone OS 2.1.1), applied
as deterministic offline binary patches to a NAND COW clone. Two independent
userspace gates block a decrypted (cryptid 0) app; this clears both:

  BLOCKER 2 - install. MobileInstallation._MobileInstallationInstall's
    verify_executable step calls the imported symbol _MISValidateSignature (from
    /usr/lib/libmis.dylib) and, on nonzero return, fires ApplicationVerificationFailed
    at VerifyingApplication (40%). We patch libmis's MISValidateSignature wrapper
    (thumb, va 0x33a27c58) to `movs r0,#0; bx lr` so it returns success. All 284
    other libmis exports are byte-identical; libmis loads by ordinary two-level
    linking (no DYLD_INSERT), so a bad edit can't abort an unrelated daemon.

  BLOCKER 3 - launch. -[SBIconController launchIcon:] launches an app only if
    -[SBApplication applicationSignatureState] == 2 (trusted). A decrypted app
    under /var/mobile/Applications is a user app with no trusted signer identity,
    so it returns 0 -> SBAppCannotBeOpenedAlertItem ("cannot be opened"). (This is
    why the same binary launches from /Applications, a system app => state 2.) We
    patch applicationSignatureState (arm, va 0x00027b44) to `mov r0,#2; bx lr` so
    every app is trusted and launches.

WHY RE-SIGN. Both files are Apple-signed. amfi_allow_any_signature=1 forgives an
invalid top-level signature but the kernel still hash-checks each code page on
fault-in; a patched page whose CodeDirectory hash no longer matches faults as
*** INVALID PAGE ***. So after each byte patch we `ldid -S` (recompute the CD page
hashes) and cdverify.py confirms every slot matches its bytes.

Files modified in the guest volume:
  /usr/lib/libmis.dylib
  /System/Library/CoreServices/SpringBoard.app/SpringBoard

Does NOT help encrypted App Store apps: those are still FairPlay-encrypted (cryptid
1) and the kernel refuses to exec their mismatched __TEXT pages regardless. Use a
decrypted (cryptid 0) bundle.

Usage:
    imgtools/patch_codesign_gate.py --nand <NAND-COW-clone>
      (reassembles the volume, applies both patches + ldid -S, fsck_hfs, writes the
       changed pages back; refuses the golden image)
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN = "/Users/shg/Developer/qemu-ios-files/nand"

SCRIPT = r"""#!/bin/sh
set -e
HERE="{here}"
LIBMIS="$MNT/usr/lib/libmis.dylib"
SB="$MNT/System/Library/CoreServices/SpringBoard.app/SpringBoard"
python3 "$HERE/patch_libmis.py" --file "$LIBMIS"
ldid -S "$LIBMIS"
python3 "$HERE/cdverify.py" "$LIBMIS" | tail -1
python3 "$HERE/patch_springboard.py" --file "$SB"
ldid -S "$SB"
python3 "$HERE/cdverify.py" "$SB" | tail -1
echo "both codesign gates patched + re-signed"
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", required=True, help="NAND page dir (a COW clone, never the golden image)")
    ap.add_argument("--workdir", default=None)
    ap.add_argument("--blocks", type=int, default=128000)
    a = ap.parse_args()

    if os.path.realpath(a.nand) == os.path.realpath(GOLDEN):
        raise SystemExit("refusing to edit the golden image; work on a COW clone (cp -Rc)")
    for tool in ("ldid",):
        if subprocess.run(["which", tool], capture_output=True).returncode:
            raise SystemExit("missing required tool: %s" % tool)

    with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False) as f:
        f.write(SCRIPT.format(here=HERE))
        script = f.name
    os.chmod(script, 0o755)
    try:
        cmd = [sys.executable, os.path.join(HERE, "editimg.py"),
               "--nand", a.nand, "--script", script, "--blocks", str(a.blocks)]
        if a.workdir:
            cmd += ["--workdir", a.workdir]
        subprocess.run(cmd, check=True)
    finally:
        os.unlink(script)
    print("\nDONE. Modified in the NAND: /usr/lib/libmis.dylib and "
          "/System/Library/CoreServices/SpringBoard.app/SpringBoard")


if __name__ == "__main__":
    main()
