#!/usr/bin/env python3
"""Tiny 32-bit Mach-O segment parser + VA<->fileoff mapping for carved kexts."""
import struct, sys


class Macho:
    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        self.segs = []      # (name, vmaddr, vmsize, fileoff, filesize, sects)
        self._parse()

    def _parse(self):
        d = self.data
        magic, cputype, cpusub, ftype, ncmds, sizeofcmds, flags = struct.unpack_from('<7I', d, 0)
        assert magic == 0xfeedface, hex(magic)
        off = 28
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from('<2I', d, off)
            if cmd == 0x1:  # LC_SEGMENT
                name = d[off+8:off+24].rstrip(b'\0').decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4I', d, off+24)
                nsects = struct.unpack_from('<I', d, off+48)[0]
                sects = []
                so = off + 56
                for _s in range(nsects):
                    sname = d[so:so+16].rstrip(b'\0').decode()
                    sgname = d[so+16:so+32].rstrip(b'\0').decode()
                    saddr, ssize, soff = struct.unpack_from('<3I', d, so+32)
                    sects.append((sname, sgname, saddr, ssize, soff))
                    so += 68
                self.segs.append((name, vmaddr, vmsize, fileoff, filesize, sects))
            off += cmdsize

    def off2va(self, o):
        for name, vmaddr, vmsize, fileoff, filesize, _ in self.segs:
            if filesize and fileoff <= o < fileoff + filesize:
                return vmaddr + (o - fileoff)
        return None

    def va2off(self, va):
        for name, vmaddr, vmsize, fileoff, filesize, _ in self.segs:
            if filesize and vmaddr <= va < vmaddr + filesize:
                return fileoff + (va - vmaddr)
        return None

    def text(self):
        for name, vmaddr, vmsize, fileoff, filesize, sects in self.segs:
            for sname, sgname, saddr, ssize, soff in sects:
                if sname == '__text':
                    return saddr, soff, ssize
        return None

    def dump(self):
        for name, vmaddr, vmsize, fileoff, filesize, sects in self.segs:
            print(f'{name:16s} vm {vmaddr:#010x}+{vmsize:#x}  file {fileoff:#x}+{filesize:#x}')
            for s in sects:
                print(f'    {s[1]}.{s[0]:16s} addr {s[2]:#010x} size {s[3]:#x} off {s[4]:#x}')


if __name__ == '__main__':
    Macho(sys.argv[1]).dump()
