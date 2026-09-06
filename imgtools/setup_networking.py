#!/usr/bin/env python3
"""Enable a working network stack *with DNS* on a NAND COW clone.

The BCM4325 WiFi model gets iOS onto the network at the transport layer - it
takes a DHCP lease, ARPs, and moves TCP. Images missing the service/daemon
configuration need the filesystem setup below before userspace (SpringBoard,
Safari, CFNetwork) can use it. These changes are applied offline. The current
prepared 7E18 image already includes networking; the resolver investigation
below records the historical 5F138 setup, not a current 3.1.3 failure.

  1. A SystemConfiguration service. The image ships an EMPTY /var/preferences,
     so PreferencesMonitor has nothing to publish under Setup:/Network/Service,
     IPMonitor never elects a primary service, and State:/Network/Global/IPv4 is
     never set - the exact state SCNetworkReachability reports as "no network",
     however well the link works. We write a minimal preferences.plist defining
     one en0/AirPort service on ConfigMethod=DHCP. IPMonitor then elects it,
     installs the default route, and publishes the global IPv4/DNS state.

  2. mDNSResponder, the resolver. gethostbyname on this build (libinfo_v1 +
     libdns_sd mDNSResponder-178.2) goes to the daemon over the mach service
     com.apple.mDNSResponder / the UDS /var/run/mDNSResponder. The image ships
     the binary but NO LaunchDaemon for it, so nothing starts it and every name
     lookup fails before a packet is sent.

     The catch that makes this a *tool* and not a one-line file drop: launchd
     silently ignores a LaunchDaemon plist that is not owned by root, and any
     file created inside the mounted volume by the host user (uid 501) is owned
     by that user, not root - so a freshly written com.apple.mDNSResponder.plist
     never loads. Rewriting an EXISTING root-owned plist *in place* keeps the
     inode's root ownership. So we overwrite a dispensable, root-owned daemon -
     ReportCrash.SafetyNet by default - with the mDNSResponder job. launchd keys
     jobs by the Label inside the plist, not the filename, so the daemon comes up
     as com.apple.mDNSResponder.

  3. A known-network entry for the model's own SSID (qemu-ios), so configd
     AUTO-JOINS at boot with no UI. This matters because tapping the network in
     Settings drives a UI join whose confirmation the current association model
     does not satisfy - it loops "Unable to join" and wedges the Settings modal -
     whereas a *known* network is joined silently by configd. The driver still
     associates for real (its own WLC_SET_SSID); this only removes the need to
     tap. Pass --ssid "" to skip it.

After this the device auto-associates at boot, takes a DHCP lease, runs the
resolver, and Safari loads pages by name. Names that resolve locally (/etc/hosts,
mDNS) load; a remote name that needs a unicast DNS round-trip is still gated by
SCNetworkReachabilityCreateWithName (a SystemConfiguration-internal limitation,
documented in docs/networking.md), not by anything this tool controls. The DNS
server itself comes from DHCP, so nothing here is tied to a particular
slirp/vmnet address.

Files written in the guest volume:
  /var/preferences/SystemConfiguration/preferences.plist        (new)
  /Library/Preferences/SystemConfiguration/preferences.plist    (new)
  /var/preferences/SystemConfiguration/com.apple.wifi.plist     (new, unless --ssid "")
  /System/Library/LaunchDaemons/<hijack-target>.plist           (rewritten in place)

Usage:
    imgtools/setup_networking.py --nand <NAND-COW-clone>
        [--hijack com.apple.ReportCrash.SafetyNet]   # daemon plist to repurpose
      (reassembles the volume, applies the changes, fsck_hfs, writes changed
       pages back; refuses the golden image)
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN = "/Users/shg/Developer/qemu-ios-files/nand"

# A minimal SCPreferences tree: one Automatic set, one en0/AirPort service on
# DHCP, and the __LINK__ that ties them together. This is the layout 2.x's
# SystemConfiguration parses; the schema drifted later.
PREFERENCES_PLIST = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
\t<key>CurrentSet</key>
\t<string>/Sets/0</string>
\t<key>NetworkServices</key>
\t<dict>
\t\t<key>0</key>
\t\t<dict>
\t\t\t<key>DNS</key>
\t\t\t<dict/>
\t\t\t<key>IPv4</key>
\t\t\t<dict>
\t\t\t\t<key>ConfigMethod</key>
\t\t\t\t<string>DHCP</string>
\t\t\t</dict>
\t\t\t<key>Interface</key>
\t\t\t<dict>
\t\t\t\t<key>DeviceName</key>
\t\t\t\t<string>en0</string>
\t\t\t\t<key>Hardware</key>
\t\t\t\t<string>AirPort</string>
\t\t\t\t<key>Type</key>
\t\t\t\t<string>Ethernet</string>
\t\t\t</dict>
\t\t\t<key>Proxies</key>
\t\t\t<dict>
\t\t\t\t<key>ExceptionsList</key>
\t\t\t\t<array>
\t\t\t\t\t<string>*.local</string>
\t\t\t\t\t<string>169.254/16</string>
\t\t\t\t</array>
\t\t\t</dict>
\t\t\t<key>UserDefinedName</key>
\t\t\t<string>Wi-Fi</string>
\t\t</dict>
\t</dict>
\t<key>Sets</key>
\t<dict>
\t\t<key>0</key>
\t\t<dict>
\t\t\t<key>Network</key>
\t\t\t<dict>
\t\t\t\t<key>Global</key>
\t\t\t\t<dict>
\t\t\t\t\t<key>IPv4</key>
\t\t\t\t\t<dict>
\t\t\t\t\t\t<key>ServiceOrder</key>
\t\t\t\t\t\t<array>
\t\t\t\t\t\t\t<string>0</string>
\t\t\t\t\t\t</array>
\t\t\t\t\t</dict>
\t\t\t\t</dict>
\t\t\t\t<key>Service</key>
\t\t\t\t<dict>
\t\t\t\t\t<key>0</key>
\t\t\t\t\t<dict>
\t\t\t\t\t\t<key>__LINK__</key>
\t\t\t\t\t\t<string>/NetworkServices/0</string>
\t\t\t\t\t</dict>
\t\t\t\t</dict>
\t\t\t</dict>
\t\t\t<key>UserDefinedName</key>
\t\t\t<string>Automatic</string>
\t\t</dict>
\t</dict>
\t<key>System</key>
\t<dict>
\t\t<key>System</key>
\t\t<dict>
\t\t\t<key>ComputerName</key>
\t\t\t<string>iPod-touch</string>
\t\t\t<key>HostName</key>
\t\t\t<string>iPod-touch</string>
\t\t</dict>
\t</dict>
</dict>
</plist>
"""

# The canonical 10.5 mDNSResponder job: -launchd, the mach service, and the
# client-facing Unix socket at /var/run/mDNSResponder. This becomes the CONTENT
# of an existing root-owned daemon plist so it stays root-owned.
MDNS_PLIST = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
\t<key>Label</key>
\t<string>com.apple.mDNSResponder</string>
\t<key>ProgramArguments</key>
\t<array>
\t\t<string>/usr/sbin/mDNSResponder</string>
\t\t<string>-launchd</string>
\t</array>
\t<key>MachServices</key>
\t<dict>
\t\t<key>com.apple.mDNSResponder</key>
\t\t<true/>
\t\t<key>com.apple.mDNSResponder.dnsproxy</key>
\t\t<true/>
\t</dict>
\t<key>Sockets</key>
\t<dict>
\t\t<key>Listeners</key>
\t\t<dict>
\t\t\t<key>SockPathName</key>
\t\t\t<string>/var/run/mDNSResponder</string>
\t\t\t<key>SockPathMode</key>
\t\t\t<integer>438</integer>
\t\t</dict>
\t</dict>
\t<key>RunAtLoad</key>
\t<true/>
\t<key>KeepAlive</key>
\t<true/>
\t<key>InitGroups</key>
\t<true/>
</dict>
</plist>
"""

# Python run inside the editimg mount with $MNT set. Kept as a here-doc so the
# tool stays a single file.
INNER = r'''
import os, subprocess, sys, plistlib, datetime
mnt = os.environ["MNT"]
hijack = os.environ["HIJACK"]
ssid = os.environ.get("KNOWN_SSID", "")

prefs = r"""{prefs}"""
mdns  = r"""{mdns}"""

# 1. SystemConfiguration preferences, in both places configd looks.
for d in ("var/preferences/SystemConfiguration",
          "Library/Preferences/SystemConfiguration"):
    p = os.path.join(mnt, d)
    os.makedirs(p, exist_ok=True)
    fp = os.path.join(p, "preferences.plist")
    with open(fp, "w") as f:
        f.write(prefs)
    subprocess.run(["plutil", "-lint", fp], check=True)
print("wrote SystemConfiguration preferences (en0/AirPort, DHCP)")

# 2. mDNSResponder daemon, by rewriting a root-owned plist IN PLACE so it keeps
#    root ownership (launchd ignores non-root daemon plists).
target = os.path.join(mnt, "System/Library/LaunchDaemons", hijack + ".plist")
if not os.path.exists(target):
    sys.exit("hijack target not present: %s" % target)
# NB: this mount does not enforce on-disk ownership, so os.stat() here reports
# the mounting user, not the file's real uid - it cannot be used to check that
# the target is root-owned. Correctness rests on the mechanism instead: the
# stock LaunchDaemons are root-owned on disk, and truncating an existing file in
# place keeps its inode (hence its on-disk owner), verified separately by
# reassembling the volume and reading it back through an owner-honouring mount.
with open(target, "wb") as f:            # truncate in place: inode + owner kept
    f.write(mdns.encode())
subprocess.run(["plutil", "-lint", target], check=True)
print("rewrote %s in place -> com.apple.mDNSResponder (kept root ownership)"
      % (hijack + ".plist"))

# 3. Known-network entry so configd auto-joins the model's SSID at boot.
if ssid:
    d = os.path.join(mnt, "var/preferences/SystemConfiguration")
    os.makedirs(d, exist_ok=True)
    wp = os.path.join(d, "com.apple.wifi.plist")
    doc = {{
        "AllowEnable": True,
        "WiFiEnabled": True,
        "allow join": True,
        "List of known networks": [{{
            "SSID_STR": ssid,
            "SSID": ssid.encode(),
            "AP_MODE": 2,
            "CHANNEL": 6,
            "lastJoined": datetime.datetime(2024, 1, 1, 0, 0, 0),
        }}],
    }}
    with open(wp, "wb") as f:
        plistlib.dump(doc, f)
    subprocess.run(["plutil", "-lint", wp], check=True)
    print("wrote known network %r (auto-join at boot, no UI)" % ssid)
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", required=True,
                    help="NAND page dir (a COW clone, never the golden image)")
    ap.add_argument("--hijack", default="com.apple.ReportCrash.SafetyNet",
                    help="root-owned LaunchDaemon whose plist is repurposed to "
                         "run mDNSResponder (default: ReportCrash.SafetyNet)")
    ap.add_argument("--ssid", default="qemu-ios",
                    help="SSID to seed as a known network for silent auto-join "
                         "(default: qemu-ios, the model's own SSID; pass '' to skip)")
    ap.add_argument("--workdir", default=None)
    ap.add_argument("--blocks", type=int, default=128000)
    a = ap.parse_args()

    if os.path.realpath(a.nand) == os.path.realpath(GOLDEN):
        raise SystemExit("refusing to edit the golden image; work on a COW clone (cp -Rc)")

    inner = INNER.format(prefs=PREFERENCES_PLIST, mdns=MDNS_PLIST)
    shell = '#!/bin/sh\nset -e\nexec python3 - <<\'PYEOF\'\n%s\nPYEOF\n' % inner

    with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False) as f:
        f.write(shell)
        script = f.name
    os.chmod(script, 0o755)

    env = dict(os.environ, HIJACK=a.hijack, KNOWN_SSID=a.ssid)
    try:
        cmd = [sys.executable, os.path.join(HERE, "editimg.py"),
               "--nand", a.nand, "--script", script, "--blocks", str(a.blocks)]
        if a.workdir:
            cmd += ["--workdir", a.workdir]
        subprocess.run(cmd, check=True, env=env)
    finally:
        os.unlink(script)

    print("\nDONE. Networking-with-DNS enabled on the clone.")
    print("Boot with -netdev user,id=wifi0,... and the WiFi model; the driver")
    print("associates for real (no IPOD_WIFI_FAKE_LINK needed on current builds).")


if __name__ == "__main__":
    main()
