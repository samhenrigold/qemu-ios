#!/usr/bin/env python3
"""The iPod touch 2G (n72ap) board for imgtools/device.py: create an emulated iPod from a manifest.

    device.py create manifests/ipod2g-7E18.json OUTDIR
    ipod2g_device.py bake MNT CONFIG      (build_nand.py --script runs this inside the mounted volume)

After device.py has verified the IPSW, decrypted it into CACHE and written identity.json, build():

  nor.bin   build_nor.py --identity: IMG2 + SysCfg (Mod#, Regn, SrNm, Batt) + nvram (btaddr, wifiaddr) made
            from the identity, the IPSW's all_flash images packed after it, each SHSH wrapped for the emulated
            UID when the firmware's iBoot unwraps (ProductVersion >= 3; 2.x iBoot verifies them raw)
  iBoot.bin the IPSW's iBoot, decrypted (the machine's direct-iboot)
  gid-blobs.bin  KBAG || IV-key for each img3 the keys page covers (the machine's gid-blobs: the emulated AES
            engine has no GID key, so it answers a KBAG from this table)
  nand/     build_nand.py over the IPSW rootfs: grown to `volume_blocks`, fstab rw, the IPSW kernelcache
            (still encrypted; the emulated AES engine decrypts it) at the path the decrypted iBoot names,
            the flash bookkeeping generated for the firmware's NAND epoch (Restore.plist SCEP), and bake()

bake() adds the guest side, all of it located at build or run time (no offsets):
  imgtools/bake-guest-tools.sh   it_agent + it_typein (SpringBoard DYLD_INSERT), sblaunch, sbdlicon, sound
                                 defaults, the .lt-guest-tools markers, and the MBXGLEngine shim only if the
                                 firmware's __GLIFunctionDispatchRec @encode is docs/ipod/gli-dispatch-7E18.tsv
                                 (else the stock engine and software CoreAnimation)
  contrib/appsync/patch-appsync-dylib.sh  MISValidateSignature -> success in the shared cache, found by symbol;
                                 libappsync.dylib DYLD_INSERTed into installd (options.appsync)
  shell package                  /bin/sh (bash) and the tools it_agent's exec runs, from the Cydia bootstrap
                                 tarballs the manifest pins (imgtools/ipod2g-shell.txt); with options.ssh also
                                 OpenSSH + a /Library/LaunchDaemons job and host keys made for this device
Every file the bake creates is given its owner in the catalog afterwards (the host mount is noowners).
"""
import hashlib, json, os, plistlib, re, shutil, struct, subprocess, sys, tarfile, tempfile, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from device import sha
from ipad1_kboot import synth_identity, udid

CACHE = os.path.expanduser("~/Developer/qemu-ios-files/ipod-ipsw/cache")
KC_PREFIX = b"/System/Library/Caches/com.apple.kernelcaches/"
GLI_REF = os.path.join(ROOT, "docs/ipod/gli-dispatch-7E18.tsv")
SHELL_LIST = os.path.join(HERE, "ipod2g-shell.txt")
DYLD_CACHE = "System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv6"
SSHD_JOB = "Library/LaunchDaemons/com.openssh.sshd.plist"
SSHD_CONFIG = """Port 22
Protocol 2
HostKey /etc/ssh/ssh_host_rsa_key
HostKey /etc/ssh/ssh_host_ecdsa_key
UsePrivilegeSeparation no
PermitRootLogin yes
PasswordAuthentication yes
PubkeyAuthentication yes
PermitEmptyPasswords no
ChallengeResponseAuthentication no
UsePAM no
StrictModes no
UseDNS no
GSSAPIAuthentication no
KerberosAuthentication no
PrintMotd no
PrintLastLog no
TCPKeepAlive yes
LoginGraceTime 300
MaxAuthTries 10
PidFile /var/run/sshd.pid
LogLevel DEBUG3
Subsystem sftp /usr/libexec/sftp-server
AcceptEnv LANG LC_*
"""
# Paths bake-guest-tools.sh creates (it documents the setowner step); owner, relative to the volume.
GUEST_TOOL_OWNERS = [
    ("0 0", "usr/local"), ("0 0", "usr/local/bin"), ("0 0", "usr/local/bin/it_agent"),
    ("0 0", "usr/local/bin/sblaunch"), ("0 0", "usr/local/bin/sbdlicon"), ("0 0", "usr/lib/it_typein.dylib"),
    ("0 0", "System/Library/LaunchDaemons/com.qemu.it-agent.plist"),
    ("0 0", "System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine.stock"),
    ("501 501", "private/var/mobile/Library/Preferences/com.apple.mobilemail.plist"),
    ("501 501", "private/var/mobile/Library/Preferences/com.apple.springboard.plist"),
    ("501 501", "private/var/mobile/Library/Preferences/com.apple.preferences.sounds.plist"),
    ("501 501", "private/var/mobile/Media/.lt-guest-tools-v1"),
    ("501 501", "private/var/mobile/Media/.lt-guest-tools-v2"),
]


def identity(seed, m):
    """The iPad's synthetic serial and MACs (ipad1_kboot.synth_identity) plus a 12-digit battery serial;
    Mod#/Regn are the manifest's. UDID = SHA1(serial + Wi-Fi MAC + BT MAC), the Wi-Fi-only formula."""
    base = synth_identity(seed)
    h = hashlib.sha256(("battery:" + seed).encode()).digest()
    ident = {"serial-number": base["serial-number"], "wifi-mac": base["wifi-mac"], "bt-mac": base["bt-mac"],
             "battery-serial": "".join(str(b % 10) for b in h[:12]),
             "model-number": m["model_number"], "region-info": m["region_info"], "seed": seed}
    ident["udid"] = udid(ident)
    return ident


def kernelcache_path(iboot):
    """The volume path the decrypted iBoot loads the kernelcache from (its one KC_PREFIX string)."""
    hits = set(re.findall(re.escape(KC_PREFIX) + rb"[\x21-\x7e]+", iboot))
    if len(hits) != 1:
        raise SystemExit("iBoot names %d kernelcache paths (%s); expected exactly one" % (len(hits), sorted(hits)))
    return hits.pop()[1:].decode()


def darwin_banner(kernel):
    m = re.search(rb"Darwin Kernel Version [^\0]+", kernel)
    return m[0].decode() if m else None


def gli_fields(cache):
    encs = set(re.findall(rb"\{__GLIFunctionDispatchRec=[^}]*\}", cache))
    if len(encs) != 1:
        return None
    return [f.decode() for f in re.findall(rb'"([^"]+)"', encs.pop())]


def gli_abi_problem(cache_path, ref=GLI_REF):
    """None if the firmware's GL dispatch table is the one the shim is built for, else why not."""
    if not os.path.exists(cache_path):
        return "no dyld shared cache (2.x)"
    have = gli_fields(open(cache_path, "rb").read())
    if have is None:
        return "no (or more than one) __GLIFunctionDispatchRec @encode in the shared cache"
    want = [l.rstrip("\n").split("\t")[1] for l in open(ref) if l[:1].isdigit()]
    if have != want:
        at = next((i for i, (h, w) in enumerate(zip(have, want)) if h != w), min(len(have), len(want)))
        return "dispatch table differs from %s at slot %d (%d vs %d slots)" % (os.path.basename(ref), at, len(have), len(want))
    return None


def kbag(img3):
    """The production KBAG's 32-byte ciphertext (IV || AES-128 key under the GID key), or None."""
    from ipad1_fw import img3_tags
    off, dlen = img3_tags(img3).get("KBAG", (None, 0))
    if off is None or dlen < 40:
        return None
    state, aes = struct.unpack_from("<II", img3, off + 12)
    return img3[off + 20:off + 52] if state == 1 and aes == 128 else None


def gid_blobs(z, members, keysfile):
    """KBAG || IV-key records for every img3 the keys page has an IV/Key for (the AES engine's GID table)."""
    text = open(keysfile).read()
    keys = {m[1]: bytes.fromhex(m[2] + m[3]) for m in re.finditer(r"\n(\S+)\nIV: (\w+)\nKey: (\w+)", text)}
    out, names = bytearray(), []
    for n in members:
        k, plain = kbag(z.read(n)), keys.get(os.path.basename(n))
        if k is not None and plain is not None and len(plain) == 32:
            out += k + plain
            names.append(os.path.basename(n))
    return bytes(out), names


def zip_member(z, name, dst):
    with z.open(name) as s, open(dst, "wb") as d:
        shutil.copyfileobj(s, d)


def build(ctx):
    m, out, work, dec, opt, step = ctx.m, ctx.out, ctx.work, ctx.dec, ctx.opt, ctx.step
    if ctx.a.gl_test:
        raise SystemExit("--gl-test: the GL fixture job is k48ap only")
    z = zipfile.ZipFile(ctx.ipsw)
    restore = ctx.restore
    major = int(restore["ProductVersion"].split(".")[0])
    epoch = restore["DeviceMap"][0]["SCEP"]
    iboot = open(os.path.join(dec, "iBoot.bin"), "rb").read()
    kc_path = kernelcache_path(iboot)
    # BuildManifest (3.x+) names the kernelcache; 2.x has only Restore.plist's KernelCachesByPlatform
    kc_member = plistlib.loads(z.read("BuildManifest.plist"))["BuildIdentities"][0]["Manifest"]["KernelCache"]["Info"]["Path"] \
        if "BuildManifest.plist" in z.namelist() else restore["KernelCachesByPlatform"]["s5l8720x"]["Release"]
    derived = {"nand_epoch": epoch, "wrap_shsh": major >= 3, "kernelcache_path": kc_path,
               "kernelcache_member": kc_member,
               "kernel": darwin_banner(open(os.path.join(dec, "kernelcache.mach"), "rb").read()),
               "iboot": (re.search(rb"iBoot-[0-9.]+", iboot) or [b"?"])[0].decode()}

    # NOR
    af = os.path.join(work, "all_flash")
    os.makedirs(af)
    prefix = "Firmware/all_flash/all_flash.%s.production/" % m["board"]
    for n in z.namelist():
        if n.startswith(prefix) and not n.endswith("/"):
            zip_member(z, n, os.path.join(af, os.path.basename(n)))
    nor = os.path.join(out, "nor.bin")
    # the stock NOR's image set, less what this firmware's all_flash manifest does not ship (4.x: no nsrv)
    import build_nor
    shipped = build_nor.all_flash_order(af)
    derived["nor_images"] = [t for t in build_nor.DEFAULT_ORDER if t in shipped]
    step("nor.bin", [sys.executable, f"{HERE}/build_nor.py", "--identity", ctx.ident_path, "--all-flash", af,
                     "--types", ",".join(derived["nor_images"]), "--out", nor]
         + ([] if derived["wrap_shsh"] else ["--no-wrap-shsh"]))
    blobs, derived["gid_blobs"] = gid_blobs(z, [n for n in z.namelist() if n.startswith(prefix) and n.endswith(".img3")]
                                            + [kc_member], os.path.expanduser(m["keys"]))
    gid = os.path.join(out, "gid-blobs.bin")
    open(gid, "wb").write(blobs)
    # 3.x+ boots its decrypted iBoot directly (the bootrom rejects a personalised LLB); 2.x runs the real
    # bootrom -> NOR LLB -> iBoot chain, so a 2.x device ships no iBoot.bin
    iboot_out = os.path.join(out, "iBoot.bin")
    derived["direct_iboot"] = major >= 3
    if derived["direct_iboot"]:
        shutil.copyfile(os.path.join(dec, "iBoot.bin"), iboot_out)

    # rootfs + kernelcache
    ctx.say("rootfs")
    from ipad1_rootfs import extract_rootfs
    rootfs = os.path.join(work, "rootfs.img")
    extract_rootfs(os.path.join(dec, "rootfs.dmg"), rootfs)
    kc = os.path.join(work, "kernelcache.img3")
    zip_member(z, kc_member, kc)

    # the bake, run by build_nand.py inside the mounted volume
    packages = {k: os.path.expanduser(v["path"]) for k, v in (m.get("packages") or {}).items()}
    for k, v in (m.get("packages") or {}).items():
        if sha(packages[k]) != v["sha256"]:
            raise SystemExit("package %s: %s does not have the pinned sha256" % (k, packages[k]))
    cfg = {"options": opt, "packages": packages, "owners": os.path.join(work, "owners.txt"),
           "report": os.path.join(work, "bake.json")}
    json.dump(cfg, open(os.path.join(work, "bake-config.json"), "w"))
    script = os.path.join(work, "bake.sh")
    open(script, "w").write('exec "%s" "%s" bake "$MNT" "%s"\n' % (sys.executable, os.path.abspath(__file__),
                                                                   os.path.join(work, "bake-config.json")))
    nand = os.path.join(out, "nand")
    step("NAND (%d blocks, epoch %d)" % (m["volume_blocks"], epoch),
         [sys.executable, f"{HERE}/build_nand.py", "--rootfs", rootfs, "--kernelcache", kc,
          "--kernelcache-path", kc_path, "--epoch", str(epoch), "--blocks", str(m["volume_blocks"]),
          "--script", script, "--owners", cfg["owners"], "--workdir", os.path.join(work, "nand-build"),
          "--out", nand])
    baked = json.load(open(cfg["report"]))
    derived.update(baked)

    pages = sorted(os.path.join(d, n) for d in ("cs0", "cs1", "cs2", "cs3") for n in os.listdir(os.path.join(nand, d)))
    listing = hashlib.sha256()
    for p in pages:
        listing.update(("%s %s\n" % (p, sha(os.path.join(nand, p)))).encode())
    tools = {n: sha(os.path.join(ROOT, n)) for n in ("contrib/it-agent/it_agent", "contrib/it-agent/it_typein.dylib",
                                                     "contrib/it-gles/sblaunch", "contrib/it-instprogress/sbdlicon")}
    if baked["gles"] == "shim":
        tools["contrib/it-gles/MBXGLEngine"] = sha(os.path.join(ROOT, "contrib/it-gles/MBXGLEngine"))
    if opt.get("appsync"):
        tools["build/appsync/libappsync.dylib"] = sha(os.path.join(ROOT, "build/appsync/libappsync.dylib"))
    return {
        "ship": [nand, nor, gid] + ([iboot_out] if derived["direct_iboot"] else []),
        "built": tools,
        "inputs": {"rootfs": os.path.join(dec, "rootfs.dmg"), "kernelcache": kc_member,
                   "iboot": os.path.join(dec, "iBoot.bin"), "all_flash": prefix,
                   "packages": {k: {"path": p, "sha256": sha(p)} for k, p in packages.items()},
                   "lockdown": None},
        "outputs": {"nand": {"path": nand, "pages": len(pages), "listing_sha256": listing.hexdigest()},
                    "nor": {"path": nor, "sha256": sha(nor)}, "iboot": {"path": iboot_out, "sha256": sha(iboot_out)} if derived["direct_iboot"] else None,
                    "gid_blobs": {"path": gid, "sha256": sha(gid)}},
        "lock": {"derived": derived},
    }


# --- bake: runs inside the mounted volume --------------------------------------------------------

def install_shell(mnt, packages, ssh, owners):
    """The listed files from the pinned tarballs; with ssh also the host keys, config and launchd job."""
    want = {}
    for line in open(SHELL_LIST):
        tarball, rel = line.split()
        if ssh or tarball == "freeze":
            want.setdefault(tarball, set()).add(rel)
    for tarball, rels in want.items():
        with tarfile.open(packages[tarball]) as t:
            members = {os.path.normpath(i.name): i for i in t.getmembers()}
            for rel in sorted(rels):
                info = members.get(rel)
                if info is None or not (info.isfile() or info.issym() or info.islnk()):
                    raise SystemExit("%s has no file %s" % (packages[tarball], rel))
                dst = os.path.join(mnt, rel)
                parent = os.path.dirname(rel)
                while parent and not os.path.isdir(os.path.join(mnt, parent)):
                    owners.append(("0 0", parent))
                    parent = os.path.dirname(parent)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if os.path.lexists(dst):
                    print("keeping the firmware's own /%s" % rel)   # 2.x ships libncurses itself
                    continue
                if info.issym():
                    os.symlink(info.linkname, dst)
                else:
                    with t.extractfile(info) as s, open(dst, "wb") as d:
                        shutil.copyfileobj(s, d)
                    os.chmod(dst, info.mode & 0o7777)
                owners.append(("0 0", rel))
    if not ssh:
        return
    etc = os.path.join(mnt, "private/etc/ssh")
    os.makedirs(etc)
    owners.append(("0 0", "private/etc/ssh"))
    open(os.path.join(etc, "sshd_config"), "w").write(SSHD_CONFIG)
    owners.append(("0 0", "private/etc/ssh/sshd_config"))
    for kind in ("rsa", "ecdsa"):
        key = os.path.join(etc, "ssh_host_%s_key" % kind)
        subprocess.run(["ssh-keygen", "-q", "-t", kind, "-N", "", "-m", "PEM", "-f", key], check=True)
        os.chmod(key, 0o600)
        owners += [("0 0", "private/etc/ssh/ssh_host_%s_key" % kind), ("0 0", "private/etc/ssh/ssh_host_%s_key.pub" % kind)]
    job = {"Label": "com.openssh.sshd", "ProgramArguments": ["/usr/sbin/sshd", "-D", "-e"], "RunAtLoad": True,
           "KeepAlive": True, "StandardOutPath": "/var/log/sshd.log", "StandardErrorPath": "/var/log/sshd.log"}
    path = os.path.join(mnt, SSHD_JOB)
    if not os.path.isdir(os.path.dirname(path)):
        os.makedirs(os.path.dirname(path))
        owners.append(("0 0", os.path.dirname(SSHD_JOB)))
    plistlib.dump(job, open(path, "wb"))
    owners.append(("0 0", SSHD_JOB))


def bake(mnt, config):
    cfg = json.load(open(config))
    opt, owners, report = cfg["options"], [], {}
    problem = gli_abi_problem(os.path.join(mnt, DYLD_CACHE)) if opt.get("gles_shim", True) else "options.gles_shim off"
    report["gles"] = "shim" if problem is None else "stock engine, software CA: " + problem
    env = dict(os.environ, MNT=mnt, IT_GLES_SHIM="1" if problem is None else "0")
    subprocess.run(["/bin/sh", os.path.join(HERE, "bake-guest-tools.sh")], env=env, check=True)
    owners += [(o, p) for o, p in GUEST_TOOL_OWNERS if os.path.lexists(os.path.join(mnt, p))]
    if opt.get("appsync"):
        env["APPSYNC_DYLIB"] = os.path.join(ROOT, "build/appsync/libappsync.dylib")
        r = subprocess.run(["/bin/sh", os.path.join(ROOT, "contrib/appsync/patch-appsync-dylib.sh")], env=env,
                           check=True, capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        report["appsync"] = [l for l in r.stdout.splitlines() if "MISValidateSignature" in l or "DYLD_INSERT" in l]
        owners.append(("0 0", "usr/lib/libappsync.dylib"))
    if opt.get("shell", True):
        install_shell(mnt, cfg["packages"], opt.get("ssh", True), owners)
        report["shell"] = "ssh" if opt.get("ssh", True) else "shell only"
    with open(cfg["owners"], "w") as f:
        f.writelines("%s %s\n" % (o, p) for o, p in owners)
    json.dump(report, open(cfg["report"], "w"))
    print("bake:", report)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "bake":
        bake(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
