#!/usr/bin/env python3
"""Unpack an iPad1,1 IPSW into the decrypted files the ipad1 machine and research use.

    ipad1_fw.py IPSW KEYS OUTDIR

KEYS is a theiphonewiki/theapplewiki key page saved as text ("Name\\nfile\\nIV: ..\\nKey: ..").
Component paths come from the IPSW's BuildManifest.plist; KEYS only supplies IV/Key per file name.
Writes OUTDIR/{iBSS,iBEC,iBoot,LLB,DeviceTree,AppleLogo}.bin, kernelcache.mach (Adler-32 checked),
DeviceTree.txt, rootfs.dmg (vfdecrypt'd; mount with hdiutil) and the two ramdisk .dmg files.
Needs openssl on PATH and cc (vfdecrypt.c is built on first use).
"""
import os, plistlib, re, struct, subprocess, sys, zipfile, zlib


def img3_tags(data):
    assert data[:4] == b"3gmI", "not an img3"
    full = struct.unpack_from("<I", data, 4)[0]
    off, tags = 0x14, {}
    while off + 12 <= full:
        magic = data[off:off + 4][::-1].decode("latin1")
        total, dlen = struct.unpack_from("<II", data, off + 4)
        if total < 12:
            break
        tags[magic] = (off, dlen)
        off += total
    return tags


def aes_cbc(buf, iv, key, decrypt=True):
    algo = {16: "aes-128-cbc", 24: "aes-192-cbc", 32: "aes-256-cbc"}[len(key)]
    return subprocess.run(["openssl", "enc", "-d" if decrypt else "-e", "-" + algo, "-nopad",
                           "-K", key.hex(), "-iv", iv.hex()],
                          input=buf, capture_output=True, check=True).stdout


def img3_decrypt(data, iv, key, plain_tail=False):
    off, dlen = img3_tags(data)["DATA"]
    if plain_tail:
        # 2.x: the final partial block is left in plaintext
        n = dlen & ~15
        return aes_cbc(data[off + 12:off + 12 + n], iv, key) + data[off + 12 + n:off + 12 + dlen]
    # 3.x+: the final partial block is encrypted into the tag's padding, not left in plaintext.
    n = (dlen + 15) & ~15
    return aes_cbc(data[off + 12:off + 12 + n], iv, key)[:dlen]


def lzss(src):
    N, F = 4096, 18
    ring, r, out, flags, i = bytearray(b" " * N), N - F, bytearray(), 0, 0
    while i < len(src):
        flags >>= 1
        if not flags & 0x100:
            flags, i = src[i] | 0xFF00, i + 1
        if flags & 1 and i < len(src):
            c, i = src[i], i + 1
            out.append(c); ring[r] = c; r = (r + 1) & (N - 1)
        elif not flags & 1 and i + 1 < len(src):
            pos, length = src[i] | ((src[i + 1] & 0xF0) << 4), (src[i + 1] & 0x0F) + 3
            i += 2
            for k in range(length):
                c = ring[(pos + k) & (N - 1)]
                out.append(c); ring[r] = c; r = (r + 1) & (N - 1)
        else:
            break
    return bytes(out)


def complzss(payload):
    assert payload[:8] == b"complzss", "not complzss"
    adler, ulen, clen = struct.unpack_from(">III", payload, 8)
    out = lzss(payload[0x180:0x180 + clen])
    assert len(out) == ulen and zlib.adler32(out) == adler, "kernelcache checksum mismatch"
    return out


def dt_dump(buf):
    lines = []

    def node(off, depth):
        nprops, nchildren = struct.unpack_from("<II", buf, off)
        off += 8
        for _ in range(nprops):
            name = buf[off:off + 32].split(b"\0", 1)[0].decode("latin1")
            ln = struct.unpack_from("<I", buf, off + 32)[0]
            val = buf[off + 36:off + 36 + (ln & 0x7FFFFFFF)]
            text = val.rstrip(b"\0")
            shown = repr(text.decode("latin1")) if val and val[-1:] == b"\0" and all(
                32 <= b < 127 or b == 0 for b in val) else val.hex()
            lines.append("  " * depth + f"{name} = {shown}" + (" [placeholder]" if ln & 0x80000000 else ""))
            off += 36 + (((ln & 0x7FFFFFFF) + 3) & ~3)
        for _ in range(nchildren):
            lines.append("  " * depth + "{")
            off = node(off, depth + 1)
            lines.append("  " * depth + "}")
        return off

    node(0, 0)
    return "\n".join(lines) + "\n"


def vfdecrypt(src, dst, key_hex):
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vfdecrypt")
    if not os.path.exists(tool):
        subprocess.run(["cc", "-O2", "-o", tool, tool + ".c"], check=True)
    subprocess.run([tool, src, dst, key_hex], check=True)


# Output name -> BuildManifest component; paths come from the IPSW's own manifest (Erase identity).
COMPONENTS = {"iBSS": "iBSS", "iBEC": "iBEC", "iBoot": "iBoot", "LLB": "LLB", "DeviceTree": "DeviceTree",
              "AppleLogo": "AppleLogo", "Kernelcache": "KernelCache"}


def components(z):
    """{component: IPSW path} from BuildManifest.plist's first (Customer Erase) identity, plus the
    Update identity's RestoreRamDisk as "UpdateRamDisk". 2.x IPSWs have no BuildManifest: Restore.plist
    names the kernelcache, rootfs and ramdisks, and the all_flash/dfu file names are the board's."""
    if "BuildManifest.plist" not in z.namelist():
        r = plistlib.loads(z.read("Restore.plist"))
        board, plat = r["DeviceMap"][0]["BoardConfig"], r["DeviceMap"][0]["Platform"]
        af = "Firmware/all_flash/all_flash.%s.production/" % board
        comp = {"iBSS": "Firmware/dfu/iBSS.%s.RELEASE.dfu" % board, "iBEC": "Firmware/dfu/iBEC.%s.RELEASE.dfu" % board,
                "iBoot": af + "iBoot.%s.RELEASE.img3" % board, "LLB": af + "LLB.%s.RELEASE.img3" % board,
                "DeviceTree": af + "DeviceTree.%s.img3" % board, "AppleLogo": af + "applelogo.%s.img3" % plat,
                "KernelCache": r["KernelCachesByPlatform"][plat]["Release"], "OS": r["SystemRestoreImages"]["User"],
                "RestoreRamDisk": r["RestoreRamDisks"]["User"], "UpdateRamDisk": r["RestoreRamDisks"]["Update"]}
        missing = [v for v in comp.values() if v not in z.namelist()]
        if missing:
            raise SystemExit("no BuildManifest.plist, and Restore.plist-derived paths are missing: %s" % missing)
        return comp
    ids = plistlib.loads(z.read("BuildManifest.plist"))["BuildIdentities"]
    comp = {k: v["Info"]["Path"] for k, v in ids[0]["Manifest"].items() if "Path" in v.get("Info", {})}
    for bi in ids[1:]:
        if bi["Info"].get("RestoreBehavior") == "Update":
            comp["UpdateRamDisk"] = bi["Manifest"]["RestoreRamDisk"]["Info"]["Path"]
    return comp


def main(ipsw, keysfile, out):
    text = open(keysfile).read()
    keys = {m[2]: (bytes.fromhex(m[3]), bytes.fromhex(m[4]))
            for m in re.finditer(r"\n([^\n]+)\n(\S+)\nIV: (\w+)\nKey: (\w+)", text)}
    os.makedirs(out, exist_ok=True)
    z = zipfile.ZipFile(ipsw)
    comp = components(z)

    def key(path):
        if os.path.basename(path) not in keys:
            raise SystemExit("%s: no IV/Key for %s (BuildManifest names it)" % (keysfile, os.path.basename(path)))
        return keys[os.path.basename(path)]

    # Which tail convention this firmware uses, from the one component with a checksum: the kernelcache.
    kc = z.read(comp["KernelCache"])
    plain_tail = False
    try:
        complzss(img3_decrypt(kc, *key(comp["KernelCache"])))
    except AssertionError:
        plain_tail = True
        print("final partial AES block is plaintext (2.x img3)")
    for name, c in COMPONENTS.items():
        payload = img3_decrypt(z.read(comp[c]), *key(comp[c]), plain_tail=plain_tail)
        if name == "Kernelcache":
            open(f"{out}/kernelcache.mach", "wb").write(complzss(payload))
        else:
            open(f"{out}/{name}.bin", "wb").write(payload)
        if name == "DeviceTree":
            open(f"{out}/DeviceTree.txt", "w").write(dt_dump(payload))
        print("ok", name, comp[c])
    for c in ("RestoreRamDisk", "UpdateRamDisk"):
        if c in comp:
            fname = comp[c]
            open(f"{out}/{fname[:-4]}-ramdisk.dmg", "wb").write(img3_decrypt(z.read(fname), *key(fname), plain_tail=plain_tail))
            print("ok", c, fname)
    osimg = comp["OS"]
    rootfs = re.search(r"\n%s\nKey: (\w+)" % re.escape(osimg), text)
    if not rootfs:
        raise SystemExit("%s: no Key for the root filesystem %s" % (keysfile, osimg))
    z.extract(osimg, out)
    vfdecrypt(f"{out}/{osimg}", f"{out}/rootfs.dmg", rootfs[1])
    os.remove(f"{out}/{osimg}")
    print("ok rootfs.dmg", osimg)


def selfcheck():
    """Round-trip an odd-length payload through encrypt -> img3 -> img3_decrypt."""
    iv, key = bytes(16), bytes(range(32))
    plain = bytes(range(37))
    enc = aes_cbc(plain + bytes(11), iv, key, decrypt=False)
    tag = b"ATAD" + struct.pack("<II", 12 + len(enc), len(plain)) + enc  # magics are byte-reversed
    img = b"3gmI" + struct.pack("<IIII", 0x14 + len(tag), 0, 0, 0) + tag
    assert img3_decrypt(img, iv, key) == plain
    assert lzss(bytes([0xFF]) + b"abcdefgh") == b"abcdefgh"


if __name__ == "__main__":
    selfcheck()
    if len(sys.argv) == 4:
        main(*sys.argv[1:])
    elif len(sys.argv) != 1:
        sys.exit(__doc__)
