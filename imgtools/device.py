#!/usr/bin/env python3
"""Create an emulated device from a manifest of declared inputs: a stock IPSW, its keys page and a seed.

    device.py create MANIFEST OUTDIR [--seed S] [--activation-hook SCRIPT] [--qemu PATH] [--guest-package ITPACK]
                     [--gl-test] [--keep-work]

One manifest format, one lock file, every board (manifests/*.json). The shared part, here: verify the IPSW
(sha1, Restore.plist ProductType/ProductBuildVersion/BoardConfig), decrypt it once with ipad1_fw.py into a
cache keyed by its sha1, write identity.json (synthetic, from the seed; mode 600), run the board's build
steps, chmod -R a-w what the board ships, and write device.lock.json (resolved hashes, build, tool git rev,
UDID, every input path). The board's own steps live in its module (BOARDS below): ipad1_device.py for the
iPad 1 (k48ap), ipod2g_device.py for the iPod touch 2G (n72ap).

--seed / --activation-hook override the manifest's identity.seed / activation.hook for this device and are
recorded in the lock. Nothing here reads a real unit's dumps or its identity.json.
"""
import argparse, hashlib, importlib, json, os, plistlib, shutil, subprocess, sys, time, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

BOARDS = {"k48ap": "ipad1_device", "n72ap": "ipod2g_device"}


def sha(path, algo="sha256"):
    h = hashlib.new(algo)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


class Context:
    """What a board's build() gets: the manifest, the decrypted cache, the identity, a step runner."""

    def __init__(self, a, m, out):
        self.a, self.m, self.out = a, m, out
        self.work = os.path.join(out, "work")
        self.opt = m.get("options", {})
        self.log = open(os.path.join(out, "create.log"), "w")
        self.t0 = time.monotonic()

    def say(self, what):
        print("[%5.0fs] %s" % (time.monotonic() - self.t0, what), flush=True)

    def step(self, name, argv, **kw):
        self.say(name)
        self.log.write("\n$ %s\n" % " ".join(argv))
        self.log.flush()
        r = subprocess.run(argv, stdout=self.log, stderr=subprocess.STDOUT, **kw)
        if r.returncode:
            raise SystemExit("%s failed (exit %d); see %s/create.log" % (name, r.returncode, self.out))


def create(a):
    m = json.load(open(a.manifest))
    if m.get("format") != 1:
        raise SystemExit("%s: unknown manifest format %r" % (a.manifest, m.get("format")))
    if m.get("board") not in BOARDS:
        raise SystemExit("%s: no builder for board %r (have %s)" % (a.manifest, m.get("board"), sorted(BOARDS)))
    board = importlib.import_module(BOARDS[m["board"]])
    ipsw, keys = os.path.expanduser(m["ipsw"]["path"]), os.path.expanduser(m["keys"])
    seed = a.seed or m["identity"]["seed"]
    hook = a.activation_hook or (m.get("activation") or {}).get("hook")
    hook = os.path.abspath(os.path.expanduser(hook)) if hook else None
    out = os.path.abspath(a.outdir)
    if os.path.exists(out) and os.listdir(out):
        raise SystemExit("%s exists and is not empty" % out)
    os.makedirs(out, exist_ok=True)
    ctx = Context(a, m, out)
    ctx.hook, ctx.hook_args, ctx.ipsw = hook, a.activation_hook_arg, ipsw

    ctx.say("verify %s" % ipsw)
    got = sha(ipsw, "sha1")
    if got != m["ipsw"]["sha1"]:
        raise SystemExit("%s: sha1 %s, manifest pins %s" % (ipsw, got, m["ipsw"]["sha1"]))
    ctx.restore = restore = plistlib.loads(zipfile.ZipFile(ipsw).read("Restore.plist"))
    found = (restore["ProductType"], restore["ProductBuildVersion"], restore["DeviceMap"][0]["BoardConfig"])
    if found != (m["product_type"], m["build"], m["board"]):
        raise SystemExit("%s is %s, manifest wants %s" % (ipsw, found, (m["product_type"], m["build"], m["board"])))

    ctx.dec = dec = os.path.join(board.CACHE, got)
    if not os.path.exists(os.path.join(dec, ".done")):
        shutil.rmtree(dec + ".tmp", ignore_errors=True)
        os.makedirs(board.CACHE, exist_ok=True)
        ctx.step("decrypt into %s" % dec, [sys.executable, f"{HERE}/ipad1_fw.py", ipsw, keys, dec + ".tmp"])
        open(os.path.join(dec + ".tmp", ".done"), "w").write(json.dumps({"ipsw": ipsw, "keys": keys}))
        os.replace(dec + ".tmp", dec)

    ctx.ident_path = os.path.join(out, "identity.json")
    ctx.ident = ident = board.identity(seed, m)
    fd = os.open(ctx.ident_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(ident, f, indent=1)
    os.makedirs(ctx.work)

    res = board.build(ctx)
    subprocess.run(["chmod", "-R", "a-w"] + res["ship"], check=True)

    ctx.say("device.lock.json")
    rev = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"],
                           capture_output=True, text=True).stdout.strip()
    lock = {
        "format": 1, "created": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "manifest": {"path": os.path.abspath(a.manifest), "sha256": sha(a.manifest), "content": m},
        "build": m["build"], "product_version": restore["ProductVersion"], "product_type": m["product_type"],
        "board": m["board"], "storage": m["storage"],
        "tool": {"repo": ROOT, "git_rev": rev + ("-dirty" if dirty else ""), "qemu": os.path.abspath(a.qemu),
                 "qemu_sha256": sha(a.qemu), "built": res["built"]},
        "inputs": dict({"ipsw": {"path": ipsw, "sha1": got}, "keys": {"path": keys, "sha256": sha(keys)},
                        "decrypted": dec, "identity": ctx.ident_path,
                        "activation_hook": {"path": hook, "sha256": sha(hook), "args": a.activation_hook_arg}
                                           if hook else None}, **res["inputs"]),
        "identity": dict({"seed": seed, "udid": ident["udid"], "sha256": sha(ctx.ident_path)}, **res.get("identity", {})),
        "outputs": res["outputs"],
    }
    lock.update(res.get("lock", {}))
    with open(os.path.join(out, "device.lock.json"), "w") as f:
        json.dump(lock, f, indent=1)
    if not a.keep_work:
        shutil.rmtree(ctx.work)
    ctx.say("%s: UDID %s" % (out, ident["udid"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("create")
    c.add_argument("manifest")
    c.add_argument("outdir")
    c.add_argument("--seed", help="override the manifest's identity.seed")
    c.add_argument("--activation-hook", metavar="SCRIPT", help="override the manifest's activation.hook")
    c.add_argument("--activation-hook-arg", metavar="ARG", action="append", default=[],
                   help="n72ap: pass ARG to the hook before the file (repeatable), e.g. an opt-in the hook asks for")
    c.add_argument("--qemu", default=os.path.join(ROOT, "build/qemu-system-arm"))
    c.add_argument("--guest-package", metavar="ITPACK", help="the arch's .itpack to bake the loader and seed package "
                   "from (default build/guest-package/<arch>.itpack, contrib/guest-package/build.sh)")
    c.add_argument("--gl-test", action="store_true", help="k48ap: bake the GL fixture job (tests/ipad1/gltest.py); a test device")
    c.add_argument("--keep-work", action="store_true", help="keep work/ (volumes and intermediate files)")
    create(ap.parse_args())


if __name__ == "__main__":
    main()
