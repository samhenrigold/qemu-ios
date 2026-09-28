#!/usr/bin/env python3
"""The iPad 1 (k48ap) board for imgtools/device.py: create an emulated iPad 1 from a manifest.

    ipad1_device.py create MANIFEST OUTDIR [--seed S] [--activation-hook SCRIPT] [--qemu PATH] [--keep-work]
    (the same as device.py create; the manifest's board picks this module)

build() drives the existing CLIs in the golden recipe's order (docs/ipad1/userland-boot.md), after device.py
has verified the IPSW, decrypted it into the cache and written identity.json: kboot.bin -> MBR
(ipad1_nand.py mbr) -> ipad1_rootfs.py build (no Lockdown, no stash) + bake --seal (+ the manifest's
opt-in activation hook, + --gl-test; the guest-package loader and seed from --guest-package, default
build/guest-package/armv7.itpack, recorded as the lock's guest_package) -> ipad1_nand.py build -> [writable_nor (4.x): ipad1_keybag.py, the
restore-ramdisk data-protection one-shot] -> ipad1_seal.py. OUTDIR gets kboot.bin and nand/ (what the app
consumes), nor.bin when the manifest sets writable_nor, identity.json, device.lock.json and create.log.
Nothing here reads the real unit's dumps (hw2/) or its identity.json.
"""
import json, os, subprocess, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from ipad1_kboot import synth_identity
from ipad1_fw import components
from ipad1_rootfs import GUEST_PACKAGE
from device import sha

CACHE = os.path.expanduser("~/Developer/qemu-ios-files/ipad1/repro/cache")
GEOMETRY = {"16g": "k48-16g"}


def identity(seed, m):
    return synth_identity(seed, m["storage"])


def build(ctx):
    """kboot.bin -> MBR -> system + data volumes -> bake --seal (+ hook) -> NAND store -> seal."""
    m, out, work, dec, opt, step = ctx.m, ctx.out, ctx.work, ctx.dec, ctx.opt, ctx.step
    ident, hook, a = ctx.ident, ctx.hook, ctx.a
    die_id = ":".join(ident["die-id"])
    kboot, mbr = os.path.join(out, "kboot.bin"), os.path.join(work, "mbr.bin")
    step("kboot.bin", [sys.executable, f"{HERE}/ipad1_kboot.py", "--identity", ctx.ident_path, dec, kboot])
    geometry = GEOMETRY[m["storage"]]
    step("mbr", [sys.executable, f"{HERE}/ipad1_nand.py", "mbr", "--geometry", geometry,
                 "--system-mib", str(m["system_mib"]), mbr])
    rootfs = os.path.join(dec, "rootfs.dmg")
    step("system + data volumes", [sys.executable, f"{HERE}/ipad1_rootfs.py", "build", "--base", "pristine",
                                   "--rootfs", rootfs, "--pristine", rootfs, "--mbr", mbr, "--out", work,
                                   "--lockdown", "none", "--stash", "none", "--data-size", m.get("data_size", "partition")]
         + ([] if opt.get("ca_ogl", True) else ["--no-ca-ogl"]) + (["--appsync"] if opt.get("appsync") else [])
         + ([] if opt.get("web_proxy", True) else ["--no-web-proxy"]) + ([] if opt.get("usb_net", True) else ["--no-usb-net"]))
    vols, tools = os.path.join(work, "pristine"), os.path.join(ROOT, "build/ipad1-guest")
    itpack = os.path.abspath(a.guest_package or GUEST_PACKAGE)
    step("bake --seal + guest-package seed" + (" + activation hook" if hook else ""),
         [sys.executable, f"{HERE}/ipad1_rootfs.py", "bake", vols, "--tools", tools, "--guest-package", itpack, "--seal"]
         + (["--activation-hook", hook] if hook else []) + (["--gl-test"] if a.gl_test else []))
    guest_package = json.load(open(os.path.join(vols, "guest-package.json")))
    nand = os.path.join(out, "nand")
    step("NAND store", [sys.executable, f"{HERE}/ipad1_nand.py", "build", "--geometry", geometry, "--mbr", mbr,
                        "--kernelcache", os.path.join(dec, "kernelcache.mach"), "--system", f"{vols}/system.img",
                        "--data", f"{vols}/data.img", "--out", nand])
    if a.keep_work:
        subprocess.run(["cp", "-cR", nand, os.path.join(work, "nand-unsealed")], check=True)
    # A writable NOR carries the effaceable region (data-protection: format, lockers,
    # the system-keybag wrapping key) per device. Blank (erased) unless the manifest
    # opts in; 3.x needs none. The sealing boot's effaceable writes persist into it.
    nor = os.path.join(out, "nor.bin") if opt.get("writable_nor") else None
    ramdisk = None
    if nor:
        with open(nor, "wb") as f:
            f.write(b"\xff" * (1 << 20))
        # 4.x data protection: effaceable storage + the system keybag, made the way a restore makes
        # them, from the IPSW's own (Update) restore ramdisk booted as a SecureRoot (docs/ipad1/ios4.md)
        ramdisk = components(zipfile.ZipFile(ctx.ipsw))["UpdateRamDisk"][:-4] + "-ramdisk.dmg"
        step("data protection: restore-ramdisk keybag one-shot",
             [sys.executable, f"{HERE}/ipad1_keybag.py", nand, nor, "--dec", dec, "--ramdisk", ramdisk,
              "--identity", ctx.ident_path, "--die-id", die_id, "--qemu", a.qemu])
    step("seal", [sys.executable, f"{HERE}/ipad1_seal.py", nand, "--qemu", a.qemu, "--kboot", kboot, "--die-id", die_id]
         + (["--nor-rw", nor] if nor else []))
    baked = sorted(os.listdir(tools))
    gles = os.path.join(ROOT, "contrib/ipad1-gles")
    return {
        "ship": [nand, kboot] + ([nor] if nor else []),
        "built": {"guest tools": {n: sha(os.path.join(tools, n)) for n in baked},
                  "GLEngine": {n: sha(os.path.join(gles, n)) for n in sorted(os.listdir(gles)) if n.startswith("GLEngine-")}
                  if opt.get("ca_ogl", True) else None,
                  "gld plugin": sha(os.path.join(gles, "GLRendererFloatQEMU.bundle/GLRendererFloatQEMU"))
                  if opt.get("ca_ogl", True) else None,
                  "libappsync.dylib": sha(os.path.join(ROOT, "build/appsync/libappsync.dylib")) if opt.get("appsync") else None},
        "inputs": {"rootfs": rootfs, "kernelcache": os.path.join(dec, "kernelcache.mach"),
                   "devicetree": os.path.join(dec, "DeviceTree.bin"),
                   "restore_ramdisk": os.path.join(dec, ramdisk) if ramdisk else None, "mbr": {"path": mbr, "sha256": sha(mbr)},
                   "guest_tools": tools, "lockdown": None, "stash": None},
        "identity": {"die_id": die_id},
        "lock": {"gl_test": a.gl_test, "guest_package": guest_package},
        "outputs": {"kboot": {"path": kboot, "sha256": sha(kboot)},
                    "nand": {"path": nand, "files": {n: sha(os.path.join(nand, n)) for n in sorted(os.listdir(nand))}},
                    "nor": {"path": nor, "sha256": sha(nor)} if nor else None},
    }


def main():
    from device import main as device_main
    device_main()


if __name__ == "__main__":
    main()
