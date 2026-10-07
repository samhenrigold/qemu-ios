#!/usr/bin/env python3
"""Turn on SpringBoard's own launch-workflow log on iOS 3.1.3 (7E18).

SpringBoard carries a built-in state-machine tracer -- "PUSHED *Preactivated
display %@", "Application finished launching %@", "Finished animating
activation of app %@", "POPPED *Activated display %@" and ~40 more -- behind a
log-level global (VA 0x107224) that `-[SpringBoard userDefaultsDidChange:]`
sets from the `SBLogging` preference.  Even switched on, the entries only go
into a 100-entry in-memory NSMutableArray; the code that dumps that array
(NSLog of the whole array at level 3) is reached from a UI gesture we have not
identified, so the preference alone gets you nothing.

This patches the logging helper itself (VA 0x10ffc) into a tail-call to NSLog.
The helper's signature is already `(CFStringRef fmt, ...)` with the varargs in
r0-r3, i.e. exactly NSLog's, so the whole body can be replaced by three
instructions:

    00010ffc  push {r7, lr}
    00010ffe  blx  #0xb0f74          ; _NSLog stub
    00011002  pop  {r7, pc}

That also removes the level gate, so every workflow event is logged
unconditionally, with SpringBoard's own timestamps.

    python3 imgtools/sb_workflow_log.py <SpringBoard binary> [-o out]
    ldid -S<entitlements.xml> out          # the image must boot with
                                           # IT_BOOT_ARGS=amfi_allow_any_signature=1

READING THE OUTPUT.  NSLog goes to ASL and, early on, to stderr.  Adding
`StandardErrorPath` to /System/Library/LaunchDaemons/com.apple.SpringBoard.plist
works, but only after an explicit `launchctl unload`+`load` -- a plain reboot
does not re-read the plist -- and SpringBoard stops writing to stderr a few
seconds after boot, so ASL is the reliable sink:

    idevicepair pair          # idevicesyslog fails with lockdownd -8 unpaired
    idevicesyslog

Offsets are for the 3.1.3 (7E18) SpringBoard; the script refuses to patch
anything else.
"""
import argparse, shutil, struct, sys

OFF = 0xfffc            # file offset of VA 0x10ffc (__TEXT is mapped at +0)
ORIG = bytes.fromhex("0fb4f0b5464640b4")
NEW  = bytes([0x80, 0xB5, 0x9F, 0xF0, 0xBA, 0xEF, 0x80, 0xBD])

# --syslog: bypass NSLog and call syslog(3) directly.
#
#     00010ffc  push {r7, lr}
#     00010ffe  adds r2, r0, #0     ; r2 = the CFStringRef format (event identity)
#     00011000  movs r0, #4         ; LOG_WARNING
#     00011002  adr  r1, #0xc       ; -> "SBW %p" at 0x11010
#     00011004  blx  #0xb1d00       ; _syslog
#     00011008  pop  {r7, pc}
#
# syslog is a C printf, so it cannot render the workflow formats' %@ arguments
# -- but it does not need to.  Every format is a *constant* CFString at a fixed
# address in __cfstring, so logging the pointer identifies the event exactly;
# resolve it back to text offline against the __cfstring table (this file's
# companion dump).  The point of doing it this way is that syslog reaches ASL
# without going through CFLog, which is the part that stops being readable.
SYSLOG = bytes([0x80, 0xB5, 0x02, 0x1C, 0x04, 0x20, 0x03, 0xA1,
                0xA0, 0xF0, 0x7C, 0xEE, 0x80, 0xBD,
                0xC0, 0x46, 0xC0, 0x46, 0xC0, 0x46]) + b"SBW %p\0\0"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("-o", "--out")
    ap.add_argument("--syslog", action="store_true",
                    help="call syslog(3) instead of NSLog (reaches ASL directly)")
    a = ap.parse_args()

    out = a.out or a.binary + ".logging"
    if out != a.binary:
        shutil.copyfile(a.binary, out)
    with open(out, "r+b") as f:
        f.seek(OFF)
        want = SYSLOG if a.syslog else NEW
        cur = f.read(len(ORIG))
        if cur in (NEW, SYSLOG[:len(ORIG)]):
            print("already patched")
            return
        if cur != ORIG:
            raise SystemExit("not a 3.1.3 (7E18) SpringBoard: %s at %#x"
                             % (cur.hex(), OFF))
        f.seek(OFF)
        f.write(want)
    print("patched %s (%s)" % (out, "syslog" if a.syslog else "NSLog"))


if __name__ == "__main__":
    main()
