#!/usr/bin/env python3
"""Userland images for the ipad1 machine: a patched copy of the 7B500 system partition plus a seeded data volume.

    ipad1_rootfs.py build [--base pristine|jailbroken] [--out DIR] [--data-size partition|SIZE] [--rootfs IMG]
                          [--stash DIR|none] [--lockdown DIR|none] [--disable LABEL]... [--ro-root] [--no-web-proxy] [--no-usb-net]
    ipad1_rootfs.py fetch [DIR]          copy /var/stash and /var/root/Library/Lockdown off the real iPad (ssh)
    ipad1_rootfs.py report DIR...        list the Mach-Os under DIR that carry no Apple signature
    ipad1_rootfs.py bake DIR [--tools build/ipad1-guest] [--guest-package ITPACK] [--seal] [--activation-hook SCRIPT]
                                         install the guest helpers into DIR/system.img
    ipad1_rootfs.py --selfcheck

`build` writes DIR/<base>/{system.img,data.img,unsigned-machos.txt}, then prints the ipad1_nand.py line:

    ipad1_nand.py build --mbr MBR --kernelcache KERNELCACHE --system DIR/<base>/system.img --data DIR/<base>/data.img \
                        --out DIR/nand-{pristine|jb}

Two bases. `pristine` (default, first boot target): the IPSW rootfs.dmg, sliced out of its UDIF/APM and
grown to partition 1 (1280 MiB from the MBR); nothing on it is jailbroken, so no AMFI boot-args are needed.
`jailbroken`: the captured 3.2.2 system partition (hw2/rdisk0s1-system.img, already partition-sized), which
stashes /Applications, /usr/libexec, /usr/share, /usr/include, /usr/lib/pam, /Library/{Ringtones,Wallpaper}
into /private/var/stash, so its data volume carries the real unit's /var/stash (fetch) or launchd finds no
lockdownd, installd, ...; its OpenSSH, bash, Cydia, Substrate are ldid-signed and need
amfi_allow_any_signature=1.

system.img edits, all through one read-write mount, no Mach-O touched:
  /private/etc/fstab                         "/dev/disk0s1 / rw" + "/dev/disk0s2 /private/var" (plain 0xAF
                                             data partition, see ipad1_nand.py; rw root is insurance for a
                                             failed data mount, --ro-root keeps the stock ro)
  .../LaunchDaemons/com.apple.SpringBoard.plist  CA_ENABLE_OGL=0, MBX2D_PAGE_FLIP=0 (userland-gl-display.md
                                             §1.3/§1.5), stdout/stderr -> /dev/console (crw--w--w- on the
                                             unit, so SpringBoard's stderr rides the serial console)
  --disable LABEL                            Disabled=true, looked up in /System/Library and /Library
The jailbreak's sshd job (/Library/LaunchDaemons/com.openssh.sshd.plist, inetd-style on port 22, host keys
in /etc/ssh, wrapper in the stash) is kept as is and reported; it becomes useful once USB/network exists.

data.img also gets /preferences/SystemConfiguration/{NetworkInterfaces,preferences}.plist (skip with
--no-usb-net): en1 pinned to the AppleUSBEthernetDevice path, as on the unit, with a DHCP service first in
the service order, so configd brings USB Ethernet up against usbmuxd's slirp (10.0.2.0/24).

data.img = fresh journaled HFSX "Data" seeded like mobile_obliterator does (the system volume's own
/private/var skeleton), plus /stash and /root/Library/Lockdown (activation record, device keys, pair
records) from `fetch`. Owners are the source rootfs's own (/var/Keychains is _securityd's), patched into
the catalog offline because the host mount is noowners; see var_owners().

unsigned-machos.txt: every Mach-O on the system volume and in the stash whose code signature has no CMS
blob (ldid ad-hoc: sshd, bash, apt, Cydia, Substrate) or none at all. Those are what
`amfi_allow_any_signature=1` has to forgive at exec; Apple's own binaries carry a (possibly empty) CMS slot.

`bake` installs this machine's guest helpers (docs/ipad1/guest-services.md): it_pbd, it_ethlink, it_prefs,
it_msmquiet, root-owned via the catalog, and the guest-package loader + seed package (contrib/guest-package
mkpkg.seed, from the armv7.itpack contrib/guest-package/build.sh makes): /usr/local/bin/it_boot and its job,
/usr/local/lighttouch/{pkgs/<serial>,current,state}, the hook targets the package keeps (the GL shim only if
this image has it) with their .baked copies. The helpers' launchd jobs come from the package, loaded by it_boot,
so none is baked into LaunchDaemons. DIR/guest-package.json records the seed for the lock. Nothing else on
either volume changes. Build the tools first with contrib/ipad1-guest/build.sh; they are ldid ad-hoc signed,
so boot with amfi_allow_any_signature=1.
"""
import argparse
import json
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_nand as bn                      # attach(), set_owner(), run(), JUNK
from ipad1_nand import make_hfs_image, mbr_parts, parse_size
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../contrib/guest-package"))
import mkpkg                                 # seed(): the loader + seed package

FILES = os.path.expanduser("~/Developer/qemu-ios-files/ipad1")
IPAD_SSH = os.path.join(FILES, "ipad-ssh.sh")
BLOCK = 4096
FSTAB = "/dev/disk0s1 / hfs rw 0 1\n/dev/disk0s2 /private/var hfs rw,nosuid,nodev 0 2\n"
FSTAB_RO = FSTAB.replace("/ hfs rw", "/ hfs ro", 1)
DAEMON_DIRS = ("System/Library/LaunchDaemons", "Library/LaunchDaemons")
SB_JOB = "System/Library/LaunchDaemons/com.apple.SpringBoard.plist"
SB_ENV = {"CA_ENABLE_OGL": "0", "MBX2D_PAGE_FLIP": "0"}
SSHD = ("Library/LaunchDaemons/com.openssh.sshd.plist", "usr/sbin/sshd", "private/etc/ssh/ssh_host_rsa_key")
# base -> (system volume under FILES, /var/stash seed under FILES or None, store name suffix)
BASES = {"pristine": ("7B500/dec/rootfs.dmg", None, "pristine"),
         "jailbroken": ("hw2/rdisk0s1-system.img", "hw2/stash", "jb")}
GLES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../contrib/gles-public")
# The GL front end (contrib/gles-public: one fat OpenGLES for every firmware) replaces the framework binary whole;
# nothing under it (GLEngine, libGFXShared, a gld plugin) is touched. FirmwareKit's FitCheck.glesFrontEnd proves the
# firmware has what it looks up at run time; this oracle installs it the same way.
OPENGLES_REL = "System/Library/Frameworks/OpenGLES.framework/OpenGLES"
GLES_APPS = ("GLTest.app", "GLTest2.app")            # contrib/gles-public/build-apps.sh
# GL CoreAnimation (the default; --no-ca-ogl opts out): stock CoreAnimation composites through the front end
SB_ENV_CA_OGL = {"MBX2D_PAGE_FLIP": "0"}
# The front end reads the firmware's __GLIFunctionDispatchRec layout (5.x's macro context) by name against the
# table it and the host share (include/hw/arm/guest-services/gles-names.h); gli_dispatch_info says what it finds.
GLI_NAMES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../include/hw/arm/guest-services/gles-names.h")
DYLD_CACHE = "System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv7"


def gli_engine(cache_path):
    """(the GL front end, a sanity line about this shared cache's dispatch layout); the second is a warning when
    the cache carries no layout, never a refusal."""
    return os.path.join(GLES, "OpenGLES"), gli_dispatch_info(cache_path)


DYLD_OVERRIDE = "System/Library/Caches/com.apple.dyld/enable-dylibs-to-override-cache"


def cache_images(data):
    """{path: (mach_header file offset, va->file offset)} for a dyld_v1 shared cache."""
    if data[:7] != b"dyld_v1":
        return {}
    moff, mcount, ioff, icount = struct.unpack_from("<4I", data, 0x10)
    maps = [struct.unpack_from("<QQQ", data, moff + 32 * i) for i in range(mcount)]
    f = lambda va: next(fo + va - a for a, sz, fo in maps if a <= va < a + sz)
    out = {}
    for i in range(icount):
        va, _, _, p = struct.unpack_from("<QQQI", data, ioff + 32 * i)
        out[data[p:data.index(b"\0", p)].decode()] = (f(va), f)
    return out


def image_strings(data, img, section=None):
    """The cached image's symbol names, or with section (e.g. "__cstring") that section's C strings."""
    mh, f = img
    ncmds, off, out = struct.unpack_from("<I", data, mh + 16)[0], mh + 28, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        if cmd == 2 and not section:                          # LC_SYMTAB: cache file offsets
            symoff, nsyms, stroff = struct.unpack_from("<III", data, off + 8)
            for k in range(nsyms):
                x = stroff + struct.unpack_from("<I", data, symoff + 12 * k)[0]
                out.append(data[x:data.index(b"\0", x)].decode("latin1"))
        if cmd == 1 and section:                              # LC_SEGMENT
            for k in range(struct.unpack_from("<I", data, off + 48)[0]):
                so = off + 56 + 68 * k
                if data[so:so + 16].rstrip(b"\0").decode() == section:
                    addr, sz = struct.unpack_from("<II", data, so + 32)
                    out += [x.decode("latin1") for x in data[f(addr):f(addr) + sz].split(b"\0") if x]
        off += size
    return out


def gli_uncache(mnt, rel=OPENGLES_REL, cache=DYLD_CACHE):
    """Let dlopen reach the GLI shim (or the iPod's MBX shim, rel/cache given) on disk. 4.x ships GLEngine inside the shared cache, and iOS dyld
    matches a cached image by path alone, so the shim installed over it would never load (3.2.x has no
    cached GLEngine: nothing to do). dyld's own switch fixes that: when
    /System/Library/Caches/com.apple.dyld/enable-dylibs-to-override-cache exists, loadPhase5 tries the
    file on disk before the cache (sDylibsOverrideCache), so the installed shim wins and the other cached
    images, which have no file on disk, still come from the cache. The cache itself is not edited. Fails
    closed if this dyld has no such switch. Returns a one-line status."""
    data = open(os.path.join(mnt, cache), "rb").read()
    if "/" + rel not in cache_images(data):
        return "no cached %s" % os.path.basename(rel)
    if b"/" + DYLD_OVERRIDE.encode() + b"\0" not in open(os.path.join(mnt, "usr/lib/dyld"), "rb").read():
        raise SystemExit("%s is in the shared cache and this dyld has no %s switch: the GL shim "
                         "cannot load (build without GL)" % (os.path.basename(rel), os.path.basename(DYLD_OVERRIDE)))
    d = os.path.dirname(os.path.join(mnt, DYLD_OVERRIDE))
    mode = os.stat(d).st_mode
    os.chmod(d, mode | 0o200)                  # the stock directory is r-x
    open(os.path.join(mnt, DYLD_OVERRIDE), "wb").close()
    os.chmod(d, mode)
    return "cached %s overridden by the file (%s)" % (os.path.basename(rel), os.path.basename(DYLD_OVERRIDE))


def gli_fields(data):
    """The dispatch fields of the first __GLIFunctionDispatchRec @encode in data (a shared cache or OpenGLES), or None."""
    enc = re.search(rb"\{__GLIFunctionDispatchRec=[^}]*\}", data)
    return [f.decode() for f in re.findall(rb'"([^"]+)"', enc[0])] if enc else None


def gli_dispatch_info(cache_path, data=None, names=GLI_NAMES):
    """What the shim will discover at load: 'GLI dispatch: N slots, K unknown to the name table (...)', or a
    warning that the cache carries no @encode (the shim then decodes OpenGLES's trampolines instead)."""
    have = gli_fields(data if data is not None else open(cache_path, "rb").read())
    if have is None:
        return "no __GLIFunctionDispatchRec @encode in %s: the shim will read OpenGLES's trampolines" % cache_path
    known = set(re.findall(r"^GLES_FN\(\w+,\s*(\w+),", open(names).read(), re.M))
    unknown = [f for f in have if f not in known]
    return "GLI dispatch: %d slots, %d unknown to the name table%s" % (len(have), len(unknown),
                                                                      " (%s)" % ", ".join(unknown[:8]) if unknown else "")
# AppSync: one dylib injected into installd (install gate) and SpringBoard (launch gate)
# via DYLD_INSERT_LIBRARIES. See contrib/appsync. Requires the AMFI boot-args (it is ldid-signed).
APPSYNC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../build/appsync")
APPSYNC_REL = "usr/lib/libappsync.dylib"
APPSYNC_JOBS = ("System/Library/LaunchDaemons/com.apple.mobile.installd.plist",)
# USB Ethernet (AppleUSBEthernetDevice, usbmuxd's slirp on the host side). Names and paths are the real
# unit's NetworkInterfaces.plist: Wi-Fi keeps en0 even with no BCM4329 model, so USB is en1 as on hardware.
SC_DIR = "preferences/SystemConfiguration"   # under /private/var (/Library/Preferences links here)
USB_ETH_SERVICE, NET_SET = "4C54E7A1-0B5E-4D6B-9A1C-5553424E4554", "4C54E7A1-0B5E-4D6B-9A1C-534554000001"
USB_ETH_IF = {"Active": True, "BSD Name": "en1", "IOBuiltin": False, "IOInterfaceType": 6, "IOInterfaceUnit": 1,
              "IOMACAddress": bytes.fromhex("0a0bad0babe0"), "SCNetworkInterfaceType": "Ethernet",
              "IOPathMatch": "IOService:/AppleARMPE/arm-io@BFC00000/AppleS5L8930XIO/usb-complex@3F108000/"
                             "AppleS5L8930XUSBArbitrator/usb-device/AppleSynopsysOTGDevice/IOUSBDeviceInterface@5/"
                             "AppleUSBEthernetDevice/IOEthernetInterface"}
MOBILE_TOP = ("mobile", "ea")                # uid 501 on the real unit; everything else under /var is root
# guest tool -> (install path on the system volume, mode); the job comes from contrib/it-pasteboard
TOOLS = {"it_pbd": ("usr/local/bin/it_pbd", 0o755), "it_ethlink": ("usr/local/bin/it_ethlink", 0o755),
         "it_prefs": ("usr/local/bin/it_prefs", 0o755),
         "it_msmquiet.dylib": ("usr/local/lib/it_msmquiet.dylib", 0o755)}
# Apple job that loads it_msmquiet (hides the USB "not supported" notice; contrib/it-msmquiet)
MSM_JOB = "System/Library/LaunchDaemons/com.apple.mobile.storage_mounter.plist"
# launchd job, installed path -> source under contrib/. The helpers' own jobs (it-pbd, it-ethlink,
# it-prefs) are the seed package's, loaded by it_boot; only the one-shots below are baked.
JOBS = {}
GUEST_PACKAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../build/guest-package/armv7.itpack")
# earlier helpers' files, removed when an image is baked again (it_notip became it_prefs)
RETIRED = ["usr/local/bin/it_notip", "System/Library/LaunchDaemons/com.qemu.it-notip.plist"]
# Bluetooth has no controller model (UART3 is silent), so BTServer's retries left
# BluetoothManager's blocking calls on SpringBoard's main thread: a ~1 s UI stall
# every ~12 s. The job's own Disabled key (in place, Apple's owner kept) keeps it
# unloaded; no binary changes. `bake --keep-bluetooth` leaves it on.
BT_JOB = "System/Library/LaunchDaemons/com.apple.BTServer.plist"
SEAL_TOOL = {"it_seal": ("usr/local/bin/it_seal", 0o755)}
SEAL_JOB = {"System/Library/LaunchDaemons/com.qemu.it-seal.plist": "it-seal/com.qemu.it-seal.plist"}
GLTEST_TOOL = {"it_gltest": ("usr/local/bin/it_gltest", 0o755)}   # tests/ipad1/gltest.py's fixture
GLTEST_JOB = {"System/Library/LaunchDaemons/com.qemu.it-gltest.plist": "it-gltest/com.qemu.it-gltest.plist"}
LC_MAIN, LC_VERSION_MIN_IPHONEOS = 0x80000028, 0x25
MH_MAGIC, FAT_MAGIC, LC_CODE_SIGNATURE, CS_CMS = 0xFEEDFACE, 0xCAFEBABE, 0x1D, 0x10000


# --- pure helpers (covered by --selfcheck) ------------------------------------------------------

def apm_hfs_slice(raw):
    """(byte offset, byte length) of the HFS partition in an Apple Partition Map."""
    assert raw[:2] == b"ER", "not an Apple partition map"
    bs = struct.unpack_from(">H", raw, 2)[0]
    n = struct.unpack_from(">I", raw, bs + 4)[0]
    for i in range(1, n + 1):
        e = raw[bs * i:bs * (i + 1)]
        start, count = struct.unpack_from(">II", e, 8)
        if e[48:80].split(b"\0")[0] in (b"Apple_HFSX", b"Apple_HFS"):
            return start * bs, count * bs
    raise ValueError("no Apple_HFS(X) partition")


def edit_plist(data, fn):
    """Apply fn(dict) to a plist, keeping its binary/XML flavour."""
    d = plistlib.loads(data)
    fn(d)
    return plistlib.dumps(d, fmt=plistlib.FMT_BINARY if data.startswith(b"bplist") else plistlib.FMT_XML)


def rewrite_plist(path, fn):
    """Read fully before opening for write: open(path, "wb") truncates the file first."""
    with open(path, "rb") as f:
        new = edit_plist(f.read(), fn)
    with open(path, "wb") as f:      # in place, so the catalog record (and Apple's uid 0) survives
        f.write(new)


def springboard_env(d, env=SB_ENV):
    assert d.get("Label") == "com.apple.SpringBoard"
    d.setdefault("EnvironmentVariables", {}).update(env)
    d["StandardOutPath"] = d["StandardErrorPath"] = "/dev/console"


def msm_insert(d):
    assert d.get("Label") == "com.apple.mobile.storage_mounter"
    d.setdefault("EnvironmentVariables", {})["DYLD_INSERT_LIBRARIES"] = "/" + TOOLS["it_msmquiet.dylib"][0]


def usb_net_interfaces(d):
    """NetworkInterfaces.plist: pin the USB Ethernet interface to en1."""
    ifs = [i for i in d.setdefault("Interfaces", []) if i.get("IOPathMatch") != USB_ETH_IF["IOPathMatch"]]
    d["Interfaces"] = sorted(ifs + [dict(USB_ETH_IF)], key=lambda i: i["IOInterfaceUnit"])


def usb_net_prefs(d):
    """preferences.plist: a DHCP service on en1, first in the current set's service order. configd only
    brings up interfaces that have a service, and 3.2 creates none for a non-builtin Ethernet by itself."""
    d.setdefault("NetworkServices", {})[USB_ETH_SERVICE] = {
        "Interface": {"DeviceName": "en1", "Hardware": "Ethernet", "Type": "Ethernet", "UserDefinedName": "USB Ethernet"},
        "IPv4": {"ConfigMethod": "DHCP"}, "DNS": {}, "UserDefinedName": "USB Ethernet"}
    cur = d.setdefault("CurrentSet", "/Sets/" + NET_SET).rsplit("/", 1)[1]
    net = d.setdefault("Sets", {}).setdefault(cur, {"UserDefinedName": "Automatic"}).setdefault("Network", {})
    net.setdefault("Service", {})[USB_ETH_SERVICE] = {"__LINK__": "/NetworkServices/" + USB_ETH_SERVICE}
    order = net.setdefault("Global", {}).setdefault("IPv4", {}).setdefault("ServiceOrder", [])
    order[:] = [USB_ETH_SERVICE] + [o for o in order if o != USB_ETH_SERVICE]


def dyld_insert(d, lib=("/" + APPSYNC_REL)):
    """Append lib to DYLD_INSERT_LIBRARIES, keeping any existing entries (e.g. SpringBoard's env)."""
    env = d.setdefault("EnvironmentVariables", {})
    libs = [x for x in env.get("DYLD_INSERT_LIBRARIES", "").split(":") if x]
    if lib not in libs:
        libs.append(lib)
    env["DYLD_INSERT_LIBRARIES"] = ":".join(libs)


# Web proxy (the app's, on slirp guestfwd 10.0.2.100:3128, as on the iPod). The Wi-Fi service
# carries a PAC. 3.2.2's Safari does NOT honour the "; DIRECT" fallback: with no guestfwd at .100 a
# proxied URL just fails, so plain host names and private/link-local/loopback IP literals go DIRECT
# (slirp reaches the LAN itself). IP literals are tested first because isInNet on a host name does a
# DNS lookup.
WIFI_SERVICE = "4C54E7A1-0B5E-4D6B-9A1C-574946490000"
PAC_PATH = "usr/local/share/ltm/proxy.pac"
PAC = """function FindProxyForURL(url, host) {
    if (isPlainHostName(host)) return "DIRECT";
    if (/^\\d+\\.\\d+\\.\\d+\\.\\d+$/.test(host) &&
        (isInNet(host, "10.0.0.0", "255.0.0.0") || isInNet(host, "172.16.0.0", "255.240.0.0") ||
         isInNet(host, "192.168.0.0", "255.255.0.0") || isInNet(host, "169.254.0.0", "255.255.0.0") ||
         isInNet(host, "127.0.0.0", "255.0.0.0"))) return "DIRECT";
    return "PROXY 10.0.2.100:3128; DIRECT";
}
"""


def wifi_proxy_prefs(d):
    """preferences.plist: the AirPort service on en0 (the unit's own shape) with the proxy PAC."""
    svc = d.setdefault("NetworkServices", {}).setdefault(WIFI_SERVICE, {
        "Interface": {"DeviceName": "en0", "Hardware": "AirPort", "Type": "Ethernet", "UserDefinedName": "AirPort"},
        "IPv4": {"ConfigMethod": "DHCP"}, "IPv6": {"ConfigMethod": "Automatic"}, "DNS": {}, "UserDefinedName": "AirPort"})
    svc["Proxies"] = {"ExceptionsList": ["*.local", "169.254/16"], "FTPPassive": 1,
                      "ProxyAutoConfigEnable": 1, "ProxyAutoConfigURLString": "file:///" + PAC_PATH}
    cur = d.setdefault("CurrentSet", "/Sets/" + NET_SET).rsplit("/", 1)[1]
    net = d.setdefault("Sets", {}).setdefault(cur, {"UserDefinedName": "Automatic"}).setdefault("Network", {})
    net.setdefault("Service", {})[WIFI_SERVICE] = {"__LINK__": "/NetworkServices/" + WIFI_SERVICE}
    net.setdefault("Interface", {}).setdefault("en0", {"AirPort": {"JoinMode": "Automatic"}})
    order = net.setdefault("Global", {}).setdefault("IPv4", {}).setdefault("ServiceOrder", [])
    order[:] = [WIFI_SERVICE] + [o for o in order if o != WIFI_SERVICE]


def seed_plist(path, fn):
    """Edit the plist at path in place, or create it (XML, as configd writes) from an empty dict."""
    if os.path.exists(path):
        return rewrite_plist(path, fn)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(edit_plist(plistlib.dumps({}), fn))


def appsync_problem(path):
    """None if path is a fat Mach-O with an armv7 slice and a code signature per slice, else why not."""
    if not os.path.exists(path):
        return "missing"
    data = open(path, "rb").read()
    if len(data) < 8 or struct.unpack_from(">I", data)[0] != FAT_MAGIC:
        return "not a fat Mach-O (expected armv6+armv7)"
    nfat = struct.unpack_from(">I", data, 4)[0]
    have_v7 = False
    for i in range(nfat):
        cputype, sub, off, size, _ = struct.unpack_from(">iiIII", data, 8 + i * 20)
        if (cputype, sub) == (12, 9):
            have_v7 = True
        slice_cmds, o = set(), off + 28
        ncmds = struct.unpack_from("<I", data, off + 16)[0]
        for _ in range(ncmds):
            cmd, sz = struct.unpack_from("<II", data, o)
            slice_cmds.add(cmd)
            o += sz
        if LC_CODE_SIGNATURE not in slice_cmds:
            return "slice %d (cpu %d/%d) is not ldid-signed" % (i, cputype, sub)
    return None if have_v7 else "no armv7 slice"


def guest_tool_problem(data):
    """None if data is a thin armv7 Mach-O 3.2 dyld will take (no LC_MAIN, no LC_VERSION_MIN, signed), else why not."""
    if len(data) < 28 or struct.unpack_from("<I", data)[0] != MH_MAGIC:
        return "not a thin 32-bit Mach-O"
    cputype, sub, _, ncmds = struct.unpack_from("<iiII", data, 4)
    if (cputype, sub) != (12, 9):
        return "cpu %d/%d, not armv7" % (cputype, sub)
    cmds, off = set(), 28
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        cmds.add(cmd)
        off += size
    if cmds & {LC_MAIN, LC_VERSION_MIN_IPHONEOS}:
        return "carries LC_MAIN/LC_VERSION_MIN (not run through mkold.py)"
    if LC_CODE_SIGNATURE not in cmds:
        return "unsigned (ldid -S)"
    return None


def owner_for(relpath):
    return (501, 501) if relpath.split("/", 1)[0] in MOBILE_TOP else (0, 0)


def hfs_info(img):
    """(signature, allocation block size, total blocks, free blocks); both 7B500 volumes use 8 KiB blocks."""
    with open(img, "rb") as f:
        f.seek(1024)
        vh = f.read(512)
    sig = vh[:2]
    if sig not in (b"H+", b"HX"):
        raise SystemExit("%s: no HFS+ volume header (got %r)" % (img, sig))
    return (sig.decode(),) + struct.unpack_from(">III", vh, 40)


def signature_kind(data):
    """None (not a Mach-O), 'none', 'adhoc' or 'apple'.

    Apple's 7B500 binaries all carry a CMS slot (0x10000) in the SuperBlob, even when it is the 8-byte empty
    one (launchd, mediaserverd); ldid -S output has CodeDirectory + empty requirements and no CMS slot.
    That is the distinction AMFI_vnode_check_signature ends up making, so it is the one reported here.
    """
    if len(data) < 32:
        return None
    if struct.unpack_from(">I", data, 0)[0] == FAT_MAGIC:              # first arm slice of a fat file
        for i in range(struct.unpack_from(">I", data, 4)[0]):
            cpu, _, off, size = struct.unpack_from(">IIII", data, 8 + 20 * i)
            if cpu == 12:
                return signature_kind(data[off:off + size])
        return None
    if struct.unpack_from("<I", data, 0)[0] != MH_MAGIC:
        return None
    ncmds, off = struct.unpack_from("<I", data, 16)[0], 28
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        if cmd == LC_CODE_SIGNATURE:
            so, sn = struct.unpack_from("<II", data, off + 8)
            blob = data[so:so + sn]
            if len(blob) < 12 or struct.unpack_from(">I", blob, 0)[0] != 0xFADE0CC0:
                return "none"
            slots = [struct.unpack_from(">I", blob, 12 + 8 * i)[0] for i in range(struct.unpack_from(">I", blob, 8)[0])]
            return "apple" if CS_CMS in slots else "adhoc"
        off += size
    return "none"


# --- host plumbing ------------------------------------------------------------------------------

def extract_rootfs(src, out):
    """Raw HFS volume from an IPSW rootfs DMG (UDIF+APM), or a plain copy if src already is one."""
    with open(src, "rb") as f:
        f.seek(1024)
        if f.read(2) in (b"H+", b"HX"):
            print("      %s is a bare HFS volume; copying" % os.path.basename(src))
            shutil.copyfile(src, out)
            return
    work = tempfile.mkdtemp(prefix="ipad1_rootfs.")
    try:
        bn.run(["hdiutil", "convert", src, "-format", "UDTO", "-o", os.path.join(work, "raw")])
        with open(os.path.join(work, "raw.cdr"), "rb") as f, open(out, "wb") as o:
            off, ln = apm_hfs_slice(f.read(64 * 512))
            f.seek(off)
            while ln:
                chunk = f.read(min(ln, 1 << 24))
                o.write(chunk)
                ln -= len(chunk)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def grow_to_partition(img, blocks):
    """Grow the volume to `blocks` x 4 KiB. hdiutil grows the backing file itself (see build_nand.resize)
    but with 8 KiB HFS blocks it stops one 4 KiB sector short, so pad the file and move the alternate
    volume header to the new end - 1024, where fsck_hfs and the kernel look for it."""
    want = blocks * BLOCK
    if os.path.getsize(img) == want:
        return
    bn.run(["hdiutil", "resize", "-sectors", str(want // 512), "-imagekey", "diskimage-class=CRawDiskImage", img])
    old = os.path.getsize(img)
    if old > want:
        raise SystemExit("resize overshot partition 1")
    with open(img, "r+b") as f:
        f.seek(old - 1024)
        avh = f.read(512)
        f.truncate(want)
        f.seek(want - 1024)
        f.write(avh)


def var_owners(img):
    """{path relative to /private/var: (uid, gid)} as the image's own catalog records them.

    The data volume is seeded through a noowners mount, so every entry lands as the host uid and has to
    be put back offline. Taking the owners from the source skeleton rather than a rule matters: securityd
    runs as _securityd (64) and cannot create its keychain and trust store in a root-owned /var/Keychains,
    which broke every keychain user (SecItemAdd -25291, profile root certificates, Mail/Wi-Fi passwords).
    """
    mnt = tempfile.mkdtemp(prefix="ipad1_owners.")
    r = subprocess.run(["hdiutil", "attach", "-readonly", "-owners", "on", "-nobrowse", "-mountpoint", mnt, img],
                       capture_output=True, text=True, check=True)
    dev = r.stdout.split()[0]
    try:
        top, out = os.path.join(mnt, "private/var"), {}
        for root, dnames, fnames in os.walk(top):
            for n in dnames + fnames:
                st = os.lstat(os.path.join(root, n))
                out[os.path.relpath(os.path.join(root, n), top)] = (st.st_uid, st.st_gid)
        return out
    finally:
        subprocess.run(["hdiutil", "detach", dev], capture_output=True)
        os.rmdir(mnt)


class Mounted:
    """attach a raw HFS image and mount it read-write at `mnt` (diskutil, no sudo: see editimg.py)."""

    def __init__(self, img, mnt):
        self.img, self.mnt, self.ok = img, mnt, False

    def __enter__(self):
        os.makedirs(self.mnt, exist_ok=True)
        self.dev = bn.attach(self.img)
        bn.run(["diskutil", "mount", "-mountPoint", self.mnt, self.dev])
        return self

    def __exit__(self, et, *_):
        for junk in bn.JUNK:
            shutil.rmtree(os.path.join(self.mnt, junk), ignore_errors=True)
        # a busy volume (Spotlight, fseventsd) refuses the unmount; fsck of a still-mounted volume then
        # reports bogus damage (seen once: "Incorrect folder count"), so retry and never fsck it mounted
        for _ in range(20):
            unmounted = subprocess.run(["diskutil", "unmount", self.dev], capture_output=True).returncode == 0
            if unmounted:
                break
            time.sleep(0.5)
        try:
            if et is None and unmounted:
                r = subprocess.run(["fsck_hfs", "-fn", self.dev], capture_output=True, text=True)  # -f: journaled data volume
                self.ok = "appears to be OK" in r.stdout
                if not self.ok:
                    sys.stdout.write(r.stdout[-600:])
        finally:
            subprocess.run(["hdiutil", "detach", self.dev], capture_output=True)
        if et is None and not unmounted:
            raise SystemExit("could not unmount %s (%s)" % (self.mnt, self.dev))
        if et is None and not self.ok:
            raise SystemExit("fsck_hfs is not happy with %s" % self.img)


def walk_files(top):
    for root, dnames, fnames in os.walk(top):
        dnames[:] = [d for d in dnames if d not in bn.JUNK]
        for n in fnames:
            if n not in bn.JUNK:
                yield os.path.join(root, n)


def report(dirs, out=sys.stdout):
    """Print (kind, path) for every non-Apple Mach-O under dirs; returns the count."""
    n = 0
    for top, prefix in dirs:
        for p in walk_files(top):
            if os.path.islink(p) or os.path.getsize(p) < 32 or os.path.getsize(p) > 64 << 20:
                continue
            with open(p, "rb") as f:
                kind = signature_kind(f.read())
            if kind in ("none", "adhoc"):
                out.write("%-6s %s\n" % (kind, prefix + os.path.relpath(p, top)))
                n += 1
    return n


def build(a):
    os.makedirs(a.out, exist_ok=True)
    system, data = os.path.join(a.out, "system.img"), os.path.join(a.out, "data.img")
    p1, p2 = mbr_parts(open(a.mbr, "rb").read(512))[:2]
    # the unit's data partition fills the rest of the exported NAND (3,597,615 x 4 KiB = 14.7 GB, what a
    # restore gives it); the image is sparse, so only what gets written costs the host anything
    data_bytes = p2[2] * BLOCK if a.data_size == "partition" else parse_size(a.data_size)
    if p1[0] != 0xAF:
        raise SystemExit("%s: partition 1 is type %#x, not Apple_HFS" % (a.mbr, p1[0]))

    print("[1/4] system volume from %s" % a.rootfs)
    extract_rootfs(a.rootfs, system)
    sig, bs, total, free = hfs_info(system)
    print("      %s %d x %d B blocks, %d free; partition 1 is %d MiB" % (sig, total, bs, free, p1[2] * BLOCK >> 20))
    if total * bs > p1[2] * BLOCK:
        raise SystemExit("volume larger than partition 1")
    grow_to_partition(system, p1[2])

    print("[2/4] editing the system volume")
    skeleton = tempfile.mkdtemp(prefix="ipad1_var.")
    with Mounted(system, os.path.join(a.out, "mnt-system")) as m:
        if a.kernelcache:
            destination = os.path.join(m.mnt, "System/Library/Caches/com.apple.kernelcaches/kernelcache")
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            shutil.copyfile(a.kernelcache, destination)
            os.chmod(destination, 0o644)
        with open(os.path.join(m.mnt, "private/etc/fstab"), "w") as f:
            f.write(FSTAB_RO if a.ro_root else FSTAB)
        if a.web_proxy:
            os.makedirs(os.path.join(m.mnt, os.path.dirname(PAC_PATH)), exist_ok=True)
            with open(os.path.join(m.mnt, PAC_PATH), "w") as f:
                f.write(PAC)
        rewrite_plist(os.path.join(m.mnt, SB_JOB),
                      lambda d: springboard_env(d, {k: v for k, v in (SB_ENV_CA_OGL if a.ca_ogl else SB_ENV).items()
                                                    if not (a.page_flip and k == "MBX2D_PAGE_FLIP")}))
        if a.appsync:
            src = os.path.join(APPSYNC, "libappsync.dylib")
            why = appsync_problem(src)
            if why:
                raise SystemExit("%s: %s (run contrib/appsync/build.sh)" % (src, why))
            dst = os.path.join(m.mnt, APPSYNC_REL)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
            os.chmod(dst, 0o644)
            for rel in APPSYNC_JOBS:
                rewrite_plist(os.path.join(m.mnt, rel), dyld_insert)
            print("      AppSync: stock libmis retained; installation-service interposition")
        apps_stashed = os.path.islink(os.path.join(m.mnt, "Applications"))
        gli_owned = []                     # files the GL install adds, root-owned below
        if a.gles or a.ca_ogl:   # stock CoreAnimation composites through the GL front end
            engine, info = gli_engine(os.path.join(m.mnt, DYLD_CACHE))
            if not os.path.exists(engine):
                raise SystemExit("%s missing (run contrib/gles-public/build.sh)" % engine)
            status = gli_uncache(m.mnt)
            print("      GL front end %s; %s; %s" % (os.path.basename(engine), status, info))
            gli_owned += [DYLD_OVERRIDE] if "overridden" in status else []
            shutil.copy(engine, os.path.join(m.mnt, OPENGLES_REL))
        if a.gles:
            for app in () if apps_stashed else GLES_APPS:
                shutil.rmtree(os.path.join(m.mnt, "Applications", app), ignore_errors=True)
                shutil.copytree(os.path.join(GLES, app), os.path.join(m.mnt, "Applications", app))
        for label in a.disable:
            hits = [os.path.join(m.mnt, d, label + ".plist") for d in DAEMON_DIRS if os.path.exists(os.path.join(m.mnt, d, label + ".plist"))]
            if not hits:
                raise SystemExit("no launchd job %s in %s" % (label, DAEMON_DIRS))
            rewrite_plist(hits[0], lambda d: d.__setitem__("Disabled", True))
        stashed = os.path.islink(os.path.join(m.mnt, "usr/libexec"))
        sshd = all(os.path.exists(os.path.join(m.mnt, p)) for p in SSHD)
        with open(os.path.join(a.out, "unsigned-machos.txt"), "w") as rep:
            dirs = [(m.mnt, "/")] + ([(a.stash, "/private/var/stash/")] if a.stash else [])
            report(dirs, rep)
        adhoc = sum(1 for l in open(os.path.join(a.out, "unsigned-machos.txt")) if l.startswith("adhoc"))
        # /private/var skeleton for the data volume, taken while the volume is mounted
        shutil.rmtree(skeleton)
        shutil.copytree(os.path.join(m.mnt, "private/var"), skeleton, symlinks=True)
    if a.appsync:     # dyld refuses a DYLD_INSERT dylib not owned by root (noowners wrote the host uid)
        bn.set_owner(system, [APPSYNC_REL], 0, 0)
    if a.web_proxy:
        bn.set_owner(system, ["usr/local", "usr/local/share", "usr/local/share/ltm", PAC_PATH], 0, 0)
    if a.gles or a.ca_ogl:   # ldid-signed: boot with amfi_allow_any_signature=1 cs_enforcement_disable=1
        apps = [] if apps_stashed or not a.gles else GLES_APPS
        bn.set_owner(system, [OPENGLES_REL] + gli_owned + ["Applications/" + app for app in apps] +
                     ["Applications/%s/%s" % (app, f) for app in apps
                      for f in os.listdir(os.path.join(GLES, app))], 0, 0)
    owners = var_owners(system)
    if not os.path.isdir(os.path.join(skeleton, "mobile")):
        # the jailbroken volume's /private/var is just `db`: the skeleton mobile_obliterator copies lives
        # on the IPSW rootfs, so slice that out too (a private temp copy, never the user's mounts)
        print("      %s has no /private/var skeleton; taking it from %s" % (os.path.basename(a.rootfs), os.path.basename(a.pristine)))
        pristine = os.path.join(a.out, "pristine.img")
        extract_rootfs(a.pristine, pristine)
        with Mounted(pristine, os.path.join(a.out, "mnt-pristine")) as m:
            shutil.copytree(os.path.join(m.mnt, "private/var"), skeleton, symlinks=True, dirs_exist_ok=True)
        owners.update(var_owners(pristine))
        os.unlink(pristine)
    print("      fstab %s root; SpringBoard env %s + stdio /dev/console%s" % ("ro" if a.ro_root else "rw", SB_ENV_CA_OGL if a.ca_ogl else SB_ENV,
          "; disabled %s" % a.disable if a.disable else ""))
    print("      sshd job: %s; %d ad-hoc signed Mach-Os in unsigned-machos.txt%s"
          % ("present (jailbreak OpenSSH, inetd-style port 22)" if sshd else "absent", adhoc,
             " (boot with amfi_allow_any_signature=1 cs_enforcement_disable=1)" if adhoc else ""))
    if stashed and not a.stash:
        raise SystemExit("this image stashes /usr/libexec into /private/var/stash; run `fetch` and pass --stash")

    print("[3/4] data volume (%.1f GB, sparse) seeded from /private/var%s%s" % (data_bytes / 1e9,
          " + " + a.stash if a.stash else "", " + " + a.lockdown if a.lockdown else ""))
    if a.stash:
        shutil.copytree(a.stash, os.path.join(skeleton, "stash"), symlinks=True)
        if a.gles and apps_stashed:   # /Applications -> /private/var/stash/Applications
            for app in GLES_APPS:
                shutil.copytree(os.path.join(GLES, app), os.path.join(skeleton, "stash/Applications", app))
    if a.lockdown:
        shutil.copytree(a.lockdown, os.path.join(skeleton, "root/Library/Lockdown"), dirs_exist_ok=True)
    # USB Ethernet first, then the Wi-Fi service: each moves its service to the head of the ServiceOrder, and
    # Wi-Fi (the default network, carrying the proxy PAC) must stay primary. With en1 first, a host running
    # usbmuxd's ipad1 bridge made USB Ethernet the primary service and Safari bypassed the PAC (a4-touch).
    if a.usb_net:
        seed_plist(os.path.join(skeleton, SC_DIR, "NetworkInterfaces.plist"), usb_net_interfaces)
        seed_plist(os.path.join(skeleton, SC_DIR, "preferences.plist"), usb_net_prefs)
        print("      USB Ethernet: en1 DHCP service in /var/%s" % SC_DIR)
    if a.web_proxy:
        seed_plist(os.path.join(skeleton, SC_DIR, "preferences.plist"), wifi_proxy_prefs)
        print("      web proxy: en0 AirPort service first, PAC /%s" % PAC_PATH)
    os.replace(make_hfs_image(data + ".dmg", data_bytes), data)
    by_owner = {}
    with Mounted(data, os.path.join(a.out, "mnt-data")) as m:
        shutil.copytree(skeleton, m.mnt, symlinks=True, dirs_exist_ok=True)
        for root, dnames, fnames in os.walk(m.mnt):
            dnames[:] = [d for d in dnames if d not in bn.JUNK]   # macOS droppings, removed at unmount
            for n in dnames + [f for f in fnames if f not in bn.JUNK]:
                rel = os.path.relpath(os.path.join(root, n), m.mnt)
                by_owner.setdefault(owners.get(rel) or owner_for(rel), []).append(rel)
    shutil.rmtree(skeleton, ignore_errors=True)
    # the mount is noowners as an ordinary user, so everything landed as the host uid: fix the catalog offline
    n = sum(bn.set_owner(data, paths, uid, gid) for (uid, gid), paths in sorted(by_owner.items()))
    print("      owners from the skeleton, else root / mobile by rule: %s (%d catalog records patched)"
          % (", ".join("%d:%d x%d" % (u, g, len(p)) for (u, g), p in sorted(by_owner.items())), n))
    for d in ("mnt-system", "mnt-data", "mnt-pristine"):
        shutil.rmtree(os.path.join(a.out, d), ignore_errors=True)

    print("[4/4] done:\n    %s/ipad1_nand.py build --mbr %s --kernelcache KERNELCACHE --system %s --data %s --out %s/nand-%s"
          % (os.path.dirname(os.path.abspath(__file__)), a.mbr, system, data, os.path.dirname(a.out), a.tag))


def bake(a):
    system = os.path.join(a.dir, "system.img")
    contrib = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../contrib")
    TOOLS, JOBS = dict(globals()["TOOLS"]), dict(globals()["JOBS"])
    if a.seal:      # one-shot clean halt for ipad1_seal.py; it deletes itself on that boot
        TOOLS.update(SEAL_TOOL)
        JOBS.update(SEAL_JOB)
    if a.gl_test:   # the GL fixture job (contrib/it-gltest), for tests/ipad1/gltest.py
        TOOLS.update(GLTEST_TOOL)
        JOBS.update(GLTEST_JOB)
    for name in TOOLS:
        with open(os.path.join(a.tools, name), "rb") as f:
            why = guest_tool_problem(f.read())
        if why:
            raise SystemExit("%s/%s: %s (run contrib/ipad1-guest/build.sh)" % (a.tools, name, why))
    with Mounted(system, os.path.join(a.dir, "mnt-system")) as m:
        for rel in RETIRED:
            if os.path.lexists(os.path.join(m.mnt, rel)):
                os.unlink(os.path.join(m.mnt, rel))
        for name, (rel, mode) in TOOLS.items():
            dst = os.path.join(m.mnt, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(os.path.join(a.tools, name), dst)
            os.chmod(dst, mode)
        for rel, src in JOBS.items():
            shutil.copyfile(os.path.join(contrib, src), os.path.join(m.mnt, rel))
            os.chmod(os.path.join(m.mnt, rel), 0o644)
        rewrite_plist(os.path.join(m.mnt, MSM_JOB), msm_insert)
        if not a.keep_bluetooth:
            rewrite_plist(os.path.join(m.mnt, BT_JOB), lambda d: d.__setitem__("Disabled", True))
        if a.activation_hook:
            activation_hook(a.activation_hook, os.path.join(m.mnt, LOCKDOWND))
        # the GL front end this image carries (build installs it unless --no-ca-ogl): the GL hooks the seed keeps
        engine, _ = gli_engine(os.path.join(m.mnt, DYLD_CACHE))
        gles = False
        # no file when OpenGLES lives only in the shared cache and build installed no front end
        if os.path.exists(os.path.join(m.mnt, OPENGLES_REL)):
            with open(os.path.join(m.mnt, OPENGLES_REL), "rb") as f:
                gles = f.read() == open(engine, "rb").read()
        seeded, record = mkpkg.seed(m.mnt, a.guest_package, gles)
    with open(os.path.join(a.dir, "guest-package.json"), "w") as f:
        json.dump(record, f, indent=1)
    # noowners mount: launchd ignores a job plist that is not root-owned
    n = bn.set_owner(system, ["usr/local", "usr/local/bin", "usr/local/lib"] + list(JOBS) + [rel for rel, _ in TOOLS.values()]
                     + ([LOCKDOWND] if a.activation_hook else []) + seeded, 0, 0)
    shutil.rmtree(os.path.join(a.dir, "mnt-system"), ignore_errors=True)
    print("baked %s + %s + it_boot, seed package %s serial %s (hooks %s) into %s (%d catalog records patched); "
          "rebuild the NAND store with ipad1_nand.py" % (", ".join(TOOLS), ", ".join(os.path.basename(j) for j in JOBS),
                                                        record["family"], record["seed"], record["hooks"], system, n))


LOCKDOWND = "usr/libexec/lockdownd"


def activation_hook(hook, target, args=()):
    """Opt-in: run the user's activation hook (`hook [ARGS] FILE`, edits FILE in place) on a copy of lockdownd, then
    ad-hoc sign the result with lockdownd's own entitlements and write it back (0755; owner fixed by bake)."""
    with tempfile.TemporaryDirectory(prefix="ipad1_hook.") as td:
        work, ents = os.path.join(td, "lockdownd"), os.path.join(td, "entitlements.plist")
        shutil.copyfile(target, work)
        before = open(work, "rb").read()
        with open(ents, "wb") as f:
            subprocess.run(["ldid", "-e", work], stdout=f, check=True)
        subprocess.run(([sys.executable] if hook.endswith(".py") else []) + [hook, *args, work], check=True)
        if open(work, "rb").read() == before:
            raise SystemExit("activation hook %s left lockdownd unchanged" % hook)
        subprocess.run(["ldid", "-S" + ents if os.path.getsize(ents) else "-S", work], check=True)
        shutil.copyfile(work, target)
        os.chmod(target, 0o755)


def fetch(out):
    """Pull /var/stash and /var/root/Library/Lockdown off the real iPad into out/stash and out/lockdown."""
    for sub, parent, name in (("stash", "/var", "stash"), ("lockdown", "/var/root/Library", "Lockdown")):
        d = os.path.join(out, sub)
        os.makedirs(d, exist_ok=True)
        subprocess.run("%s 'tar cf - -C %s %s' | tar xf - -C %s --strip-components 1"
                       % (IPAD_SSH, parent, name, d), shell=True, check=True)
        print("fetched %s: %s" % (d, sorted(os.listdir(d))))


def selfcheck():
    apm = bytearray(512 * 4)
    apm[:2] = b"ER"
    struct.pack_into(">H", apm, 2, 512)
    for i, (start, count, typ) in enumerate([(1, 63, b"Apple_partition_map"), (64, 1000, b"Apple_HFSX")], 1):
        e = 512 * i
        apm[e:e + 2] = b"PM"
        struct.pack_into(">II", apm, e + 4, 2, start)
        struct.pack_into(">I", apm, e + 12, count)
        apm[e + 48:e + 48 + len(typ)] = typ
    assert apm_hfs_slice(bytes(apm)) == (64 * 512, 1000 * 512)

    # the GLI dispatch sanity line: fields the name table knows, one it does not, no @encode at all
    enc = b'{__GLIFunctionDispatchRec="accum"^?"clear"^?"made_up"^?}'
    assert gli_fields(enc) == ["accum", "clear", "made_up"] and gli_fields(b"nothing") is None
    assert gli_dispatch_info("x", enc) == "GLI dispatch: 3 slots, 1 unknown to the name table (made_up)"
    assert gli_dispatch_info("x", b"nothing").startswith("no __GLIFunctionDispatchRec @encode")
    with tempfile.TemporaryDirectory() as mnt:   # the cached-GLEngine override: only with GLEngine cached
        def cache(*paths):
            img = bytearray(b"dyld_v1   armv7\0" + struct.pack("<4I", 0x40, 1, 0x60, len(paths))).ljust(0x40, b"\0")
            img += struct.pack("<QQQ", 0, 0x1000, 0).ljust(32, b"\0")
            names = b"".join(p.encode() + b"\0" for p in paths)
            for i, p in enumerate(paths):
                img += struct.pack("<QQQI", 0, 0, 0, 0x100 + len(b"".join(q.encode() + b"\0" for q in paths[:i]))).ljust(32, b"\0")
            os.makedirs(os.path.join(mnt, os.path.dirname(DYLD_CACHE)), exist_ok=True)
            open(os.path.join(mnt, DYLD_CACHE), "wb").write(bytes(img.ljust(0x100, b"\0")) + names)
        os.makedirs(os.path.join(mnt, "usr/lib"))
        open(os.path.join(mnt, "usr/lib/dyld"), "wb").write(b"x\0/%s\0" % DYLD_OVERRIDE.encode())
        cache("/usr/lib/libz.dylib")
        assert gli_uncache(mnt) == "no cached OpenGLES" and not os.path.exists(os.path.join(mnt, DYLD_OVERRIDE))
        cache("/usr/lib/libz.dylib", "/" + OPENGLES_REL)
        assert "overridden" in gli_uncache(mnt) and os.path.getsize(os.path.join(mnt, DYLD_OVERRIDE)) == 0
        open(os.path.join(mnt, "usr/lib/dyld"), "wb").write(b"no switch")
        try:
            gli_uncache(mnt)
            assert False, "a dyld without the override switch must fail the build"
        except SystemExit:
            pass

    job = {"Label": "com.apple.SpringBoard", "EnvironmentVariables": {"X": "1"}, "KeepAlive": True}
    out = edit_plist(plistlib.dumps(job, fmt=plistlib.FMT_BINARY), springboard_env)
    sb = plistlib.loads(out)
    assert out.startswith(b"bplist") and sb["EnvironmentVariables"] == {"X": "1", **SB_ENV}
    assert sb["StandardErrorPath"] == "/dev/console" and sb["KeepAlive"] is True
    out = edit_plist(plistlib.dumps({"Label": "x"}), lambda d: d.__setitem__("Disabled", True))
    assert out.startswith(b"<?xml") and plistlib.loads(out)["Disabled"] is True
    try:
        edit_plist(plistlib.dumps({"Label": "other"}), springboard_env)
        assert False
    except AssertionError:
        pass

    real = {"CurrentSet": "/Sets/S", "NetworkServices": {"W": {"Interface": {"DeviceName": "en0"}}},
            "Sets": {"S": {"Network": {"Global": {"IPv4": {"ServiceOrder": ["W"]}}, "Service": {"W": {}}}}}}
    usb_net_prefs(real)
    usb_net_prefs(real)                      # idempotent
    net = real["Sets"]["S"]["Network"]
    assert net["Global"]["IPv4"]["ServiceOrder"] == [USB_ETH_SERVICE, "W"] and "W" in net["Service"]
    assert real["NetworkServices"][USB_ETH_SERVICE]["Interface"]["DeviceName"] == "en1"
    fresh = {}
    usb_net_prefs(fresh)
    assert fresh["Sets"][NET_SET]["Network"]["Service"][USB_ETH_SERVICE]["__LINK__"].endswith(USB_ETH_SERVICE)
    ifs = {"Interfaces": [{"BSD Name": "en0", "IOInterfaceUnit": 0, "IOPathMatch": "wifi"}, dict(USB_ETH_IF)]}
    usb_net_interfaces(ifs)
    assert [i["BSD Name"] for i in ifs["Interfaces"]] == ["en0", "en1"]
    plistlib.loads(plistlib.dumps(real)), plistlib.loads(plistlib.dumps(ifs))

    # dyld_insert: appends without clobbering, idempotent
    d = {"EnvironmentVariables": {"CA_ENABLE_OGL": "0"}}
    dyld_insert(d); dyld_insert(d)
    assert d["EnvironmentVariables"]["DYLD_INSERT_LIBRARIES"] == "/" + APPSYNC_REL
    assert d["EnvironmentVariables"]["CA_ENABLE_OGL"] == "0"
    d2 = {"EnvironmentVariables": {"DYLD_INSERT_LIBRARIES": "/usr/lib/other.dylib"}}
    dyld_insert(d2)
    assert d2["EnvironmentVariables"]["DYLD_INSERT_LIBRARIES"] == "/usr/lib/other.dylib:/" + APPSYNC_REL
    wp = {}
    usb_net_prefs(wp)
    wifi_proxy_prefs(wp)
    wifi_proxy_prefs(wp)                       # idempotent
    wnet = wp["Sets"][NET_SET]["Network"]
    assert wnet["Global"]["IPv4"]["ServiceOrder"] == [WIFI_SERVICE, USB_ETH_SERVICE]
    assert wp["NetworkServices"][WIFI_SERVICE]["Proxies"]["ProxyAutoConfigURLString"] == "file:///" + PAC_PATH
    assert "DIRECT" in PAC.split("PROXY 10.0.2.100:3128")[1]

    assert owner_for("mobile") == owner_for("mobile/Library/Preferences/a.plist") == owner_for("ea") == (501, 501)
    assert owner_for("stash/Applications") == owner_for("root/Library/Lockdown") == owner_for("mobileX") == (0, 0)
    assert FSTAB_RO.splitlines()[0] == "/dev/disk0s1 / hfs ro 0 1" and "s2s1" not in FSTAB

    def macho(slots):
        blob = struct.pack(">III", 0xFADE0CC0, 12 + 8 * len(slots), len(slots)) + b"".join(struct.pack(">II", s, 0) for s in slots)
        lc = struct.pack("<IIII", LC_CODE_SIGNATURE, 16, 28 + 16, len(blob))
        return struct.pack("<7I", MH_MAGIC, 12, 9, 2, 1, len(lc), 0) + lc + blob
    assert signature_kind(macho([0, 2, CS_CMS])) == "apple" and signature_kind(macho([0, 2])) == "adhoc"
    assert signature_kind(struct.pack("<7I", MH_MAGIC, 12, 9, 2, 0, 0, 0) + bytes(8)) == "none"
    fat = struct.pack(">II", FAT_MAGIC, 1) + struct.pack(">5I", 12, 9, 28, len(macho([0, 2])), 12) + macho([0, 2])
    assert signature_kind(fat) == "adhoc" and signature_kind(b"#!/bin/sh\n" + bytes(40)) is None

    def tool(cmds, sub=9):
        lcs = b"".join(struct.pack("<II", c, 8) for c in cmds)
        return struct.pack("<7I", MH_MAGIC, 12, sub, 2, len(cmds), len(lcs), 0) + lcs
    assert guest_tool_problem(tool([LC_CODE_SIGNATURE])) is None
    assert "armv7" in guest_tool_problem(tool([LC_CODE_SIGNATURE], sub=6))
    assert "LC_MAIN" in guest_tool_problem(tool([LC_MAIN, LC_CODE_SIGNATURE]))
    assert "unsigned" in guest_tool_problem(tool([]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--selfcheck", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    b = sub.add_parser("build")
    b.add_argument("--base", choices=BASES, default="pristine",
                   help="pristine = IPSW rootfs.dmg, no stash, no AMFI flags needed; jailbroken = the capture + /var/stash")
    b.add_argument("--rootfs", help="override the base's system volume (bare HFS image or IPSW rootfs DMG)")
    b.add_argument("--mbr", default=os.path.join(FILES, "hw2/rdisk0-head4M.bin"))
    b.add_argument("--pristine", default=os.path.join(FILES, "7B500/dec/rootfs.dmg"), help="IPSW rootfs, source of the /private/var skeleton")
    b.add_argument("--out", default=os.path.join(FILES, "userland"), help="images land in OUT/<base>/, the store in OUT/nand-<tag>")
    b.add_argument("--kernelcache", help="IPSW img3 kernelcache to install for real-iBoot fsboot")
    b.add_argument("--data-size", default="partition",
                   help="data volume size: 'partition' (the MBR's partition 2, as on the unit) or e.g. 2g")
    b.add_argument("--stash", help="fetch output for /var/stash (jailbroken default: hw2/stash); 'none' to skip")
    b.add_argument("--lockdown", default=os.path.join(FILES, "hw2/lockdown"), help="fetch output for the Lockdown dir; 'none' to skip")
    b.add_argument("--disable", action="append", default=[], metavar="LABEL", help="launchd job to mark Disabled")
    b.add_argument("--ro-root", action="store_true", help="keep the stock read-only root")
    b.add_argument("--no-web-proxy", dest="web_proxy", action="store_false",
                   help="skip the en0 Wi-Fi service with the web proxy PAC (proxy, else DIRECT)")
    b.add_argument("--no-usb-net", dest="usb_net", action="store_false",
                   help="skip the en1 (USB Ethernet) DHCP network service")
    b.add_argument("--gles", action="store_true", help="also install the GLTest/GLTest2.app test apps (the GL front end itself always goes in; run contrib/gles-public/build.sh and build-apps.sh first)")
    b.add_argument("--page-flip", action="store_true", help="leave CoreAnimation's IOMFB page flipping on (no MBX2D_PAGE_FLIP=0)")
    b.add_argument("--no-ca-ogl", dest="ca_ogl", action="store_false",
                   help="software CoreAnimation (CA_ENABLE_OGL=0) instead of the default GL compositing through the GLI shim")
    b.add_argument("--appsync", action="store_true", help="install libappsync.dylib and inject it into installd (system trust library stays stock) (run contrib/appsync/build.sh first)")
    f = sub.add_parser("fetch")
    f.add_argument("dir", nargs="?", default=os.path.join(FILES, "hw2"))
    r = sub.add_parser("report")
    r.add_argument("dirs", nargs="+")
    k = sub.add_parser("bake")
    k.add_argument("dir", help="a build output dir holding system.img and data.img")
    k.add_argument("--tools", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "../build/ipad1-guest"))
    k.add_argument("--guest-package", default=GUEST_PACKAGE, metavar="ITPACK",
                   help="the armv7 .itpack whose package for this build is baked as the seed (contrib/guest-package/build.sh)")
    k.add_argument("--keep-bluetooth", action="store_true", help="leave com.apple.BTServer enabled (default: Disabled)")
    k.add_argument("--seal", action="store_true", help="also install it_seal, the one-shot clean halt ipad1_seal.py needs")
    k.add_argument("--gl-test", action="store_true", help="also install it_gltest, the GL fixture job tests/ipad1/gltest.py reads")
    k.add_argument("--activation-hook", metavar="SCRIPT", help="opt-in: run SCRIPT on /usr/libexec/lockdownd (then re-signed ad hoc)")
    a = ap.parse_args()
    selfcheck()
    if a.cmd == "build":
        rootfs, stash, a.tag = BASES[a.base]
        a.rootfs = a.rootfs or os.path.join(FILES, rootfs)
        a.stash = a.stash or (os.path.join(FILES, stash) if stash else "none")
        a.out = os.path.join(a.out, a.base)
        for opt in ("stash", "lockdown"):
            if getattr(a, opt) == "none" or not os.path.isdir(getattr(a, opt)):
                print("      no %s seed at %s (run `fetch`)" % (opt, getattr(a, opt)))
                setattr(a, opt, None)
        build(a)
    elif a.cmd == "bake":
        bake(a)
    elif a.cmd == "fetch":
        fetch(a.dir)
    elif a.cmd == "report":
        print("%d non-Apple Mach-Os" % report([(d, d.rstrip("/") + "/") for d in a.dirs]))
    elif not a.selfcheck:
        ap.print_help()


if __name__ == "__main__":
    main()
