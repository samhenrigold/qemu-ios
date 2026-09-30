#!/usr/bin/env python3
"""The iPod touch 1G (n45ap, iPhone OS 1.x) device bake: the Python oracle for FirmwareKit's N45Recipe.

    ipod1g_device.py prepare SET OUT [--guest-package ITPACK] [--no-gles]

SET is devos50's public n45ap_v1 set (bootrom_s5l8900, iboot_204_n45ap.bin, nor_n45ap.bin, nand/). Its nand/
is a page directory in qemu-ios-generate-nand's layout (bank<N>/<page>.page; the FTL's virtual block 0 at
physical block 201, logical block n at virtual block n + 1, the HFSX volume from LBA 3, one 2048-byte page per
LBA). prepare writes the device OUT: nand/, that directory with bake() applied (every page the bake did not
change is a hard link to SET's, a changed or new page is a new file; SET is never written), bootrom.bin,
iBoot.bin and nor.bin (copies), bake.json (the bake's report) and device.lock.json (board n45ap, the build,
derived.gles_engine), which tests/ipod/regress.py --device reads. The machine boots it as
-M iPod-Touch-1G,bootrom=OUT/bootrom.bin,iboot=OUT/iBoot.bin,nand=OUT/nand,nand-overlay=<its own dir> with a
copy of OUT/nor.bin on -drive if=pflash.

bake(mnt, itpack) is the part N45Recipe has to do the same way, on the mounted 1.x system volume:
  gles     only when the stock OpenGLES exports exactly contrib/it-gles/opengles-1x.exports
           (gles2x_front_end): the guest package's n45-ios1 hook, OpenGLES-1x (gles2x.c built GLES2X_EAGL=0),
           replaces the framework binary (the stock one kept as OpenGLES.baked), and SpringBoard's launchd job gets
           LK_ENABLE_OGL=1, LK_AUTO_ENABLE_OGL=0, LK_ENABLE_MBX2D=0: LayerKit composites through the host. Without
           the hook the job keeps LK_ENABLE_OGL unset (software LayerKit); LK_ENABLE_OGL=1 over the stock IMG
           driver would drive the unemulated MBX, so a package without the hook is refused.
  package  mkpkg.seed: the loader and the seed package, as on every board (the legacy-linked it_boot runs on
           1.x: contrib/armv6-toolchain crt1old.c and legacy.h), so later hooks are delivered as elsewhere.
Every file the bake writes is root-owned afterwards (the host mount is noowners; build_nand.set_owner).
"""
import json, os, plistlib, shutil, struct, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HERE = os.path.join(ROOT, "imgtools")
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "contrib/it-gles"))
sys.path.insert(0, os.path.join(ROOT, "contrib/guest-package"))

OPENGLES = "System/Library/Frameworks/OpenGLES.framework/OpenGLES"
SPRINGBOARD_JOB = "System/Library/LaunchDaemons/com.apple.SpringBoard.plist"
GUEST_PACKAGE = os.path.join(ROOT, "build/guest-package/armv6.itpack")
EXPORTS = os.path.join(ROOT, "contrib/it-gles/opengles-1x.exports")

PAGE, SPARE, BANKS, PPB = 2048, 64, 8, 128
FTL_START, FIRST_LBA = 201, 3
# devos50's data pages all carry this spare (no lpn, no age); a page the bake adds gets it too
PLACEHOLDER_SPARE = bytes(10) + b"\xff" + bytes(SPARE - 11)


def location(lpn):
    """(bank, page) of a logical page in devos50's layout (logical block n = virtual block n + 1)."""
    v = lpn + PAGE // 2 + FTL_START * PPB * BANKS
    return v % BANKS, v // (PPB * BANKS) * PPB + (v // BANKS) % PPB


def page_path(nand, lpn):
    b, p = location(lpn)
    return os.path.join(nand, "bank%d" % b, "%d.page" % p)


def read_volume(nand, img):
    """The flat HFSX volume (sparse: unwritten pages stay holes). Returns its size in pages."""
    hdr = open(page_path(nand, FIRST_LBA), "rb").read()[1024:1536]
    if hdr[:2] not in (b"H+", b"HX"):
        raise SystemExit("%s: no HFS+ volume header at LBA %d" % (nand, FIRST_LBA))
    bs, total = struct.unpack_from(">II", hdr, 40)
    pages = bs * total // PAGE
    with open(img, "wb") as out:
        out.truncate(bs * total)
        for i in range(pages):
            try:
                d = open(page_path(nand, FIRST_LBA + i), "rb").read(PAGE)
            except FileNotFoundError:
                continue
            if any(d):
                out.seek(i * PAGE)
                out.write(d)
    return pages


def write_nand(base, img, pages, out):
    """out = base with the pages of img that differ; returns how many were written."""
    for d, _, names in os.walk(base):
        rel = os.path.relpath(d, base)
        os.makedirs(os.path.join(out, rel), exist_ok=True)
        for n in names:
            os.link(os.path.join(d, n), os.path.join(out, rel, n))
    changed = 0
    with open(img, "rb") as f:
        for i in range(pages):
            new = f.read(PAGE)
            src = page_path(base, FIRST_LBA + i)
            try:
                old = open(src, "rb").read()
            except FileNotFoundError:
                old = None
            if (old[:PAGE] if old else bytes(PAGE)) == new:
                continue
            dst = page_path(out, FIRST_LBA + i)
            if os.path.exists(dst):
                os.unlink(dst)          # a hard link to base: never write through it
            with open(dst, "wb") as g:
                g.write(new + (old[PAGE:PAGE + SPARE] if old else PLACEHOLDER_SPARE))
            changed += 1
    return changed


def gles2x_front_end(mnt):
    """(True, line) if the stock OpenGLES exports exactly opengles-1x.exports, else (False, why)."""
    import gles2x_exports
    stock = os.path.join(mnt, OPENGLES)
    if not os.path.exists(stock):
        return False, "no %s" % OPENGLES
    if os.path.exists(stock + ".baked"):
        stock += ".baked"
    want, got = set(gles2x_exports.read_list(EXPORTS)), set(gles2x_exports.scan(stock))
    if want != got:
        return False, "stock OpenGLES exports differ from opengles-1x.exports (missing %s, extra %s): stock kept" % (
            sorted(want - got)[:4], sorted(got - want)[:4])
    return True, "GL front end replaces OpenGLES (%d exports, the firmware's own)" % len(got)


def springboard_env(mnt, ogl):
    path = os.path.join(mnt, SPRINGBOARD_JOB)
    data = open(path, "rb").read()
    job = plistlib.loads(data)
    env = job.setdefault("EnvironmentVariables", {})
    if ogl:
        env.update(LK_ENABLE_OGL="1", LK_AUTO_ENABLE_OGL="0")
    else:
        env.pop("LK_ENABLE_OGL", None)
        env.pop("LK_AUTO_ENABLE_OGL", None)
    env["LK_ENABLE_MBX2D"] = "0"            # never the unemulated MBX 2D path (devos50's image already says so)
    open(path, "wb").write(plistlib.dumps(job, fmt=plistlib.FMT_BINARY if data.startswith(b"bplist") else plistlib.FMT_XML))


def bake(mnt, itpack=GUEST_PACKAGE, gles=True):
    """Bake the mounted 1.x system volume; returns (report, root-owned volume-relative paths)."""
    import mkpkg
    report, owners = {}, [SPRINGBOARD_JOB]
    front, why = gles2x_front_end(mnt) if gles else (False, "gles off")
    seeded, report["guest_package"] = mkpkg.seed(mnt, itpack, front)
    if front and "/" + OPENGLES not in report["guest_package"]["hooks"]:
        raise SystemExit("%s has no OpenGLES hook for this build; rebuild contrib/guest-package" % itpack)
    springboard_env(mnt, front)
    report["gles"] = why + ("; LayerKit composites through it (LK_ENABLE_OGL=1)" if front else "; software LayerKit")
    report["gles_engine"] = "OpenGLES" if front else None
    return report, owners + seeded


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


SET_FILES = {"bootrom.bin": "bootrom_s5l8900", "iBoot.bin": "iboot_204_n45ap.bin", "nor.bin": "nor_n45ap.bin"}


def prepare(set_dir, out, itpack, gles):
    from build_nand import set_owner
    if os.path.exists(out):
        raise SystemExit("%s exists" % out)
    nand = os.path.join(set_dir, "nand")
    work = tempfile.mkdtemp(prefix="ipod1g.")
    try:
        img, mnt = os.path.join(work, "volume.img"), os.path.join(work, "mnt")
        os.makedirs(mnt)
        pages = read_volume(nand, img)
        dev = run(["hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage", "-nomount", img]).split()[0]
        try:
            run(["diskutil", "mount", "-mountPoint", mnt, dev])
            try:
                report, owned = bake(mnt, itpack, gles)
                version = plistlib.load(open(os.path.join(mnt, "System/Library/CoreServices/SystemVersion.plist"), "rb"))
                for junk in (".fseventsd", ".Spotlight-V100", ".Trashes", ".TemporaryItems"):
                    shutil.rmtree(os.path.join(mnt, junk), ignore_errors=True)
            finally:
                subprocess.run(["diskutil", "unmount", mnt], capture_output=True)
            set_owner(img, owned, 0, 0)
            fsck = subprocess.run(["fsck_hfs", "-fn", dev], capture_output=True, text=True).stdout
            # devos50's volume already fails on directory folder counts alone (an HFSX field 1.x never keeps)
            problems = [l for l in fsck.splitlines() if l.startswith("   ")
                        and not l.strip().startswith(("Executing fsck_hfs", "The volume name is", "HasFolderCount flag",
                                                      "Incorrect folder count", "(It should be"))]
            if "appears to be OK" not in fsck and problems:
                raise SystemExit("fsck_hfs is not happy; nothing written:\n" + "\n".join(problems[:20]))
        finally:
            subprocess.run(["hdiutil", "detach", dev], capture_output=True)
        report["pages_written"] = write_nand(nand, img, pages, os.path.join(out, "nand"))
        report["base"] = os.path.abspath(nand)
        for name, src in SET_FILES.items():
            shutil.copyfile(os.path.join(set_dir, src), os.path.join(out, name))
        json.dump(report, open(os.path.join(out, "bake.json"), "w"), indent=1)
        json.dump({"board": "n45ap", "product_version": version["ProductVersion"],
                   "build": version["ProductBuildVersion"], "machine": {},
                   "derived": {"gles_engine": report["gles_engine"]}},
                  open(os.path.join(out, "device.lock.json"), "w"), indent=1)
        print("bake:", json.dumps(report))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    a = sys.argv[1:]
    if a[:1] == ["prepare"] and len(a) >= 3:
        itpack = a[a.index("--guest-package") + 1] if "--guest-package" in a else GUEST_PACKAGE
        prepare(a[1], a[2], itpack, "--no-gles" not in a)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
