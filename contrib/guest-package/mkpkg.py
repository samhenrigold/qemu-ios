#!/usr/bin/env python3
"""Assemble guest packages from built guest tools, pack them per arch, compose offers.

    mkpkg.py build SRC OUT        SRC: a tree the contrib/*/build.sh recipes ran in
                                  (build.sh here makes one). Writes OUT/packages/<family>/
                                  {manifest.json, bin/, jobs/, hooks/} and OUT/<arch>.itpack
    mkpkg.py unpack IN.itpack DIR  and check every manifest's hashes
    mkpkg.py offer PKGDIR OUTDIR --build B [--good N] [--bad N]
                                  the offer directory QEMU's guest-package= serves (for the
                                  app's composition in P5 and for tests; OUTDIR is replaced)
    mkpkg.py selfcheck

A package is what contrib/it-boot/it_boot.c installs as /usr/local/lighttouch/pkgs/<serial>.
Its jobs run the package's binaries through /usr/local/lighttouch/current, so they are
rewritten here; hooks replace stock-path files (the preparer keeps a .baked copy of each).

The .itpack is nandpack's idea (contrib/macos-app/nandpack.py) for arbitrary files: one
custom magic, a JSON index and one zlib stream, so the notary service, which opens any
archive it recognises and rejects the unsigned guest Mach-Os inside, sees neither.
"""
import hashlib
import json
import os
import plistlib
import shutil
import struct
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
MAGIC = b"ITPACK01"
CURRENT = "/usr/local/lighttouch/current"
MBX = "/System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine"
GLENGINE = "/System/Library/Frameworks/OpenGLES.framework/GLEngine.bundle/GLEngine"
GLD = "/System/Library/Frameworks/OpenGLES.framework/GLRendererFloatQEMU.bundle/GLRendererFloatQEMU"
# host protocol ranges a package speaks: [oldest, newest]
HOST = {"guest-package": [1, 1], "gles": [0, 0]}

IPOD_BIN = {"it_agent": "contrib/it-agent/it_agent", "itmedia": "contrib/it-media/itmedia",
            "itphoto": "contrib/it-media/itphoto", "ittrust": "contrib/it-proxy/ittrust",
            "itproxy": "contrib/it-proxy/itproxy", "itstatus": "contrib/it-status/itstatus",
            "ithalt": "contrib/it-halt/ithalt", "itorient": "contrib/it-orientation/itorient",
            "sbdlicon": "contrib/it-instprogress/sbdlicon", "sblaunch": "contrib/it-gles/sblaunch"}
# it_agent (armv7, contrib/ipad1-guest/build.sh) replaces it_pbd from serial 2: the same pasteboard, plus the
# foreground app, lock state, launch and sync that no stock service answers. Two pasteboard daemons would race.
IPAD_BIN = {n: "build/ipad1-guest/" + n for n in ("it_agent", "it_ethlink", "it_prefs")}
IPAD_JOBS = ["contrib/it-agent/com.qemu.it-agent.plist", "contrib/it-ethlink/com.qemu.it-ethlink.plist",
             "contrib/it-prefs/com.qemu.it-prefs.plist"]
# it_msmquiet: the mounter has already loaded the previous shim when the hook changes, and a respring does not
# drop a notice SpringBoard already holds (tested on 4.2.1), so the next boot's mounter is the one that changes.
IPAD_HOOKS = [("build/ipad1-guest/it_msmquiet.dylib", "/usr/local/lib/it_msmquiet.dylib", None, False),
              ("build/appsync/libappsync.dylib", "/usr/lib/libappsync.dylib", None, False)]
# hooks: (source, stock target, gli dispatch id or None, respring)
FAMILIES = {
    "n72-ios2": {"arch": "armv6", "boards": ["n72ap"], "builds": ["5F138"], "stub": True},
    "n72-ios3": {"arch": "armv6", "boards": ["n72ap"], "builds": ["7E18"], "bin": IPOD_BIN,
                 "jobs": ["contrib/it-agent/com.qemu.it-agent.plist"],
                 "hooks": [("contrib/it-gles/MBXGLEngine-7E18", MBX, "7E18", True),
                           ("contrib/it-agent/it_typein.dylib", "/usr/lib/it_typein.dylib", None, True),
                           ("build/appsync/libappsync.dylib", "/usr/lib/libappsync.dylib", None, False)]},
    "n72-ios4": {"arch": "armv6", "boards": ["n72ap"], "builds": ["8C148"], "stub": True},
    "k48-ios3": {"arch": "armv7", "boards": ["k48ap"], "builds": ["7B367", "7B500"], "bin": IPAD_BIN,
                 "jobs": IPAD_JOBS,
                 "hooks": [("contrib/ipad1-gles/GLEngine-7B500", GLENGINE, "7B500", True)] + IPAD_HOOKS},
    "k48-ios4": {"arch": "armv7", "boards": ["k48ap"], "builds": ["8C148"], "bin": IPAD_BIN, "jobs": IPAD_JOBS,
                 "hooks": [("contrib/ipad1-gles/GLEngine-8C148", GLENGINE, "8C148", True),
                           ("contrib/ipad1-gles/GLRendererFloatQEMU.bundle/GLRendererFloatQEMU", GLD, "8C148",
                            True)] + IPAD_HOOKS},
}
# 2.x dyld refuses LC_DYLD_INFO_ONLY; everything the loader runs on it must be legacy-linked
LEGACY_BUILDS = ("5F138",)

MH_MAGIC, FAT_MAGIC = 0xFEEDFACE, 0xCAFEBABE
LC_MAIN, LC_VERSION_MIN_IPHONEOS, LC_CODE_SIGNATURE, LC_DYLD_INFO_ONLY = 0x80000028, 0x25, 0x1D, 0x80000022
SUBTYPE = {"armv6": 6, "armv7": 9}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def macho_problem(data, arch, legacy=False):
    """None if data is a Mach-O (or a fat one with a slice) for arch that old dyld takes (and, armv7, signed)."""
    if len(data) >= 8 and struct.unpack_from(">I", data)[0] == FAT_MAGIC:
        for i in range(struct.unpack_from(">I", data, 4)[0]):
            cpu, sub, off, size, _ = struct.unpack_from(">iiIII", data, 8 + 20 * i)
            if (cpu, sub) == (12, SUBTYPE[arch]):
                return macho_problem(data[off:off + size], arch, legacy)
        return "fat, without an %s slice" % arch
    if len(data) < 28 or struct.unpack_from("<I", data)[0] != MH_MAGIC:
        return "not a 32-bit Mach-O"
    cpu, sub, filetype, ncmds = struct.unpack_from("<iiII", data, 4)
    if (cpu, sub) != (12, SUBTYPE[arch]):
        return "cpu %d/%d, not %s" % (cpu, sub, arch)
    cmds, off = set(), 28
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        cmds.add(cmd)
        off += size
    if LC_MAIN in cmds or (filetype == 2 and LC_VERSION_MIN_IPHONEOS in cmds):   # bundles keep theirs
        return "LC_MAIN/LC_VERSION_MIN (not through mkold.py)"
    if legacy and LC_DYLD_INFO_ONLY in cmds:
        return "LC_DYLD_INFO_ONLY (2.x dyld refuses it; LEGACY_LINK=1)"
    if LC_CODE_SIGNATURE not in cmds and arch == "armv7":   # 3.2+ AMFI wants one; the iPod ships unsigned
        return "unsigned (ldid -S)"
    return None


def rewrite_job(data):
    """Point the job's program at the package, through the current symlink."""
    job = plistlib.loads(data)
    args = job.get("ProgramArguments") or [job.get("Program")]
    exe = args[0]
    if not exe or not exe.startswith("/usr/local/bin/"):
        raise SystemExit("job %s: program %r is not a /usr/local/bin helper" % (job.get("Label"), exe))
    job.pop("Program", None)
    job["ProgramArguments"] = [CURRENT + "/bin/" + os.path.basename(exe)] + list(args[1:])
    return plistlib.dumps(job, fmt=plistlib.FMT_XML)


def assemble(src, out, family, spec, serial, version):
    """OUT/<family>/ with its payloads and manifest.json; returns the manifest."""
    arch, legacy = spec["arch"], any(b in LEGACY_BUILDS for b in spec["builds"])
    pkg = os.path.join(out, family)
    shutil.rmtree(pkg, ignore_errors=True)
    os.makedirs(pkg)
    files, jobs, hooks, provides = [], [], [], []

    def read(source):
        path = os.path.join(src, source)
        if not os.path.exists(path):
            raise SystemExit("%s: %s was not built (see the logs)" % (family, source))
        return open(path, "rb").read()

    def add(rel, data, mode, macho):
        why = macho and macho_problem(data, arch, legacy)
        if why:
            raise SystemExit("%s %s: %s" % (family, rel, why))
        path = os.path.join(pkg, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        os.chmod(path, mode)
        files.append({"name": rel, "mode": "%o" % mode, "size": len(data), "sha256": sha256(data)})

    for name, source in sorted(spec.get("bin", {}).items()):
        add("bin/" + name, read(source), 0o755, True)
        provides.append(name)
    for source in spec.get("jobs", []):
        rel = "jobs/" + os.path.basename(source)
        add(rel, rewrite_job(read(source)), 0o644, False)
        jobs.append(rel)
    for source, target, gli, respring in spec.get("hooks", []):
        rel = "hooks/" + os.path.basename(target)
        add(rel, read(source), 0o755, True)
        hooks.append({"file": rel, "target": target, "gli": gli, "respring": respring})
        provides.append(os.path.basename(target))
    manifest = {"format": 1, "serial": serial, "version": version, "family": family, "arch": arch,
                "stub": bool(spec.get("stub")),
                "requires": {"boards": spec["boards"], "builds": spec["builds"],
                             "link": "legacy" if legacy else "modern", "host": HOST},
                "provides": provides, "files": files, "jobs": jobs, "hooks": hooks}
    with open(os.path.join(pkg, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
        f.write("\n")
    return manifest


def pack(entries, out):
    """entries: [(name, bytes)] -> out, one zlib stream behind a JSON index."""
    index = json.dumps({"format": 1, "entries": [{"name": n, "size": len(d)} for n, d in entries]}).encode()
    z = zlib.compressobj(9)
    with open(out + ".tmp", "wb") as f:
        f.write(MAGIC + struct.pack("<I", len(index)) + index)
        for _, data in entries:
            f.write(z.compress(data))
        f.write(z.flush())
    os.replace(out + ".tmp", out)
    if read_pack(out) != entries:
        raise SystemExit("%s: round trip mismatch" % out)


def read_pack(path):
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:8] != MAGIC:
        raise SystemExit("%s: not an .itpack" % path)
    n = struct.unpack_from("<I", blob, 8)[0]
    index = json.loads(blob[12:12 + n])
    data, entries, off = zlib.decompress(blob[12 + n:]), [], 0
    for e in index["entries"]:
        name = e["name"]
        if name.startswith("/") or ".." in name.split("/"):
            raise SystemExit("%s: bad entry name %r" % (path, name))
        entries.append((name, data[off:off + e["size"]]))
        off += e["size"]
    if off != len(data):
        raise SystemExit("%s: index does not cover the stream" % path)
    return entries


def unpack(path, out):
    entries = read_pack(path)
    for name, data in entries:
        dst = os.path.join(out, name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
    for name, data in entries:
        if name.endswith("/manifest.json"):
            pkg = os.path.join(out, os.path.dirname(name))
            for f in json.loads(data)["files"]:
                p = os.path.join(pkg, f["name"])
                os.chmod(p, int(f["mode"], 8))
                if sha256(open(p, "rb").read()) != f["sha256"]:
                    raise SystemExit("%s: %s hash mismatch" % (path, p))
        elif name.startswith("loader/it_boot"):
            os.chmod(os.path.join(out, name), 0o755)
    return entries


def offer_text(manifest, build, good=(), bad=()):
    lines = ["ltpkg 1", "build " + build, "serial %d %s" % (manifest["serial"], manifest["version"])]
    lines += ["verdict good %d" % s for s in good] + ["verdict bad %d" % s for s in bad]
    by_name = {f["name"]: f for f in manifest["files"]}
    hooks = {h["file"]: h for h in manifest["hooks"]}
    for i, f in enumerate(manifest["files"]):
        kind = "hook" if f["name"] in hooks else "job" if f["name"] in manifest["jobs"] else "file"
        line = "%s %d %s %s %d %s" % (kind, i, f["name"], f["mode"], f["size"], by_name[f["name"]]["sha256"])
        if kind == "hook":
            line += " " + hooks[f["name"]]["target"] + (" respring" if hooks[f["name"]]["respring"] else "")
        lines.append(line)
    return "\n".join(lines) + "\n"


def offer(pkg, out, build, good=(), bad=()):
    """Compose the directory guest-package= serves: `offer` plus the payloads at their package paths."""
    manifest = json.load(open(os.path.join(pkg, "manifest.json")))
    if build not in manifest["requires"]["builds"]:
        raise SystemExit("%s is for %s, not %s" % (manifest["family"], manifest["requires"]["builds"], build))
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    for f in manifest["files"]:
        dst = os.path.join(out, f["name"])
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(os.path.join(pkg, f["name"]), dst)
    with open(os.path.join(out, "offer"), "w") as f:
        f.write(offer_text(manifest, build, good, bad))


SEED_ROOT = "usr/local/lighttouch"
LOADER = ("usr/local/bin/it_boot", "System/Library/LaunchDaemons/com.qemu.it-boot.plist")
SYSTEM_VERSION = "System/Library/CoreServices/SystemVersion.plist"


def seed(mnt, itpack, gli=None):
    """Bake the loader and the seed package into the system volume mounted at mnt (the preparers, P4).

    The package is the itpack's family for the volume's ProductBuildVersion. It lands as it_boot
    would install it (pkgs/<serial>/ with its `offer` record, `current` -> it, `state` "seed N").
    A hook is kept only if its target is on the volume and its gli id is None or `gli` (the shim
    the preparer installed; None: no shim); then target and <target>.baked get the package's
    bytes, so the first boot has nothing to change and no respring. Baked launchd jobs the package
    provides are removed: it_boot loads them. Returns (volume-relative paths written, all to be
    root-owned; the lock's guest_package record). FirmwareKit's GuestPackage.seed is the Swift port."""
    entries = dict(read_pack(itpack))
    build = plistlib.load(open(os.path.join(mnt, SYSTEM_VERSION), "rb"))["ProductBuildVersion"]
    fams = [n[:-len("/manifest.json")] for n in entries if n.endswith("/manifest.json")
            and build in json.loads(entries[n])["requires"]["builds"]]
    if len(fams) > 1:
        raise SystemExit("%s: %d packages for build %s" % (itpack, len(fams), build))
    if not fams:
        # No family covers this build (a build newer than the itpack): the volume gets no loader and
        # no helpers, and stays stock. The lock records family None so the gap is visible.
        return [], {"family": None, "seed": None, "version": None, "gli": gli,
                    "itpack": {"path": os.path.abspath(itpack), "sha256": sha256(open(itpack, "rb").read())},
                    "hooks": [], "jobs": []}
    family = fams[0]
    m = json.loads(entries[family + "/manifest.json"])
    hooks = [h for h in m["hooks"] if h["gli"] in (None, gli) and os.path.exists(os.path.join(mnt, h["target"][1:]))]
    dropped = {h["file"] for h in m["hooks"]} - {h["file"] for h in hooks}
    m = dict(m, hooks=hooks, files=[f for f in m["files"] if f["name"] not in dropped])
    made = []

    def put(rel, data, mode):
        missing, parent = [], os.path.dirname(rel)
        while parent and not os.path.isdir(os.path.join(mnt, parent)):
            missing.insert(0, parent)
            parent = os.path.dirname(parent)
        for d in missing:
            os.mkdir(os.path.join(mnt, d))
        with open(os.path.join(mnt, rel), "wb") as f:     # in place: an existing file keeps its catalog record
            f.write(data)
        os.chmod(os.path.join(mnt, rel), mode)
        made.extend(missing + [rel])

    put(LOADER[0], entries["loader/it_boot"], 0o755)
    put(LOADER[1], entries["loader/com.qemu.it-boot.plist"], 0o644)
    pkg = "%s/pkgs/%d" % (SEED_ROOT, m["serial"])
    for f in m["files"]:
        put(pkg + "/" + f["name"], entries[family + "/" + f["name"]], int(f["mode"], 8))
    put(pkg + "/offer", offer_text(m, build).encode(), 0o644)
    os.symlink("pkgs/%d" % m["serial"], os.path.join(mnt, SEED_ROOT, "current"))
    put(SEED_ROOT + "/state", b"seed %d\n" % m["serial"], 0o644)
    made.append(SEED_ROOT + "/current")
    mode = {f["name"]: int(f["mode"], 8) for f in m["files"]}
    for h in hooks:
        for rel in (h["target"][1:], h["target"][1:] + ".baked"):
            put(rel, entries[family + "/" + h["file"]], mode[h["file"]])
    for j in m["jobs"]:
        rel = "System/Library/LaunchDaemons/" + os.path.basename(j)
        if os.path.lexists(os.path.join(mnt, rel)):
            os.unlink(os.path.join(mnt, rel))
    record = {"family": family, "seed": m["serial"], "version": m["version"], "gli": gli,
              "itpack": {"path": os.path.abspath(itpack), "sha256": sha256(open(itpack, "rb").read())},
              "hooks": [h["target"] for h in hooks], "jobs": [os.path.basename(j) for j in m["jobs"]]}
    return made, record


def build(src, out):
    version = dict(l.split(None, 1) for l in open(os.path.join(HERE, "VERSION")).read().splitlines() if l.strip())
    serial, ver = int(version["serial"]), version["version"].strip()
    packages = os.path.join(out, "packages")
    by_arch = {}
    for family, spec in FAMILIES.items():
        m = assemble(src, packages, family, spec, serial, ver)
        entries = [(family + "/manifest.json", open(os.path.join(packages, family, "manifest.json"), "rb").read())]
        entries += [(family + "/" + f["name"], open(os.path.join(packages, family, f["name"]), "rb").read())
                    for f in m["files"]]
        by_arch.setdefault(spec["arch"], []).extend(entries)
        print("%-9s %s serial %d: %d files, %d jobs, %d hooks%s" % (family, spec["arch"], serial, len(m["files"]),
              len(m["jobs"]), len(m["hooks"]), " (stub)" if m["stub"] else ""))
    for arch, entries in sorted(by_arch.items()):
        loader = open(os.path.join(src, "build/it-boot", arch, "it_boot"), "rb").read()
        why = macho_problem(loader, arch, legacy=arch == "armv6")
        if why:
            raise SystemExit("loader/%s: %s" % (arch, why))
        entries += [("loader/it_boot", loader),
                    ("loader/com.qemu.it-boot.plist",
                     open(os.path.join(src, "build/it-boot", arch, "com.qemu.it-boot.plist"), "rb").read())]
        pack(entries, os.path.join(out, arch + ".itpack"))
        print("%s.itpack: %d entries, %d bytes" % (arch, len(entries), os.path.getsize(os.path.join(out, arch + ".itpack"))))


def selfcheck():
    with tempfile.TemporaryDirectory() as t:
        entries = [("a/manifest.json", json.dumps({"files": [{"name": "bin/x", "mode": "755",
                                                              "sha256": sha256(b"x" * 5000)}]}).encode()),
                   ("a/bin/x", b"x" * 5000), ("loader/it_boot", b"\xfe\xed")]
        pack(entries, os.path.join(t, "t.itpack"))
        assert unpack(os.path.join(t, "t.itpack"), os.path.join(t, "u")) == entries
        assert os.stat(os.path.join(t, "u/a/bin/x")).st_mode & 0o777 == 0o755
        blob = open(os.path.join(t, "t.itpack"), "rb").read()
        assert b"bin/x" in blob[:200] and b"xxxx" not in blob     # payload bytes are only in the stream
        job = rewrite_job(plistlib.dumps({"Label": "l", "ProgramArguments": ["/usr/local/bin/it_pbd", "-v"]}))
        assert plistlib.loads(job)["ProgramArguments"] == [CURRENT + "/bin/it_pbd", "-v"]
        m = {"serial": 3, "version": "1.0", "requires": {"builds": ["7E18"]}, "jobs": ["jobs/j.plist"],
             "files": [{"name": "bin/x", "mode": "755", "size": 1, "sha256": "0" * 64},
                       {"name": "jobs/j.plist", "mode": "644", "size": 2, "sha256": "1" * 64},
                       {"name": "hooks/MBXGLEngine", "mode": "755", "size": 3, "sha256": "2" * 64}],
             "hooks": [{"file": "hooks/MBXGLEngine", "target": MBX, "gli": "7E18", "respring": True}]}
        assert offer_text(m, "7E18", good=[2]).splitlines() == [
            "ltpkg 1", "build 7E18", "serial 3 1.0", "verdict good 2", "file 0 bin/x 755 1 " + "0" * 64,
            "job 1 jobs/j.plist 644 2 " + "1" * 64, "hook 2 hooks/MBXGLEngine 755 3 %s %s respring" % ("2" * 64, MBX)]
        thin = b"\xce\xfa\xed\xfe" + struct.pack("<iiII", 12, 9, 2, 0) + b"\0" * 12
        assert macho_problem(thin, "armv7") == "unsigned (ldid -S)" and macho_problem(thin, "armv6") == "cpu 12/9, not armv6"
    print("PASS: itpack round trip and opacity, job rewrite, offer grammar, Mach-O check")


def main():
    a = sys.argv[1:]
    if a[:1] == ["build"] and len(a) == 3:
        build(a[1], a[2])
    elif a[:1] == ["unpack"] and len(a) == 3:
        for name, data in unpack(a[1], a[2]):
            print("%8d %s" % (len(data), name))
    elif a[:1] == ["offer"] and len(a) >= 5 and a[3] == "--build":
        opts = {"--good": [], "--bad": []}
        rest = a[5:]
        for k, v in zip(rest[::2], rest[1::2]):
            opts[k].append(int(v))
        offer(a[1], a[2], a[4], opts["--good"], opts["--bad"])
    elif a == ["selfcheck"]:
        selfcheck()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
