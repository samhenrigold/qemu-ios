#!/usr/bin/env python3
"""Bounded Mach-O/import audit for rebuilt legacy fixtures; no SDK redistribution."""
import argparse
import hashlib
import json
import plistlib
from pathlib import Path
import struct


def thin(data):
    if data[:4] == b'\xca\xfe\xba\xbe':
        count=struct.unpack_from('>I',data,4)[0]
        for i in range(count):
            cpu,subtype,offset,size,align=struct.unpack_from('>5I',data,8+i*20)
            if cpu==12 and subtype&0xffffff in (6,9):
                return data[offset:offset+size]
        raise ValueError('no ARMv6/ARMv7 SDK link stub')
    return data


def macho(data):
    data=thin(data)
    magic,cpu,subtype,kind,count,size,flags=struct.unpack_from('<7I',data)
    if magic!=0xfeedface or cpu!=12: raise ValueError('requires 32-bit ARM Mach-O')
    offset=28;commands=[];imports=set();exports=set();libraries=[];encrypted=False
    for _ in range(count):
        cmd,length=struct.unpack_from('<II',data,offset)
        if length<8 or offset+length>28+size: raise ValueError('malformed load command')
        commands.append(cmd)
        if cmd==0x21:
            encrypted = encrypted or bool(struct.unpack_from("<I",data,offset+16)[0])
        if cmd==2:
            symoff,nsyms,stroff,strsize=struct.unpack_from('<4I',data,offset+8)
            strings=data[stroff:stroff+strsize]
            for i in range(nsyms):
                index,typ,sect,desc,value=struct.unpack_from('<IBBHI',data,symoff+i*12)
                end=strings.find(b'\0',index)
                if end<0: raise ValueError('unterminated symbol')
                name=strings[index:end].decode('utf8')
                if typ&1 and not typ&0xe0:
                    if typ&0x0e==0 and value==0: imports.add(name)
                    elif typ&0x0e!=0: exports.add(name)
        if cmd==0xc:
            nameoff=struct.unpack_from('<I',data,offset+8)[0]
            libraries.append(data[offset+nameoff:offset+length].split(b'\0')[0].decode('utf8'))
        offset+=length
    if offset!=28+size: raise ValueError('bad command span')
    return dict(cpu=cpu,subtype=subtype,kind=kind,flags=flags,commands=commands,
                imports=sorted(imports),exports=sorted(exports),libraries=libraries,encrypted=encrypted)


def validate_sdk(sdk, version):
    info=plistlib.loads((Path(sdk)/"SDKSettings.plist").read_bytes())
    if info.get("CanonicalName") != "iphoneos"+version or info.get("Version") != version:
        raise ValueError("fixture requires actual iPhoneOS SDK "+version)


def audit(binary,sdk,legacy):
    data=Path(binary).read_bytes();info=macho(data)
    if (info['subtype'],info['kind'])!=(6,2): raise ValueError('requires ARMv6 executable')
    if 5 not in info['commands'] or 0x80000028 in info['commands']: raise ValueError('requires LC_UNIXTHREAD')
    if info['encrypted']: raise ValueError('encrypted fixture')
    if 0x1d not in info['commands']: raise ValueError('missing signature command')
    if legacy:
        forbidden={0x22,0x80000022,0x80000028,0x25,0x32,0x2a}
        if forbidden.intersection(info['commands']) or info['flags']&0x200000:
            raise ValueError('unsupported legacy load commands/PIE')
    stub=Path(sdk)/'usr/lib/libSystem.dylib';sdkinfo=macho(stub.read_bytes())
    missing=set(info['imports'])-set(sdkinfo['exports'])
    if missing: raise ValueError('imports absent from supplied SDK libSystem: '+', '.join(sorted(missing)))
    if len(info['libraries'])!=1 or 'libSystem' not in info['libraries'][0]:
        raise ValueError('unexpected statically linked framework/dependency')
    info.pop('exports');info.update(binary=str(Path(binary).resolve()),sha256=hashlib.sha256(data).hexdigest(),
        sdkStubSHA256=hashlib.sha256(stub.read_bytes()).hexdigest(),legacy=legacy,
        limits='Static imports and load commands only; dynamic framework/API and native rendering/audio need guest qualification')
    return info


if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('binary');ap.add_argument('--sdk',required=True)
    ap.add_argument('--legacy',action='store_true');ap.add_argument('--copy-link-stub');ap.add_argument('--sdk-version')
    args=ap.parse_args()
    if args.sdk_version: validate_sdk(args.sdk,args.sdk_version)
    if args.copy_link_stub:
        source=thin((Path(args.sdk)/'usr/lib/libSystem.dylib').read_bytes())
        info=macho(source)
        if info['subtype']&0xffffff not in (6,9): raise ValueError('unexpected SDK subtype')
        copy=bytearray(source);struct.pack_into('<I',copy,8,9)
        Path(args.copy_link_stub).write_bytes(copy)
    else: print(json.dumps(audit(args.binary,args.sdk,args.legacy),indent=2))
