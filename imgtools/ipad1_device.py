#!/usr/bin/env python3
"""The iPad 1 (k48ap) board for imgtools/device.py: create an emulated iPad 1 from a manifest.

    ipad1_device.py create MANIFEST OUTDIR [--seed S] [--activation-hook SCRIPT] [--qemu PATH] [--keep-work]
    (the same as device.py create; the manifest's board picks this module)

After device.py verifies and decrypts the IPSW, build() generates catalog GID
records, iBoot and NOR, then installs the original IMG3 kernelcache in the system
volume. Both sealing and subsequent boots use real iBoot. OUTDIR contains
iBoot.bin, nor.bin, gid-blobs.bin, nand/, identity.json and device.lock.json.
The existing 4.x keybag preparation still uses an explicit direct-ramdisk boot.
Nothing here reads the real unit's dumps (hw2/) or its identity.json.
"""
import os, subprocess, sys, zipfile
from pathlib import Path
from ipad1_gid import gid_blobs, host_usb_devicetree

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from ipad1_kboot import synth_identity
from ipad1_fw import components
from device import sha

CACHE = os.path.expanduser("~/Developer/qemu-ios-files/ipad1/repro/cache")
GEOMETRY = {"16g": "k48-16g"}


def identity(seed, m):
    return synth_identity(seed, m["storage"])


def build(ctx):
    """Firmware + catalog keys -> volumes -> NAND -> iBoot sealing boot."""
    m, out, work, dec, opt, step = ctx.m, ctx.out, ctx.work, ctx.dec, ctx.opt, ctx.step
    ident, hook, a = ctx.ident, ctx.hook, ctx.a
    die_id = ":".join(ident["die-id"])
    mbr = os.path.join(work, "mbr.bin")
    with zipfile.ZipFile(ctx.ipsw) as z:
        comp = components(z)
        flash = Path(work) / "all_flash"
        flash.mkdir()
        prefix = comp["iBoot"].rsplit("/", 1)[0] + "/"
        for name in z.namelist():
            if name.startswith(prefix) and not name.endswith("/"):
                (flash / os.path.basename(name)).write_bytes(z.read(name))
        kernelcache = os.path.join(work, "kernelcache.img3")
        Path(kernelcache).write_bytes(z.read(comp["KernelCache"]))
        blobs, key_names = gid_blobs(z, os.path.expanduser(m["keys"]))
    gid = os.path.join(out, "gid-blobs.bin")
    Path(gid).write_bytes(blobs)
    dt_img3 = flash / os.path.basename(comp["DeviceTree"])
    dt_img3.write_bytes(host_usb_devicetree(dt_img3.read_bytes(),
                                         Path(dec, "DeviceTree.bin").read_bytes(), blobs))
    iboot, nor = os.path.join(out, "iBoot.bin"), os.path.join(out, "nor.bin")
    step("iBoot + NOR", [sys.executable, f"{HERE}/ipad1_iboot.py", "--iboot", f"{dec}/iBoot.bin",
                        "--all-flash", str(flash), "--identity", ctx.ident_path,
                        "--patcher", a.iboot_patcher, "--out", out])
    geometry = GEOMETRY[m["storage"]]
    step("mbr", [sys.executable, f"{HERE}/ipad1_nand.py", "mbr", "--geometry", geometry,
                 "--system-mib", str(m["system_mib"]), mbr])
    rootfs = os.path.join(dec, "rootfs.dmg")
    step("system + data volumes", [sys.executable, f"{HERE}/ipad1_rootfs.py", "build", "--base", "pristine",
                                   "--rootfs", rootfs, "--pristine", rootfs, "--mbr", mbr, "--out", work,
                                   "--kernelcache", kernelcache,
                                   "--lockdown", "none", "--stash", "none", "--data-size", m.get("data_size", "partition")]
         + ([] if opt.get("ca_ogl", True) else ["--no-ca-ogl"]) + (["--appsync"] if opt.get("appsync") else [])
         + ([] if opt.get("web_proxy", True) else ["--no-web-proxy"]) + ([] if opt.get("usb_net", True) else ["--no-usb-net"]))
    vols, tools = os.path.join(work, "pristine"), os.path.join(ROOT, "build/ipad1-guest")
    step("bake --seal" + (" + activation hook" if hook else ""),
         [sys.executable, f"{HERE}/ipad1_rootfs.py", "bake", vols, "--tools", tools, "--seal"]
         + (["--activation-hook", hook] if hook else []) + (["--gl-test"] if a.gl_test else []))
    nand = os.path.join(out, "nand")
    step("NAND store", [sys.executable, f"{HERE}/ipad1_nand.py", "build", "--geometry", geometry, "--mbr", mbr,
                        "--kernelcache", os.path.join(dec, "kernelcache.mach"), "--system", f"{vols}/system.img",
                        "--data", f"{vols}/data.img", "--out", nand])
    if a.keep_work:
        subprocess.run(["cp", "-cR", nand, os.path.join(work, "nand-unsealed")], check=True)
    # A writable NOR carries the effaceable region (data-protection: format, lockers,
    # the system-keybag wrapping key) per device, alongside its boot images.
    # 4.x prepares the lockers before sealing; 3.x needs no keybag helper.
    ramdisk = None
    if opt.get("writable_nor"):
        # 4.x data protection: effaceable storage + the system keybag, made the way a restore makes
        # them, from the IPSW's own (Update) restore ramdisk booted as a SecureRoot (docs/ipad1/ios4.md)
        ramdisk = components(zipfile.ZipFile(ctx.ipsw))["UpdateRamDisk"][:-4] + "-ramdisk.dmg"
        step("data protection: restore-ramdisk keybag one-shot",
             [sys.executable, f"{HERE}/ipad1_keybag.py", nand, nor, "--dec", dec, "--ramdisk", ramdisk,
              "--identity", ctx.ident_path, "--die-id", die_id, "--qemu", a.qemu])
    step("seal", [sys.executable, f"{HERE}/ipad1_seal.py", nand, "--qemu", a.qemu, "--iboot", iboot, "--gid-blobs", gid,
                  "--nor-rw", nor, "--die-id", die_id])
    baked = sorted(os.listdir(tools))
    gles = os.path.join(ROOT, "contrib/ipad1-gles")
    return {
        "ship": [nand, iboot, nor, gid],
        "built": {"guest tools": {n: sha(os.path.join(tools, n)) for n in baked},
                  "GLEngine": {n: sha(os.path.join(gles, n)) for n in sorted(os.listdir(gles)) if n.startswith("GLEngine-")}
                  if opt.get("ca_ogl", True) else None,
                  "gld plugin": sha(os.path.join(gles, "GLRendererFloatQEMU.bundle/GLRendererFloatQEMU"))
                  if opt.get("ca_ogl", True) else None,
                  "libappsync.dylib": sha(os.path.join(ROOT, "build/appsync/libappsync.dylib")) if opt.get("appsync") else None},
        "inputs": {"rootfs": rootfs, "kernelcache": {"ipsw_member": comp["KernelCache"], "sha256": sha(kernelcache)},
                   "devicetree": os.path.join(dec, "DeviceTree.bin"),
                   "restore_ramdisk": os.path.join(dec, ramdisk) if ramdisk else None, "mbr": {"path": mbr, "sha256": sha(mbr)},
                   "guest_tools": tools, "lockdown": None, "stash": None},
        "identity": {"die_id": die_id},
        "lock": {"gl_test": a.gl_test, "boot_strategy": "iboot",
                 "gid_components": key_names, "iboot_signature_checks": "pattern-patched"},
        "outputs": {"iboot": {"path": iboot, "sha256": sha(iboot)},
                    "gid_blobs": {"path": gid, "sha256": sha(gid)},
                    "nand": {"path": nand, "files": {n: sha(os.path.join(nand, n)) for n in sorted(os.listdir(nand))}},
                    "nor": {"path": nor, "sha256": sha(nor)} if nor else None},
    }


def main():
    from device import main as device_main
    device_main()


if __name__ == "__main__":
    main()
