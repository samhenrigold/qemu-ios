#!/usr/bin/env python3
"""Install app bundles into a mounted copy of the guest volume.

The NAND image is a single HFSX volume whose fstab is `/dev/disk0s1 / hfs rw`,
with no separate data partition and no
`/private/var/mobile/Library/Caches/com.apple.mobile.installation.plist` - so
installd rebuilds the installation cache on every boot by scanning the app
directories. That means a bundle only has to be *present* to be discovered;
there is no cache file to forge.

Three bundles go in, as a deliberate experiment matrix:
  1. a clone of a stock app in /Applications                 - tests system-app scanning
  2. a clone of a stock app in /var/mobile/Applications/UUID - tests user-app scanning
  3. the real IPA payload in /var/mobile/Applications/UUID   - adds FairPlay (cryptid 1)
If 1 and 2 appear and 3 does not, the blocker is FairPlay, not the injection.
"""

import argparse
import os
import plistlib
import shutil
import subprocess
import sys
import uuid

IPA = "/private/tmp/claude-501/-Users-shg-Developer-qemu-ios/ebb367f9-df80-473b-a8c5-d32a64cc0728/scratchpad/ipa"


def set_modes(root):
    for dirpath, dirs, files in os.walk(root):
        os.chmod(dirpath, 0o755)
        for f in files:
            p = os.path.join(dirpath, f)
            os.chmod(p, 0o755 if is_macho(p) else 0o644)


def is_macho(path):
    try:
        with open(path, "rb") as f:
            magic = f.read(4)
        return magic in (b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xce",
                         b"\xca\xfe\xba\xbe", b"\xcf\xfa\xed\xfe")
    except OSError:
        return False


def clone_stock(mnt, src_name, dst_name, bundle_id, display, dest_dir):
    src = os.path.join(mnt, "Applications", src_name + ".app")
    dst = os.path.join(dest_dir, dst_name + ".app")
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst, symlinks=True)
    exe = os.path.join(dst, src_name)
    if os.path.exists(exe) and src_name != dst_name:
        pass  # keep the executable name; Info.plist still points at it
    ip = os.path.join(dst, "Info.plist")
    with open(ip, "rb") as f:
        info = plistlib.load(f)
    info["CFBundleIdentifier"] = bundle_id
    info["CFBundleDisplayName"] = display
    info["CFBundleName"] = display
    info.pop("SBAppTags", None)
    with open(ip, "wb") as f:
        plistlib.dump(info, f, fmt=plistlib.FMT_BINARY)
    set_modes(dst)
    return dst


def install_user_app(mnt, app_src, metadata_src=None):
    """Place a .app under /var/mobile/Applications/<UUID>/ the way a real install does."""
    u = str(uuid.uuid4()).upper()
    base = os.path.join(mnt, "private", "var", "mobile", "Applications", u)
    os.makedirs(base, exist_ok=True)
    dst = os.path.join(base, os.path.basename(app_src))
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(app_src, dst, symlinks=True)
    for sub in ("Documents", "Library", "Library/Preferences", "tmp"):
        os.makedirs(os.path.join(base, sub), exist_ok=True)
    if metadata_src and os.path.exists(metadata_src):
        shutil.copy2(metadata_src, os.path.join(base, "iTunesMetadata.plist"))
    set_modes(base)
    return u, dst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mnt", required=True)
    a = ap.parse_args()
    mnt = a.mnt

    # 1. stock clone in /Applications
    p1 = clone_stock(mnt, "Calculator", "CalcTwo", "com.example.calctwo",
                     "CalcTwo", os.path.join(mnt, "Applications"))
    print("system-app clone:", p1)

    # 2. stock clone under /var/mobile/Applications
    staging = "/tmp/_stage_calcthree"
    if os.path.exists(staging):
        shutil.rmtree(staging)
    os.makedirs(staging)
    p2src = clone_stock(mnt, "Calculator", "CalcThree", "com.example.calcthree",
                        "CalcThree", staging)
    u2, p2 = install_user_app(mnt, p2src)
    print("user-app clone:   %s  (UUID %s)" % (p2, u2))

    # 3. the real IPA
    app3 = os.path.join(IPA, "FunnyPics", "Payload", "Funny Pics.app")
    meta3 = os.path.join(IPA, "FunnyPics", "iTunesMetadata.plist")
    u3, p3 = install_user_app(mnt, app3, meta3)
    print("real IPA:         %s  (UUID %s)" % (p3, u3))

    shutil.rmtree(staging, ignore_errors=True)
    for junk in (".fseventsd", ".Spotlight-V100", ".Trashes", ".TemporaryItems"):
        shutil.rmtree(os.path.join(mnt, junk), ignore_errors=True)
    subprocess.run(["sync"])


if __name__ == "__main__":
    main()
