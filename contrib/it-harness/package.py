#!/usr/bin/env python3
"""Package and check the actual ARMv6/iOS 3.1.3 app, using only stdlib."""
import plistlib
from pathlib import Path
import struct
import sys
import zipfile

ASSETS = ('stereo.wav', 'aac.m4a', 'tone.mp3', 'lossless.m4a', 'h264.mp4', 'mpeg4.mp4')


def check(ipa):
    with zipfile.ZipFile(ipa) as archive:
        prefix = 'Payload/Harness.app/'
        info = plistlib.loads(archive.read(prefix + 'Info.plist'))
        assert info['CFBundleIdentifier'] == 'com.qemuios.harness'
        assert info['DTSDKName'] == 'iphoneos3.1.3'
        binary = archive.read(prefix + 'Harness')
        magic, cpu, subtype, kind, count, size, flags = struct.unpack_from('<7I', binary)
        assert (magic, cpu, subtype, kind) == (0xfeedface, 12, 6, 2), 'not ARMv6 executable'
        offset = 28
        commands = []
        for _ in range(count):
            cmd, length = struct.unpack_from('<II', binary, offset)
            assert length >= 8 and offset + length <= 28 + size
            commands.append(cmd)
            if cmd == 0x21:
                assert struct.unpack_from('<I', binary, offset + 16)[0] == 0, 'encrypted'
            offset += length
        assert offset == 28 + size
        assert 5 in commands and 0x80000028 not in commands, 'requires legacy LC_UNIXTHREAD'
        assert 0x1d in commands, 'missing code signature'
        assert archive.getinfo(prefix + 'Harness').external_attr >> 16 & 0o111, 'not executable'
        for asset in ASSETS:
            assert len(archive.read(prefix + asset)) > 1000, f'missing/empty fixture {asset}'
    print('PASS: IPA metadata, ARMv6 entry point, signature command, executable mode, six media fixtures')


def package(output):
    output = Path(output)
    app = output / 'Payload/Harness.app'
    info = dict(CFBundleDisplayName='Test Harness', CFBundleName='Harness',
                CFBundleExecutable='Harness', CFBundleIdentifier='com.qemuios.harness',
                CFBundleInfoDictionaryVersion='6.0', CFBundlePackageType='APPL',
                CFBundleVersion='1.0', CFBundleSupportedPlatforms=['iPhoneOS'],
                DTPlatformName='iphoneos', DTSDKName='iphoneos3.1.3',
                MinimumOSVersion='3.1', LSRequiresIPhoneOS=True,
                UIStatusBarHidden=False)
    # Metadata is generated in the disposable build directory, not source files.
    with (app / 'Info.plist').open('wb') as stream:
        plistlib.dump(info, stream)
    with zipfile.ZipFile(output / 'Harness.ipa', 'w', zipfile.ZIP_DEFLATED) as archive:
        for name in ('Harness', 'Info.plist', *ASSETS):
            archive.write(app / name, 'Payload/Harness.app/' + name)


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--check':
        check(sys.argv[2])
    elif len(sys.argv) == 2:
        package(sys.argv[1])
    else:
        sys.exit('usage: package.py BUILD_DIR | --check IPA')
