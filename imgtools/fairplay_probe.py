#!/usr/bin/env python3
"""Build variants of an injected app to isolate what blocks a FairPlay app.

An unencrypted clone launches from /Applications but the real IPA does not, so
something about the purchased bundle is refused. Three variants, all in
/Applications so the (separate) /var/mobile/Applications problem is out of the
picture:

  FPnoSC   real app, SC_Info removed, cryptid still 1
  FPc0     real app, SC_Info removed, LC_ENCRYPTION_INFO cryptid patched to 0
  CalcSC   unencrypted stock clone with an SC_Info directory added

CalcSC working while FPnoSC fails means encryption is the trigger, not the
presence of SC_Info. FPc0 getting further than "cannot be opened" means the
refusal is the kernel declining to load a Mach-O whose cryptid is set with no
text decrypter registered - i.e. FairPlay, not code signing.
"""

import argparse
import os
import plistlib
import shutil
import struct
import sys

LC_ENCRYPTION_INFO = 0x21


def patch_cryptid(path, value=0):
    """Set cryptid in every LC_ENCRYPTION_INFO of a thin Mach-O."""
    with open(path, "rb") as f:
        buf = bytearray(f.read())
    magic = struct.unpack_from("<I", buf, 0)[0]
    if magic != 0xFEEDFACE:
        raise SystemExit("not a thin 32-bit little-endian Mach-O: 0x%x" % magic)
    ncmds = struct.unpack_from("<I", buf, 16)[0]
    off = 28
    patched = 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", buf, off)
        if cmd == LC_ENCRYPTION_INFO:
            struct.pack_into("<I", buf, off + 16, value)
            patched += 1
        off += size
    if not patched:
        raise SystemExit("no LC_ENCRYPTION_INFO found")
    with open(path, "wb") as f:
        f.write(buf)
    return patched


def set_modes(root):
    for dirpath, _dirs, files in os.walk(root):
        os.chmod(dirpath, 0o755)
        for f in files:
            os.chmod(os.path.join(dirpath, f), 0o755)


def retag(app, bundle_id, display, executable=None):
    ip = os.path.join(app, "Info.plist")
    with open(ip, "rb") as f:
        info = plistlib.load(f)
    info["CFBundleIdentifier"] = bundle_id
    info["CFBundleDisplayName"] = display
    info["CFBundleName"] = display
    info.pop("SBAppTags", None)
    if executable:
        info["CFBundleExecutable"] = executable
    with open(ip, "wb") as f:
        plistlib.dump(info, f, fmt=plistlib.FMT_BINARY)
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mnt", required=True)
    ap.add_argument("--ipa-app", required=True, help="path to the extracted .app")
    a = ap.parse_args()
    apps = os.path.join(a.mnt, "Applications")

    for name, bid, disp in (("FPnoSC", "com.probe.fpnosc", "FPnoSC"),
                            ("FPc0", "com.probe.fpc0", "FPc0")):
        dst = os.path.join(apps, name + ".app")
        shutil.rmtree(dst, ignore_errors=True)
        shutil.copytree(a.ipa_app, dst, symlinks=True)
        shutil.rmtree(os.path.join(dst, "SC_Info"), ignore_errors=True)
        info = retag(dst, bid, disp)
        if name == "FPc0":
            n = patch_cryptid(os.path.join(dst, info["CFBundleExecutable"]), 0)
            print("FPc0: cryptid cleared in %d load command(s)" % n)
        set_modes(dst)
        print("built", dst)

    dst = os.path.join(apps, "CalcSC.app")
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(os.path.join(apps, "Calculator.app"), dst, symlinks=True)
    shutil.copytree(os.path.join(a.ipa_app, "SC_Info"),
                    os.path.join(dst, "SC_Info"))
    retag(dst, "com.probe.calcsc", "CalcSC")
    set_modes(dst)
    print("built", dst)


if __name__ == "__main__":
    main()
