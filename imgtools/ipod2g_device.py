#!/usr/bin/env python3
"""The iPod touch 2G (n72ap) board for imgtools/device.py: create an emulated iPod from a manifest.

    device.py create manifests/ipod2g-7E18.json OUTDIR
    ipod2g_device.py bake MNT CONFIG      (build_nand.py --script runs this inside the mounted volume)

After device.py has verified the IPSW, decrypted it into CACHE and written identity.json, build():

  nor.bin   build_nor.py --identity: IMG2 + SysCfg (Mod#, Regn, SrNm, Batt) + nvram (btaddr, wifiaddr) made
            from the identity, the IPSW's all_flash images packed after it, each SHSH wrapped for the emulated
            UID for 3.x+; on 2.x only iBoot is wrapped (its LLB unwraps it)
  iBoot.bin the IPSW's iBoot, decrypted (the machine's direct-iboot)
  gid-blobs.bin  KBAG || IV-key for each img3 the keys page covers (the machine's gid-blobs: the emulated AES
            engine has no GID key, so it answers a KBAG from this table)
  nand/     build_nand.py over the IPSW rootfs: grown to `volume_blocks`, fstab rw, the IPSW kernelcache
            (still encrypted; the emulated AES engine decrypts it) at the path the decrypted iBoot names,
            the flash bookkeeping generated for the firmware's NAND epoch (Restore.plist SCEP), and bake()

bake() adds the guest side, all of it located at build or run time (no offsets):
  imgtools/bake-guest-tools.sh   it_agent + it_typein (SpringBoard DYLD_INSERT), sblaunch, sbdlicon, sound
                                 defaults, the .lt-guest-tools markers, and the MBXGLEngine shim
                                 contrib/it-gles/MBXGLEngine, one binary for every firmware: it reads the
                                 firmware's __GLIFunctionDispatchRec layout out of OpenGLES at load (2.x has no
                                 engine bundle: the package's OpenGLES hook, the same core under the firmware's
                                 own export names, replaces the framework binary; gles2x_front_end). 4.x caches
                                 MBXGLEngine, so dyld's enable-dylibs-to-override-cache switch is created too
                                 (ipad1_rootfs.gli_uncache)
  contrib/appsync/patch-appsync-dylib.sh  MISValidateSignature -> success in the shared cache, found by symbol;
                                 libappsync.dylib DYLD_INSERTed into installd (options.appsync)
  it_prefs                       build/ipod-guest/it_prefs (contrib/it-prefs/build-ipod.sh) + com.qemu.it-prefs, the
                                 iPad's one-shot: as mobile, through CFPreferences, SBDidShowReorderText = true if
                                 SpringBoard's binary names that key (no first-run "Edit Home Screen" tip)
  install_web_proxy()            the iPad's proxy PAC and the en0 Wi-Fi service that uses it (options.web_proxy)
  mkpkg.seed()                   the guest-package loader (/usr/local/bin/it_boot + its job) and the seed package
                                 from --guest-package (default build/guest-package/armv6.itpack): pkgs/<serial>,
                                 current, state, the kept hook targets + .baked copies (MBXGLEngine only with the
                                 shim); the package's jobs (com.qemu.it-agent) are removed from LaunchDaemons, as
                                 it_boot loads them. The lock's guest_package records it.
No shell, sshd or third-party binary is installed: guest services are stock lockdown services plus it_agent
(docs/archive/guest-services-plan.md). Only our own helpers above are added.
Every file the bake creates is given its owner in the catalog afterwards (the host mount is noowners).
"""
import hashlib, json, os, plistlib, re, shutil, struct, subprocess, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from device import sha
from ipad1_kboot import synth_identity, udid

CACHE = os.path.expanduser("~/Developer/qemu-ios-files/ipod-ipsw/cache")
KC_PREFIX = b"/System/Library/Caches/com.apple.kernelcaches/"
MBX = "System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine"
OPENGLES = "System/Library/Frameworks/OpenGLES.framework/OpenGLES"
DYLD_CACHE = "System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv6"
WEB_PROXY_PAC = "usr/local/share/ltm/proxy.pac"   # ipad1_rootfs.PAC_PATH
GUEST_PACKAGE = os.path.join(ROOT, "build/guest-package/armv6.itpack")
# the iPad's it_prefs, SpringBoard tip only (contrib/it-prefs/build-ipod.sh): built file -> (volume path, mode)
PREFS = {"build/ipod-guest/it_prefs": ("usr/local/bin/it_prefs", 0o755),
         "contrib/it-prefs/com.qemu.it-prefs.plist": ("System/Library/LaunchDaemons/com.qemu.it-prefs.plist", 0o644)}
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
    ("501 501", "private/var/mobile/Media/.lt-guest-tools-v3"),
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


def gles2x_front_end(mnt):
    """1.x/2.x (no engine bundle): (True, line) if the stock OpenGLES exports exactly the names the GL
    front end does (contrib/it-gles/opengles-2x.exports), so the guest package's hook may replace it;
    else (False, why) and the framework stays stock."""
    sys.path.insert(0, os.path.join(ROOT, "contrib/it-gles"))
    import gles2x_exports
    listfile = os.path.join(ROOT, "contrib/it-gles/opengles-2x.exports")
    stock = os.path.join(mnt, OPENGLES)
    if not os.path.exists(stock):
        return False, "no %s" % OPENGLES
    want, got = set(gles2x_exports.read_list(listfile)), set(gles2x_exports.scan(stock))
    if want != got:
        return False, "stock OpenGLES exports differ from opengles-2x.exports (missing %s, extra %s): stock kept" % (
            sorted(want - got)[:4], sorted(got - want)[:4])
    return True, "GL front end replaces OpenGLES (%d exports, the firmware's own)" % len(got)


def gli_engine(cache_path):
    """(True with a sanity line about the firmware's dispatch layout, the shim reads it at load), else
    (False, why): 2.x has no shared cache, and the shim is 3.x+ (the package loader is too)."""
    if not os.path.exists(cache_path):
        return False, "no dyld shared cache (2.x)"
    from ipad1_rootfs import gli_dispatch_info
    return True, gli_dispatch_info(cache_path)


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
    # The baked helpers are linked for the 3.1+ dyld; 3.0's, like 2.x's, refuses LC_DYLD_INFO_ONLY
    # ("dyld: unknown required load command 0x80000022") and SpringBoard never comes up with it_typein inserted.
    guest_tools = tuple(int(x) for x in restore["ProductVersion"].split(".")[:2]) >= (3, 1)
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
    # 2.x boots SecureROM -> LLB -> iBoot and the SecureROM verifies the LLB raw. The epoch-1 chain (iBoot-385.22,
    # 2.1.1) unwraps only iBoot's SHSH (in its LLB) and verifies the rest raw; the epoch-2 chain (385.49, 2.2/2.2.1)
    # unwraps every image it loads ("load_macho_image: failed to load device tree" with a raw DeviceTree).
    wrap = derived["nor_images"] if major >= 3 else \
        [t for t in derived["nor_images"] if t != "illb"] if epoch >= 2 else ["ibot"]
    derived["wrap_shsh_types"] = wrap
    step("nor.bin", [sys.executable, f"{HERE}/build_nor.py", "--identity", ctx.ident_path, "--all-flash", af,
                     "--types", ",".join(derived["nor_images"]), "--out", nor]
         + ([] if derived["wrap_shsh"] else ["--wrap-shsh-types", ",".join(wrap)]))
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
    if m.get("packages") or opt.get("shell") or opt.get("ssh"):
        raise SystemExit("manifest asks for tool packages / a shell / ssh: iPod images carry no shell any more "
                         "(docs/archive/guest-services-plan.md); remove packages and options.shell/ssh")
    if guest_tools and not os.path.exists(os.path.join(ROOT, "build/ipod-guest/it_prefs")):
        raise SystemExit("build/ipod-guest/it_prefs missing (run contrib/it-prefs/build-ipod.sh)")
    cfg = {"options": opt, "guest_tools_supported": guest_tools, "owners": os.path.join(work, "owners.txt"),
           "report": os.path.join(work, "bake.json"), "activation_hook": ctx.hook, "activation_hook_args": ctx.hook_args,
           "guest_package": os.path.abspath(ctx.a.guest_package or GUEST_PACKAGE)}
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
    guest_package = baked.pop("guest_package")
    derived.update(baked)

    # 4.x data protection: effaceable storage (in NOR) + the system keybag (on the volume), made the way a
    # restore makes them, from the IPSW's own restore ramdisk booted as a SecureRoot (ipod2g_keybag.py).
    # Every device made here boots with aes-uid=engine (the one-shot below needs it: the legacy UID path cannot
    # hold a keybag); legacy stays the machine default only for existing images, whose keychains depend on it.
    machine = {"aes-uid": "engine"}
    if opt.get("data_protection"):
        from ipad1_fw import components
        helper = os.path.join(ROOT, "build/ipod-guest/it_keybag")
        if not os.path.exists(helper):
            raise SystemExit("data_protection: build %s first (contrib/it-keybag/build-ipod.sh)" % helper)
        comp = components(z)   # the 4.0 betas ship no Update identity: the restore ramdisk, as firmwarekit's Recipe
        ramdisk = comp.get("UpdateRamDisk", comp["RestoreRamDisk"])[:-4] + "-ramdisk.dmg"
        step("data protection: restore-ramdisk keybag one-shot",
             [sys.executable, f"{HERE}/ipod2g_keybag.py", out, "--dec", dec, "--ramdisk", ramdisk,
              "--helper", helper, "--qemu", os.path.abspath(ctx.a.qemu)])
        derived["keybag_ramdisk"] = ramdisk

    pages = sorted(os.path.join(d, n) for d in ("cs0", "cs1", "cs2", "cs3") for n in os.listdir(os.path.join(nand, d)))
    listing = hashlib.sha256()
    for p in pages:
        listing.update(("%s %s\n" % (p, sha(os.path.join(nand, p)))).encode())
    tools = {n: sha(os.path.join(ROOT, n)) for n in ("contrib/it-agent/it_agent", "contrib/it-agent/it_typein.dylib",
                                                     "contrib/it-gles/sblaunch", "contrib/it-instprogress/sbdlicon")
             + tuple(PREFS)} if cfg["guest_tools_supported"] else {}
    if baked.get("gles_engine") == "MBXGLEngine":     # the OpenGLES hook is the itpack's (lock: guest_package)
        tools["contrib/it-gles/MBXGLEngine"] = sha(os.path.join(ROOT, "contrib/it-gles/MBXGLEngine"))
    if opt.get("appsync"):
        tools["build/appsync/libappsync.dylib"] = sha(os.path.join(ROOT, "build/appsync/libappsync.dylib"))
    if opt.get("data_protection"):
        tools["build/ipod-guest/it_keybag"] = sha(os.path.join(ROOT, "build/ipod-guest/it_keybag"))
    return {
        "ship": [nand, nor, gid] + ([iboot_out] if derived["direct_iboot"] else []),
        "built": tools,
        "inputs": {"rootfs": os.path.join(dec, "rootfs.dmg"), "kernelcache": kc_member,
                   "iboot": os.path.join(dec, "iBoot.bin"), "all_flash": prefix,
                   "lockdown": None},
        "outputs": {"nand": {"path": nand, "pages": len(pages), "listing_sha256": listing.hexdigest()},
                    "nor": {"path": nor, "sha256": sha(nor)}, "iboot": {"path": iboot_out, "sha256": sha(iboot_out)} if derived["direct_iboot"] else None,
                    "gid_blobs": {"path": gid, "sha256": sha(gid)}},
        # machine options the device must boot with (regress.py --device applies them)
        "lock": {"derived": derived, "guest_package": guest_package, "machine": machine},
    }


# --- bake: runs inside the mounted volume --------------------------------------------------------

def install_web_proxy(mnt, owners):
    """The iPad's proxy routing (imgtools/ipad1_rootfs.py): the PAC file, and configd's preferences with the
    en0 Wi-Fi service pointing at it (the app's web proxy on the 10.0.2.100:3128 guestfwd; private IPs DIRECT).
    Nothing runs in the guest to set it, and proxy on/off is the host proxy's mode."""
    from ipad1_rootfs import PAC, PAC_PATH, SC_DIR, seed_plist, wifi_proxy_prefs
    for rel in ("usr/local", "usr/local/share", "usr/local/share/ltm", "private/var/" + SC_DIR):
        if not os.path.isdir(os.path.join(mnt, rel)):
            os.makedirs(os.path.join(mnt, rel))
            owners.append(("0 0", rel))
    with open(os.path.join(mnt, PAC_PATH), "w") as f:
        f.write(PAC)
    prefs = "private/var/%s/preferences.plist" % SC_DIR
    seed_plist(os.path.join(mnt, prefs), wifi_proxy_prefs)
    owners += [("0 0", PAC_PATH), ("0 0", prefs)]


def bake(mnt, config):
    cfg = json.load(open(config))
    opt, owners, report = cfg["options"], [], {}
    gli, info = gli_engine(os.path.join(mnt, DYLD_CACHE)) if opt.get("gles_shim", True) else (False, "options.gles_shim off")
    engine = os.path.join(ROOT, "contrib/it-gles/MBXGLEngine")
    if gli and not os.path.exists(engine):
        raise SystemExit("%s missing (run contrib/it-gles/build.sh)" % engine)
    report["gles"] = "shim MBXGLEngine (%s)" % info if gli else "stock engine, software CA: " + info
    # 1.x/2.x: no engine to replace; the package's OpenGLES hook (the same core) goes in instead
    front = False
    if not gli and opt.get("gles_shim", True) and not os.path.exists(os.path.join(mnt, DYLD_CACHE)):
        front, why = gles2x_front_end(mnt)
        report["gles"] = why + ("; CA composites through it (CA_ENABLE_OGL=1)" if front else "")
    report["gles_shim"] = bool(gli or front)
    report["gles_engine"] = "MBXGLEngine" if gli else "OpenGLES" if front else None
    supported = cfg["guest_tools_supported"]
    report["guest_tools"] = "installed" if supported else "omitted: current helpers require the iOS 3.1+ dyld"
    env = dict(os.environ, MNT=mnt, IT_GLES_SHIM="1" if gli else "0", IT_CA_OGL="1" if gli or front else "0",
               IT_GLES_ENGINE=engine,
               IT_GUEST_TOOLS="1" if supported else "0")
    subprocess.run(["/bin/sh", os.path.join(HERE, "bake-guest-tools.sh")], env=env, check=True)
    if gli:
        from ipad1_rootfs import gli_uncache, DYLD_OVERRIDE
        report["gles_cache"] = gli_uncache(mnt, MBX, DYLD_CACHE)
        if "overridden" in report["gles_cache"]:
            owners.append(("0 0", DYLD_OVERRIDE))
    if opt.get("appsync"):
        env["APPSYNC_DYLIB"] = os.path.join(ROOT, "build/appsync/libappsync.dylib")
        r = subprocess.run(["/bin/sh", os.path.join(ROOT, "contrib/appsync/patch-appsync-dylib.sh")], env=env,
                           check=True, capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        report["appsync"] = [l for l in r.stdout.splitlines() if "MISValidateSignature" in l or "DYLD_INSERT" in l]
        owners.append(("0 0", "usr/lib/libappsync.dylib"))
    if supported:
        for src, (rel, mode) in PREFS.items():
            shutil.copyfile(os.path.join(ROOT, src), os.path.join(mnt, rel))
            os.chmod(os.path.join(mnt, rel), mode)
            owners.append(("0 0", rel))
        report["prefs"] = "it_prefs: SBDidShowReorderText at first boot"
    if opt.get("web_proxy", True):
        install_web_proxy(mnt, owners)
        report["web_proxy"] = "PAC /%s on the en0 Wi-Fi service" % WEB_PROXY_PAC
    if cfg.get("activation_hook"):
        from ipad1_rootfs import activation_hook, LOCKDOWND
        activation_hook(cfg["activation_hook"], os.path.join(mnt, LOCKDOWND), cfg["activation_hook_args"])
        owners.append(("0 0", LOCKDOWND))
        report["activation"] = "activation hook applied and daemon re-signed"
    sys.path.insert(0, os.path.join(ROOT, "contrib/guest-package"))
    import mkpkg
    # the loader (it_boot) is linked for the 3.1+ dyld like the other helpers; on 2.x the package carries
    # only the OpenGLES front-end hook (FirmwareKit's N72Board bakes the same)
    seeded, report["guest_package"] = mkpkg.seed(mnt, cfg["guest_package"], gli or front) if (supported or front) else ([], None)
    if front and "/" + OPENGLES not in report["guest_package"]["hooks"]:
        # CA_ENABLE_OGL=1 over the stock driver drives the unemulated MBX: fail rather than wedge
        raise SystemExit("%s has no OpenGLES hook for this build; rebuild contrib/guest-package" % cfg["guest_package"])
    owners += [("0 0", p) for p in seeded]
    owners += [(o, p) for o, p in GUEST_TOOL_OWNERS if os.path.lexists(os.path.join(mnt, p))]
    with open(cfg["owners"], "w") as f:
        f.writelines("%s %s\n" % (o, p) for o, p in owners)
    json.dump(report, open(cfg["report"], "w"))
    print("bake:", report)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "bake":
        bake(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
