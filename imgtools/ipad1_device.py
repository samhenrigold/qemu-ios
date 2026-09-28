#!/usr/bin/env python3
"""Create an emulated iPad 1 from a manifest of declared inputs: a stock IPSW, its keys page and a seed.

    ipad1_device.py create MANIFEST OUTDIR [--seed S] [--activation-hook SCRIPT] [--qemu PATH] [--keep-work]

A thin driver over the existing CLIs, in the golden recipe's order (docs/ipad1/userland-boot.md):
verify the IPSW (sha1, Restore.plist ProductType/ProductBuildVersion/BoardConfig) -> ipad1_fw.py into a
cache keyed by the IPSW's sha1 -> identity.json (synthetic, from the seed; mode 600) -> kboot.bin ->
MBR (ipad1_nand.py mbr) -> ipad1_rootfs.py build (no Lockdown, no stash) + bake --seal (+ the manifest's
opt-in activation hook) -> ipad1_nand.py build -> ipad1_seal.py -> chmod -R a-w -> device.lock.json.

OUTDIR gets kboot.bin and nand/ (what the app consumes), identity.json, device.lock.json (resolved hashes,
build, tool git rev, UDID, every input path) and create.log. --seed / --activation-hook override the
manifest's identity.seed / activation.hook for this device and are recorded in the lock.
Nothing here reads the real unit's dumps (hw2/) or its identity.json.
"""
import argparse, hashlib, json, os, plistlib, shutil, subprocess, sys, time, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from ipad1_kboot import synth_identity

CACHE = os.path.expanduser("~/Developer/qemu-ios-files/ipad1/repro/cache")
GEOMETRY = {"16g": "k48-16g"}


def sha(path, algo="sha256"):
    h = hashlib.new(algo)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def create(a):
    m = json.load(open(a.manifest))
    if m.get("format") != 1:
        raise SystemExit("%s: unknown manifest format %r" % (a.manifest, m.get("format")))
    ipsw, keys = os.path.expanduser(m["ipsw"]["path"]), os.path.expanduser(m["keys"])
    seed = a.seed or m["identity"]["seed"]
    hook = a.activation_hook or (m.get("activation") or {}).get("hook")
    hook = os.path.abspath(os.path.expanduser(hook)) if hook else None
    out = os.path.abspath(a.outdir)
    if os.path.exists(out) and os.listdir(out):
        raise SystemExit("%s exists and is not empty" % out)
    os.makedirs(out, exist_ok=True)
    work, log = os.path.join(out, "work"), open(os.path.join(out, "create.log"), "w")
    t0 = time.monotonic()

    def step(name, argv):
        print("[%5.0fs] %s" % (time.monotonic() - t0, name), flush=True)
        log.write("\n$ %s\n" % " ".join(argv))
        log.flush()
        r = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT)
        if r.returncode:
            raise SystemExit("%s failed (exit %d); see %s/create.log" % (name, r.returncode, out))

    print("[    0s] verify %s" % ipsw, flush=True)
    got = sha(ipsw, "sha1")
    if got != m["ipsw"]["sha1"]:
        raise SystemExit("%s: sha1 %s, manifest pins %s" % (ipsw, got, m["ipsw"]["sha1"]))
    restore = plistlib.loads(zipfile.ZipFile(ipsw).read("Restore.plist"))
    found = (restore["ProductType"], restore["ProductBuildVersion"], restore["DeviceMap"][0]["BoardConfig"])
    if found != (m["product_type"], m["build"], m["board"]):
        raise SystemExit("%s is %s, manifest wants %s" % (ipsw, found, (m["product_type"], m["build"], m["board"])))

    dec = os.path.join(CACHE, got)
    if not os.path.exists(os.path.join(dec, ".done")):
        shutil.rmtree(dec + ".tmp", ignore_errors=True)
        os.makedirs(CACHE, exist_ok=True)
        step("decrypt into %s" % dec, [sys.executable, f"{HERE}/ipad1_fw.py", ipsw, keys, dec + ".tmp"])
        open(os.path.join(dec + ".tmp", ".done"), "w").write(json.dumps({"ipsw": ipsw, "keys": keys}))
        os.replace(dec + ".tmp", dec)

    ident_path = os.path.join(out, "identity.json")
    ident = synth_identity(seed, m["storage"])
    fd = os.open(ident_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(ident, f, indent=1)
    die_id = ":".join(ident["die-id"])
    kboot, mbr = os.path.join(out, "kboot.bin"), os.path.join(work, "mbr.bin")
    os.makedirs(work)
    step("kboot.bin", [sys.executable, f"{HERE}/ipad1_kboot.py", "--identity", ident_path, dec, kboot])
    geometry = GEOMETRY[m["storage"]]
    step("mbr", [sys.executable, f"{HERE}/ipad1_nand.py", "mbr", "--geometry", geometry,
                 "--system-mib", str(m["system_mib"]), mbr])
    opt = m.get("options", {})
    rootfs = os.path.join(dec, "rootfs.dmg")
    step("system + data volumes", [sys.executable, f"{HERE}/ipad1_rootfs.py", "build", "--base", "pristine",
                                   "--rootfs", rootfs, "--pristine", rootfs, "--mbr", mbr, "--out", work,
                                   "--lockdown", "none", "--stash", "none", "--data-size", m.get("data_size", "partition")]
         + ([] if opt.get("ca_ogl", True) else ["--no-ca-ogl"]) + (["--appsync"] if opt.get("appsync") else [])
         + ([] if opt.get("web_proxy", True) else ["--no-web-proxy"]) + ([] if opt.get("usb_net", True) else ["--no-usb-net"]))
    vols, tools = os.path.join(work, "pristine"), os.path.join(ROOT, "build/ipad1-guest")
    step("bake --seal" + (" + activation hook" if hook else ""),
         [sys.executable, f"{HERE}/ipad1_rootfs.py", "bake", vols, "--tools", tools, "--seal"]
         + (["--activation-hook", hook] if hook else []))
    nand = os.path.join(out, "nand")
    step("NAND store", [sys.executable, f"{HERE}/ipad1_nand.py", "build", "--geometry", geometry, "--mbr", mbr,
                        "--kernelcache", os.path.join(dec, "kernelcache.mach"), "--system", f"{vols}/system.img",
                        "--data", f"{vols}/data.img", "--out", nand])
    if a.keep_work:
        subprocess.run(["cp", "-cR", nand, os.path.join(work, "nand-unsealed")], check=True)
    step("seal", [sys.executable, f"{HERE}/ipad1_seal.py", nand, "--qemu", a.qemu, "--kboot", kboot, "--die-id", die_id])
    subprocess.run(["chmod", "-R", "a-w", nand, kboot], check=True)

    print("[%5.0fs] device.lock.json" % (time.monotonic() - t0), flush=True)
    rev = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"],
                           capture_output=True, text=True).stdout.strip()
    baked = sorted(os.listdir(tools))
    built = {"guest tools": {n: sha(os.path.join(tools, n)) for n in baked},
             "GLEngine": sha(os.path.join(ROOT, "contrib/ipad1-gles/GLEngine")) if opt.get("ca_ogl", True) else None,
             "libappsync.dylib": sha(os.path.join(ROOT, "build/appsync/libappsync.dylib")) if opt.get("appsync") else None}
    lock = {
        "format": 1, "created": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "manifest": {"path": os.path.abspath(a.manifest), "sha256": sha(a.manifest), "content": m},
        "build": m["build"], "product_version": restore["ProductVersion"], "product_type": m["product_type"], "board": m["board"], "storage": m["storage"],
        "tool": {"repo": ROOT, "git_rev": rev + ("-dirty" if dirty else ""), "qemu": os.path.abspath(a.qemu),
                 "qemu_sha256": sha(a.qemu), "built": built},
        "inputs": {"ipsw": {"path": ipsw, "sha1": got}, "keys": {"path": keys, "sha256": sha(keys)},
                   "decrypted": dec, "rootfs": rootfs, "kernelcache": os.path.join(dec, "kernelcache.mach"),
                   "devicetree": os.path.join(dec, "DeviceTree.bin"), "mbr": {"path": mbr, "sha256": sha(mbr)},
                   "identity": ident_path,
                   "guest_tools": tools, "lockdown": None, "stash": None,
                   "activation_hook": {"path": hook, "sha256": sha(hook)} if hook else None},
        "identity": {"seed": seed, "udid": ident["udid"], "die_id": die_id, "sha256": sha(ident_path)},
        "outputs": {"kboot": {"path": kboot, "sha256": sha(kboot)},
                    "nand": {"path": nand, "files": {n: sha(os.path.join(nand, n)) for n in sorted(os.listdir(nand))}}},
    }
    with open(os.path.join(out, "device.lock.json"), "w") as f:
        json.dump(lock, f, indent=1)
    if not a.keep_work:
        shutil.rmtree(work)
    print("[%5.0fs] %s: UDID %s" % (time.monotonic() - t0, out, ident["udid"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("create")
    c.add_argument("manifest")
    c.add_argument("outdir")
    c.add_argument("--seed", help="override the manifest's identity.seed")
    c.add_argument("--activation-hook", metavar="SCRIPT", help="override the manifest's activation.hook")
    c.add_argument("--qemu", default=os.path.join(ROOT, "build/qemu-system-arm"))
    c.add_argument("--keep-work", action="store_true", help="keep work/ (volumes, MBR, an unsealed store clone)")
    a = ap.parse_args()
    create(a)


if __name__ == "__main__":
    main()
