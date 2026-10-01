#!/usr/bin/env python3
"""Actual rebuilt fixture negative audit gate, requires local SDK and binaries."""
import argparse
import importlib.util
from pathlib import Path
import struct
import tempfile

spec=importlib.util.spec_from_file_location('fixture_audit',Path(__file__).with_name('audit.py'))
A=importlib.util.module_from_spec(spec);spec.loader.exec_module(A)
ap=argparse.ArgumentParser();ap.add_argument('--sdk',required=True);ap.add_argument('--binary',required=True);args=ap.parse_args()
A.validate_sdk(args.sdk,'2.0');A.audit(args.binary,args.sdk,True)
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)/'bad';original=Path(args.binary).read_bytes()
    mutations=[]
    data=bytearray(original);struct.pack_into('<I',data,24,struct.unpack_from('<I',data,24)[0]|0x200000);mutations.append((data,'PIE'))
    data=bytearray(original);struct.pack_into('<I',data,8,9);mutations.append((data,'ARMv6'))
    data=bytearray(original);offset=28
    while struct.unpack_from('<I',data,offset)[0]!=5:offset+=struct.unpack_from('<I',data,offset+4)[0]
    struct.pack_into('<I',data,offset,0x80000028);mutations.append((data,'UNIXTHREAD'))
    data=bytearray(original);offset=28
    while struct.unpack_from('<I',data,offset)[0]!=2:offset+=struct.unpack_from('<I',data,offset+4)[0]
    symoff,count,stroff,strsize=struct.unpack_from('<4I',data,offset+8)
    for i in range(count):
        index,typ,sect,desc,value=struct.unpack_from('<IBBHI',data,symoff+i*12)
        if typ&1 and typ&0x0e==0 and data[stroff+index:stroff+index+7]==b'_fseek\0':
            data[stroff+index:stroff+index+6]=b'_NOPE_';break
    else:raise AssertionError('actual fixture missing expected imported _fseek')
    mutations.append((data,'imports absent'))
    for data,message in mutations:
        path.write_bytes(data)
        try:A.audit(path,args.sdk,True)
        except ValueError as error:assert message in str(error),error
        else:raise AssertionError('accepted '+message)
print('PASS actual ARMv6/legacy/import audit plus rejected PIE, subtype, entry command and absent import mutations')
