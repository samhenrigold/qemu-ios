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
import re
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
# The GL front end replaces the framework binary whole: contrib/gles-public (every iPad build; one fat binary),
# contrib/it-gles/gles1x.c (the iPod's 1.x)
OPENGLES = "/System/Library/Frameworks/OpenGLES.framework/OpenGLES"
GL_TARGETS = (MBX, OPENGLES)
# host protocol ranges a package speaks: [oldest, newest]; gles 1 = the name-keyed wire (gles-names.h)
HOST = {"guest-package": [1, 1], "gles": [1, 1]}

IPOD_BIN = {"it_agent": "contrib/it-agent/it_agent", "itmedia": "contrib/it-media/itmedia",
            "itphoto": "contrib/it-media/itphoto", "ittrust": "contrib/it-proxy/ittrust",
            "itproxy": "contrib/it-proxy/itproxy", "itstatus": "contrib/it-status/itstatus",
            "ithalt": "contrib/it-halt/ithalt", "itorient": "contrib/it-orientation/itorient",
            "sbdlicon": "contrib/it-instprogress/sbdlicon", "sblaunch": "contrib/it-gles/sblaunch",
            "it_prefs": "build/ipod-guest/it_prefs"}
# The 2.x/3.0 set shares the same legacy-linked helpers as later armv6 guests.
# Media, networking and status tools retain their own qualification boundary.
IPOD_LEGACY_BIN = {n: IPOD_BIN[n] for n in ("it_agent", "sblaunch", "sbdlicon", "it_prefs")}
# it_prefs (contrib/it-prefs/build-ipod.sh: no Wi-Fi location) runs from the package on every iPod 2G family,
# so devices prepared with it baked get its later settings too.
PREFS_JOB = "contrib/it-prefs/com.qemu.it-prefs.plist"
IPOD_JOBS = ["contrib/it-agent/com.qemu.it-agent.plist", PREFS_JOB]
# it_agent (armv7, contrib/ipad1-guest/build.sh) replaces it_pbd from serial 2: the same pasteboard, plus the
# foreground app, lock state, launch and sync that no stock service answers. Two pasteboard daemons would race.
IPAD_BIN = {n: "build/ipad1-guest/" + n for n in ("it_agent", "it_ethlink", "it_prefs")}
IPAD_JOBS = ["contrib/it-agent/com.qemu.it-agent.plist", "contrib/it-ethlink/com.qemu.it-ethlink.plist",
             PREFS_JOB]
# it_msmquiet: the mounter has already loaded the previous shim when the hook changes, and a respring does not
# drop a notice SpringBoard already holds (tested on 4.2.1), so the next boot's mounter is the one that changes.
IPAD_HOOKS = [("build/ipad1-guest/it_msmquiet.dylib", "/usr/local/lib/it_msmquiet.dylib", False),
              ("build/appsync/libappsync.dylib", "/usr/lib/libappsync.dylib", False)]
# armv7 on 3.0 (the 3GS's 7A341/7A400): the same helpers and hooks, legacy-linked against the 3.1.3 SDK
# (guest-package/build.sh's ipad1-guest-legacy, appsync's libappsync-legacy.dylib)
IPAD_LEGACY_BIN = {n: "build/ipad1-guest-legacy/" + n for n in IPAD_BIN}
IPAD_LEGACY_HOOKS = [("build/ipad1-guest-legacy/it_msmquiet.dylib", "/usr/local/lib/it_msmquiet.dylib", False),
                     ("build/appsync/libappsync-legacy.dylib", "/usr/lib/libappsync.dylib", False)]
# hooks: (source, stock target, respring). The GL shims are one binary per arch: they read the
# firmware's dispatch layout at load (contrib/it-gles/gles_dispatch.c), so no hook is per build.
# builds: exact ids or "<major>*" for every build of that iOS major (2.x = 5*, 3.x = 7*, 4.x = 8*, 5.x = 9*), so a new point
# release needs no row here (LightTouchMac docs/matrix.md).
ARMV7_BOARDS = ["k48ap", "n81ap", "n90ap", "n18ap", "n88ap"]
FAMILIES = {
    # 1.x builds are 3A*/3B* (1.1-1.1.2) and 4A*/4B* (1.1.3-1.1.5), and the iPhone's 1A*/1C* (1.0-1.0.2): no agent
    # or helpers yet, the GL front end only; the legacy-linked it_boot runs there (armv6-toolchain crt1old.c/legacy.h:
    # 1.x's crt1 and stat ABI)
    "n45-ios1": {"arch": "armv6", "boards": ["n45ap", "m68ap"], "builds": ["1*", "3*", "4*"],
                 "hooks": [("contrib/it-gles/OpenGLES-1x", OPENGLES, True)]},
    "n72-ios2": {"arch": "armv6", "boards": ["n72ap"], "builds": ["5*"], "bin": IPOD_LEGACY_BIN,
                 "jobs": IPOD_JOBS,
                 "hooks": [("contrib/gles-public/OpenGLES", OPENGLES, True),
                           ("contrib/it-agent/it_typein.dylib", "/usr/lib/it_typein.dylib", True)]},
    # 3.0 (7A341, the iPod 2G's only 3.0 build) has 3.1's engine ABI but 2.x's dyld (no LC_DYLD_INFO_ONLY):
    # the legacy-linked loader, engine and core helpers. Family by dyld capability, so 3.1+ are listed
    # by build (the iPod's 3.x series is closed).
    "n72-ios30": {"arch": "armv6", "boards": ["n72ap"], "builds": ["7A341"], "bin": IPOD_LEGACY_BIN,
                  "jobs": IPOD_JOBS,
                  "hooks": [("contrib/gles-public/OpenGLES", OPENGLES, True),
                            ("contrib/it-agent/it_typein.dylib", "/usr/lib/it_typein.dylib", True)]},
    "n72-ios3": {"arch": "armv6", "boards": ["n72ap"], "builds": ["7C144", "7C145", "7D11", "7E18"], "bin": IPOD_BIN,
                 "jobs": IPOD_JOBS,
                 "hooks": [("contrib/gles-public/OpenGLES", OPENGLES, True),
                           ("contrib/it-agent/it_typein.dylib", "/usr/lib/it_typein.dylib", True),
                           ("build/appsync/libappsync.dylib", "/usr/lib/libappsync.dylib", False)]},
    # 4.x: it_prefs alone so far; the agent and the rest are still baked there.
    "n72-ios4": {"arch": "armv6", "boards": ["n72ap"], "builds": ["8*"], "bin": {"it_prefs": IPOD_BIN["it_prefs"]},
                 "jobs": [PREFS_JOB]},
    # every armv7 board: the payloads read what differs per board at load, as they do per firmware. The family
    # names stay k48-*, as prepared devices record them in their locks.
    # 3.x by build: 3.0 (the 3GS's 7A*) has 2.x's dyld (no LC_DYLD_INFO_ONLY), so it is k48-ios30's, as the
    # iPod's 3.0 is n72-ios30's (the 3.x series is closed)
    "k48-ios3": {"arch": "armv7", "boards": ARMV7_BOARDS, "builds": ["7B367", "7B405", "7B500", "7C144", "7C145",
                 "7D11", "7E18"], "bin": IPAD_BIN, "jobs": IPAD_JOBS,
                 "hooks": [("contrib/gles-public/OpenGLES", OPENGLES, True)] + IPAD_HOOKS},
    "k48-ios30": {"arch": "armv7", "boards": ["n88ap"], "builds": ["7A341", "7A400"], "bin": IPAD_LEGACY_BIN,
                  "jobs": IPAD_JOBS,
                  "hooks": [("contrib/gles-public/OpenGLES", OPENGLES, True)] + IPAD_LEGACY_HOOKS},
}
# 4.x and 5.x: 3.x's payloads byte for byte. The GL front end, agent and mounter shim read what each changed off the
# firmware at load (docs/ipad1/gles-public-seam.md, docs/ipad1/ios5.md); only the build range differs.
FAMILIES["k48-ios4"] = dict(FAMILIES["k48-ios3"], builds=["8*"])
FAMILIES["k48-ios5"] = dict(FAMILIES["k48-ios3"], builds=["9*"])
# 6.x: the boards that run it (the iPad and the iPod touch 3G stop at 5.1.1).
FAMILIES["k48-ios6"] = dict(FAMILIES["k48-ios3"], boards=["n81ap", "n90ap", "n88ap"], builds=["10*"])
# 7.x (the iPhone 4 only): a read-only root (its launchd cannot `mount -uw /`), so the package is baked at prepare
# time (seed) and changes only on a re-prepare; it_boot installs, swaps hooks and writes state nowhere there.
FAMILIES["k48-ios7"] = dict(FAMILIES["k48-ios3"], boards=["n90ap"], builds=["11*"])
# 1.x/2.x dyld refuses LC_DYLD_INFO_ONLY; everything the loader runs on it must be legacy-linked
LEGACY_BUILDS = ("1*", "3*", "4*", "5*", "7A341", "7A400")


def build_matches(builds, build):
    """requires.builds membership: an exact id, or "<major>*" matching the build's leading number."""
    major = re.match(r"\d+", build).group(0)
    return any(b == build or (b.endswith("*") and b[:-1] == major) for b in builds)

MH_MAGIC, FAT_MAGIC = 0xFEEDFACE, 0xCAFEBABE
LC_MAIN, LC_VERSION_MIN_IPHONEOS, LC_CODE_SIGNATURE, LC_DYLD_INFO_ONLY = 0x80000028, 0x25, 0x1D, 0x80000022
SUBTYPE = {"armv6": 6, "armv7": 9}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def macho_problem(data, arch, legacy=False, signed=False):
    """None if data is a Mach-O (or a fat one with a slice) for arch that old dyld takes (and, armv7 or
    signed=True, signed)."""
    if len(data) >= 8 and struct.unpack_from(">I", data)[0] == FAT_MAGIC:
        for i in range(struct.unpack_from(">I", data, 4)[0]):
            cpu, sub, off, size, _ = struct.unpack_from(">iiIII", data, 8 + 20 * i)
            if (cpu, sub) == (12, SUBTYPE[arch]):
                return macho_problem(data[off:off + size], arch, legacy, signed)
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
    if LC_CODE_SIGNATURE not in cmds and (arch == "armv7" or signed):   # 3.2+ AMFI wants one; iPod helpers ship unsigned
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
    arch, legacy = spec["arch"], any(b in LEGACY_BUILDS or build_matches(LEGACY_BUILDS, b) for b in spec["builds"])
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
    for source, target, respring in spec.get("hooks", []):
        rel = "hooks/" + os.path.basename(target)
        data = read(source)
        if (target in GL_TARGETS or target == "/usr/lib/it_typein.dylib") and family != "n45-ios1" and macho_problem(data, arch, legacy, signed=True):
            # GL maps into every GL process, SpringBoard's included (3.0's even with software CA), and a
            # signed process is killed at an unsigned library's first page (1.x predates code signing)
            raise SystemExit("%s %s: %s" % (family, rel, macho_problem(data, arch, legacy, signed=True)))
        add(rel, data, 0o755, True)
        hooks.append({"file": rel, "target": target, "respring": respring})
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
    if not build_matches(manifest["requires"]["builds"], build):
        raise SystemExit("%s is for %s, not %s" % (manifest["family"], manifest["requires"]["builds"], build))
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    for f in manifest["files"]:
        dst = os.path.join(out, f["name"])
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(os.path.join(pkg, f["name"]), dst)
    with open(os.path.join(out, "offer"), "w") as f:
        f.write(offer_text(manifest, build, good, bad))


HOOK_PROVENANCE = b"file-or-absence 1\n"
SEED_ROOT = "usr/local/lighttouch"
LOADER = ("usr/local/bin/it_boot", "System/Library/LaunchDaemons/com.qemu.it-boot.plist")
LEGACY_LOADER = "loader/it_boot-legacy"   # a legacy-linked family's loader, where the arch's own is modern (armv7)
SYSTEM_VERSION = "System/Library/CoreServices/SystemVersion.plist"


def preserve_hook(mnt, rel, put=None):
    """Keep the original file, or its absence, before installing an override.

    Existing provenance is never overwritten. The returned volume-relative
    backup/marker must be root-owned even when an earlier installer made it.
    """
    baked, absent = rel + ".baked", rel + ".baked-absent"
    have_baked = os.path.lexists(os.path.join(mnt, baked))
    have_absent = os.path.lexists(os.path.join(mnt, absent))
    if have_baked and have_absent:
        raise ValueError("conflicting hook provenance for " + rel)
    if have_absent:
        path = os.path.join(mnt, absent)
        if not os.path.isfile(path) or os.path.islink(path) or os.stat(path).st_size:
            raise ValueError("invalid absent hook marker for " + rel)
        return absent
    if have_baked:
        return baked
    if put is None:
        def put(name, data, mode):
            path = os.path.join(mnt, name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as out:
                out.write(data)
            os.chmod(path, mode)
    path = os.path.join(mnt, rel)
    if os.path.lexists(path):
        st = os.stat(path)
        put(baked, open(path, "rb").read(), st.st_mode & 0o7777)
        return baked
    put(absent, b"", 0o644)
    return absent


def seed(mnt, itpack, gles=True):
    """Bake the loader and the seed package into the system volume mounted at mnt (the preparers, P4).

    The package is the itpack's family for the volume's ProductBuildVersion. It lands as it_boot
    would install it (pkgs/<serial>/ with its `offer` record, `current` -> it, `state` with
    "seed N" and the installed hooks).
    A hook is kept only if its target is on the volume (and, for the GL engines, if the preparer
    installed the shim: `gles`); then <target>.baked keeps what the volume had (the stock file,
    or what the preparer already put there) and target gets the package's bytes, so the first boot
    has nothing to change and no respring, and a package without the hook puts .baked back. Baked launchd jobs the package provides are
    removed: it_boot loads them. Returns (volume-relative paths written, all to be root-owned; the
    lock's guest_package record). FirmwareKit's GuestPackage.seed is the Swift port."""
    entries = dict(read_pack(itpack))
    build = plistlib.load(open(os.path.join(mnt, SYSTEM_VERSION), "rb"))["ProductBuildVersion"]
    fams = [n[:-len("/manifest.json")] for n in entries if n.endswith("/manifest.json")
            and build_matches(json.loads(entries[n])["requires"]["builds"], build)]
    if len(fams) > 1:
        raise SystemExit("%s: %d packages for build %s" % (itpack, len(fams), build))
    if not fams:
        # No family covers this build (a build newer than the itpack): the volume gets no loader and
        # no helpers, and stays stock. The lock records family None so the gap is visible.
        return [], {"family": None, "seed": None, "version": None, "gles": bool(gles),
                    "itpack": {"path": os.path.abspath(itpack), "sha256": sha256(open(itpack, "rb").read())},
                    "hooks": [], "jobs": []}
    family = fams[0]
    m = json.loads(entries[family + "/manifest.json"])
    hooks = [h for h in m["hooks"] if (gles or h["target"] not in GL_TARGETS)
             and os.path.exists(os.path.join(mnt, h["target"][1:]))]
    if any(os.path.lexists(os.path.join(mnt, h["target"][1:] + ".baked-absent")) for h in hooks) \
            and entries.get("loader/hook-provenance") != HOOK_PROVENANCE:
        raise ValueError("guest loader cannot restore absent hook originals; rebuild the guest exports")
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

    legacy = m["requires"].get("link") == "legacy" and LEGACY_LOADER in entries
    put(LOADER[0], entries[LEGACY_LOADER if legacy else "loader/it_boot"], 0o755)
    put(LOADER[1], entries["loader/com.qemu.it-boot.plist"], 0o644)
    pkg = "%s/pkgs/%d" % (SEED_ROOT, m["serial"])
    for f in m["files"]:
        put(pkg + "/" + f["name"], entries[family + "/" + f["name"]], int(f["mode"], 8))
    put(pkg + "/offer", offer_text(m, build).encode(), 0o644)
    os.symlink("pkgs/%d" % m["serial"], os.path.join(mnt, SEED_ROOT, "current"))
    state = b"seed %d\n" % m["serial"]
    put(SEED_ROOT + "/state", state, 0o644)
    made.append(SEED_ROOT + "/current")
    mode = {f["name"]: int(f["mode"], 8) for f in m["files"]}
    for h in hooks:
        rel = h["target"][1:]
        backup = preserve_hook(mnt, rel, put)
        if backup not in made:
            made.append(backup)
        put(rel, entries[family + "/" + h["file"]], mode[h["file"]])
        # The first offer can remove this hook before it_boot ever reads the
        # seed offer. Remember only targets whose installation succeeded.
        state += ("hook %d %s\n" % (bool(h.get("respring", False)), h["target"])).encode()
        put(SEED_ROOT + "/state", state, 0o644)
    for j in m["jobs"]:
        rel = "System/Library/LaunchDaemons/" + os.path.basename(j)
        if os.path.lexists(os.path.join(mnt, rel)):
            os.unlink(os.path.join(mnt, rel))
    record = {"family": family, "seed": m["serial"], "version": m["version"], "gles": bool(gles),
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
        entries += [("loader/it_boot", loader)]
        # armv7's loader is modern-linked; its legacy families (k48-ios30) get the legacy-linked one (seed picks)
        if arch == "armv7":
            legacy_loader = open(os.path.join(src, "build/it-boot/armv7-legacy/it_boot"), "rb").read()
            why = macho_problem(legacy_loader, arch, legacy=True)
            if why:
                raise SystemExit("loader/armv7-legacy: %s" % why)
            entries.append((LEGACY_LOADER, legacy_loader))
        entries += [("loader/hook-provenance", HOOK_PROVENANCE),
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
             "hooks": [{"file": "hooks/MBXGLEngine", "target": MBX, "respring": True}]}
        assert offer_text(m, "7E18", good=[2]).splitlines() == [
            "ltpkg 1", "build 7E18", "serial 3 1.0", "verdict good 2", "file 0 bin/x 755 1 " + "0" * 64,
            "job 1 jobs/j.plist 644 2 " + "1" * 64, "hook 2 hooks/MBXGLEngine 755 3 %s %s respring" % ("2" * 64, MBX)]
        thin = b"\xce\xfa\xed\xfe" + struct.pack("<iiII", 12, 9, 2, 0) + b"\0" * 12
        assert macho_problem(thin, "armv7") == "unsigned (ldid -S)" and macho_problem(thin, "armv6") == "cpu 12/9, not armv6"
        thin6 = b"\xce\xfa\xed\xfe" + struct.pack("<iiII", 12, 6, 8, 0) + b"\0" * 12
        assert macho_problem(thin6, "armv6") is None and macho_problem(thin6, "armv6", signed=True) == "unsigned (ldid -S)"
    # seed bakes a legacy-linked family's own loader where the arch's is modern (armv7.itpack's k48-ios30)
    with tempfile.TemporaryDirectory() as t:
        man = {"serial": 1, "version": "1", "requires": {"builds": ["7A341"], "link": "legacy"}, "files": [],
               "jobs": [], "hooks": []}
        pack([("f/manifest.json", json.dumps(man).encode()), ("loader/it_boot", b"modern"), (LEGACY_LOADER, b"legacy"),
              ("loader/com.qemu.it-boot.plist", b"")], os.path.join(t, "p.itpack"))
        os.makedirs(os.path.join(t, "v", os.path.dirname(SYSTEM_VERSION)))
        plistlib.dump({"ProductBuildVersion": "7A341"}, open(os.path.join(t, "v", SYSTEM_VERSION), "wb"))
        seed(os.path.join(t, "v"), os.path.join(t, "p.itpack"))
        assert open(os.path.join(t, "v", LOADER[0]), "rb").read() == b"legacy"
    # every shipped iPad build has exactly one family, and each carries the agent and the one GL front end
    for build, want in (("7B500", "k48-ios3"), ("8C148", "k48-ios4"), ("8L1", "k48-ios4"), ("9B206", "k48-ios5"),
                        ("10B329", "k48-ios6"), ("10B500", "k48-ios6"), ("11D257", "k48-ios7")):
        for board in FAMILIES[want]["boards"]:
            fams = [f for f, s in FAMILIES.items() if board in s["boards"] and build_matches(s["builds"], build)]
            assert fams == [want], (board, build, fams)
        assert "it_agent" in FAMILIES[want]["bin"]
        assert [(s, t) for s, t, _ in FAMILIES[want]["hooks"] if t in GL_TARGETS] == [("contrib/gles-public/OpenGLES", OPENGLES)]
    # the 3GS's 3.0 is the legacy-linked armv7 family; 3.1+ stay modern
    for build, want in (("7A341", "k48-ios30"), ("7A400", "k48-ios30"), ("7C144", "k48-ios3"), ("7E18", "k48-ios3")):
        fams = [f for f, s in FAMILIES.items() if "n88ap" in s["boards"] and build_matches(s["builds"], build)]
        assert fams == [want], ("n88ap", build, fams)
    assert set(FAMILIES["k48-ios30"]["bin"]) == set(IPAD_BIN) and all("legacy" in p for p in FAMILIES["k48-ios30"]["bin"].values())
    assert [t for _, t, _ in FAMILIES["k48-ios30"]["hooks"]] == [t for _, t, _ in FAMILIES["k48-ios3"]["hooks"]]
    assert all(build_matches(LEGACY_BUILDS, b) for b in FAMILIES["k48-ios30"]["builds"])
    # every iPod 2G build has one family; 3.0's is legacy-linked (its dyld is 2.x's) and carries the engine
    for build, want in (("5F138", "n72-ios2"), ("7A341", "n72-ios30"), ("7C145", "n72-ios3"), ("7E18", "n72-ios3"),
                        ("8C148", "n72-ios4")):
        fams = [f for f, s in FAMILIES.items() if "n72ap" in s["boards"] and build_matches(s["builds"], build)]
        assert fams == [want], (build, fams)
    assert build_matches(LEGACY_BUILDS, "7A341") and not build_matches(LEGACY_BUILDS, "7E18")
    for family in ("n72-ios2", "n72-ios30"):
        spec = FAMILIES[family]
        assert set(spec["bin"]) == {"it_agent", "sblaunch", "sbdlicon", "it_prefs"}
        assert spec["jobs"] == ["contrib/it-agent/com.qemu.it-agent.plist", PREFS_JOB]
        assert [t for _, t, _ in spec["hooks"]] == [OPENGLES, "/usr/lib/it_typein.dylib"]
        # Older guests have no native media qualification yet.
        assert "itmedia" not in spec["bin"]
    # it_prefs and its job ride every iPod 2G family: one delivery path, existing devices included
    for family, spec in FAMILIES.items():
        if "n72ap" in spec["boards"]:
            assert spec["bin"]["it_prefs"] == "build/ipod-guest/it_prefs" and PREFS_JOB in spec["jobs"], family
    assert plistlib.loads(rewrite_job(open(os.path.join(HERE, "../..", PREFS_JOB), "rb").read()))["Label"] \
        == "com.qemu.guest-prefs", "the baked com.qemu.it-prefs job would keep the package's from loading"
    # the one GL front end, byte for byte, wherever a family hooks GL (1.x's is its own: no EAGL, old ObjC)
    assert {s for f in FAMILIES.values() for s, t, _ in f.get("hooks", []) if t in GL_TARGETS} == \
        {"contrib/gles-public/OpenGLES", "contrib/it-gles/OpenGLES-1x"}
    print("PASS: itpack round trip and opacity, job rewrite, offer grammar, Mach-O check, "
          "one family per armv7 board and iPod 2G build")


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
