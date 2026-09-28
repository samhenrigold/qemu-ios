"""Build A4's GID stand-in from the selected IPSW and catalog keys.

Records are encrypted KBAG (48 bytes) || plaintext IV/key (48 bytes).
No firmware key material belongs in the emulator binary.
"""
import os
import re
import struct


def gid_blobs(z, keysfile):
    with open(keysfile) as f:
        keys = {m[1]: bytes.fromhex(m[2] + m[3])
                for m in re.finditer(r"\n(\S+)\nIV: ([0-9a-fA-F]+)\nKey: ([0-9a-fA-F]+)", f.read())}
    records, names = {}, []
    for name in z.namelist():
        plain = keys.get(os.path.basename(name))
        if plain is None:
            continue
        data = z.read(name)
        if data[:4] != b"3gmI":
            continue
        if len(data) < 20:
            raise ValueError(f"{name}: truncated IMG3 header")
        off, bags = 20, []
        full = struct.unpack_from("<I", data, 4)[0]
        if full < 20 or full > len(data):
            raise ValueError(f"{name}: truncated IMG3")
        while off + 12 <= full:
            tag, total, size = struct.unpack_from("<4sII", data, off)
            if total < 12 or off + total > full or size > total - 12:
                raise ValueError(f"{name}: malformed IMG3 tag")
            if tag == b"GABK" and size >= 8:
                state, bits = struct.unpack_from("<II", data, off + 12)
                if state in (1, 2):
                    if bits != 256 or size != 56 or len(plain) != 48:
                        raise ValueError(f"{name}: invalid AES-256 KBAG/key")
                    bags.append(data[off + 20:off + 68])
            off += total
        for kbag in bags:
            if kbag in records and records[kbag] != plain:
                raise ValueError(f"{name}: conflicting keys for the same KBAG")
            # Production and development KBAGs wrap the same image IV/key
            # under different hardware GIDs; DATA is shared by both modes.
            records[kbag] = plain
        if bags:
            names.append(name)
    if not records:
        raise ValueError("no AES-256 IMG3 keys matched this IPSW")
    return b"".join(k + v for k, v in records.items()), names


def host_usb_devicetree(image, plaintext, records):
    """Describe the emulated HSIC keyboard controller in the NOR DeviceTree.

    Keep the IPSW's KBAG and encryption; change only board configuration data.
    The prepared iBoot already bypasses image signatures, which this invalidates.
    """
    from ipad1_fw import img3_tags, aes_cbc
    from ipad1_kboot import DeviceTree
    tree = DeviceTree(plaintext)
    if 'arm-io/usb-complex' not in tree.props:
        raise ValueError('DeviceTree has no USB complex')
    tree.add('arm-io/usb-complex', 'hsic-enabled')
    plain = bytes(tree.buf)
    pairs = {records[i:i + 48]: records[i + 48:i + 96] for i in range(0, len(records), 96)}
    off, key = 20, None
    while off + 12 <= len(image):
        tag, size, dlen = struct.unpack_from('<4sII', image, off)
        if size < 12:
            break
        if tag == b'GABK' and dlen == 56 and struct.unpack_from('<I', image, off + 12)[0] == 1:
            key = pairs.get(image[off + 20:off + 68])
            break
        off += size
    if key is None:
        raise ValueError('DeviceTree production KBAG has no catalog key')
    off, old_len = img3_tags(image)['DATA']
    old_size = struct.unpack_from('<I', image, off + 4)[0]
    encrypted = aes_cbc(plain + b'\0' * (-len(plain) % 16), key[:16], key[16:], decrypt=False)
    tag = struct.pack('<4sII', b'ATAD', 12 + len(encrypted), len(plain)) + encrypted
    result = bytearray(image[:off] + tag + image[off + old_size:])
    delta = len(tag) - old_size
    # IMG3 full size, data-area size and signature offset all grow by the DATA delta.
    for field in (4, 8, 12):
        value = struct.unpack_from('<I', result, field)[0]
        struct.pack_into('<I', result, field, value + delta)
    return bytes(result)
