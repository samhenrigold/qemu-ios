#!/usr/bin/env python3
"""Fail fast when a qemu-system-arm links an FFmpeg without contrib/ffmpeg's patches.

The iPod H.264 bridge needs h264-chunk-er.patch; stock (Homebrew) FFmpeg has the
same version number, so only the path of the linked libavcodec tells them apart.
    ffmpeg_guard.check(qemu_path)  -> None when fine, else a one-line reason
    ffmpeg_guard.py QEMU           -> exit 0 / 1 with the reason
"""
import os, re, subprocess, sys

PATCHED_PREFIX = os.environ.get(
    "FFMPEG_PREFIX", os.path.expanduser("~/Developer/qemu-ios/build-native14/prefix"))
FIX = "reconfigure with scripts/configure-patched-ffmpeg"


def linked_libavcodec(qemu):
    """The libavcodec dylib qemu loads, with @rpath resolved; None if not linked."""
    out = subprocess.run(["otool", "-l", "-L", qemu], capture_output=True, text=True,
                         check=True).stdout
    lib = next((l.split()[0] for l in out.splitlines()
                if "libavcodec" in l and "compatibility version" in l), None)
    if lib and lib.startswith("@rpath/"):
        here = os.path.dirname(os.path.realpath(qemu))
        for rpath in re.findall(r"^\s+path (\S+) \(offset", out, re.M):
            rpath = rpath.replace("@loader_path", here).replace("@executable_path", here)
            cand = os.path.join(rpath, lib[len("@rpath/"):])
            if os.path.exists(cand):
                return os.path.realpath(cand)
    return os.path.realpath(lib) if lib else None


def check(qemu):
    lib = linked_libavcodec(qemu)
    if not lib:
        return "%s links no libavcodec (no iPod H.264/AAC decode); %s" % (qemu, FIX)
    if not lib.startswith(os.path.realpath(PATCHED_PREFIX) + os.sep):
        return "%s links %s, not the patched FFmpeg in %s; %s" % (qemu, lib, PATCHED_PREFIX, FIX)
    return None


if __name__ == "__main__":
    why = check(sys.argv[1])
    print(why or "ok: patched FFmpeg")
    sys.exit(1 if why else 0)
