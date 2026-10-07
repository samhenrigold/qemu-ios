#!/usr/bin/env python3
"""Package and check the actual ARMv6/iOS 3.1.3 app, using only stdlib."""
import argparse
import plistlib
from pathlib import Path
from audit import macho
import zipfile

ASSETS = ('stereo.wav', 'aac.m4a', 'tone.mp3', 'lossless.m4a', 'h264.mp4', 'mpeg4.mp4')


def check(ipa):
    with zipfile.ZipFile(ipa) as archive:
        prefix = 'Payload/Harness.app/'
        info = plistlib.loads(archive.read(prefix + 'Info.plist'))
        assert info['CFBundleIdentifier'] == 'com.qemuios.harness'
        assert (info['DTSDKName'],info['MinimumOSVersion']) in [('iphoneos3.1.3','3.1'),('iphoneos2.0','2.0')]
        binary = archive.read(prefix + 'Harness')
        image=macho(binary)
        assert (image['cpu'],image['subtype'],image['kind']) == (12,6,2), 'not ARMv6 executable'
        assert not image['encrypted'], 'encrypted'
        assert 5 in image['commands'] and 0x80000028 not in image['commands'], 'requires legacy LC_UNIXTHREAD'
        assert 0x1d in image['commands'], 'missing code signature'
        assert archive.getinfo(prefix + 'Harness').external_attr >> 16 & 0o111, 'not executable'
        for asset in ASSETS:
            assert len(archive.read(prefix + asset)) > 1000, f'missing/empty fixture {asset}'
    print('PASS: IPA metadata, ARMv6 entry point, signature command, executable mode, six media fixtures')


def package(output, flavor="full"):
    output = Path(output)
    app = output / 'Payload/Harness.app'
    info = dict(CFBundleDisplayName='Test Harness', CFBundleName='Harness',
                CFBundleExecutable='Harness', CFBundleIdentifier='com.qemuios.harness',
                CFBundleInfoDictionaryVersion='6.0', CFBundlePackageType='APPL',
                CFBundleVersion='1.0', CFBundleSupportedPlatforms=['iPhoneOS'],
                DTPlatformName='iphoneos', DTSDKName='iphoneos2.0' if flavor=='ios2' else 'iphoneos3.1.3',
                MinimumOSVersion='2.0' if flavor=='ios2' else '3.1', LSRequiresIPhoneOS=True,
                UIStatusBarHidden=False)
    # Metadata is generated in the disposable build directory, not source files.
    with (app / 'Info.plist').open('wb') as stream:
        plistlib.dump(info, stream)
    with zipfile.ZipFile(output / 'Harness.ipa', 'w', zipfile.ZIP_DEFLATED) as archive:
        for name in ('Harness', 'Info.plist', *ASSETS):
            archive.write(app / name, 'Payload/Harness.app/' + name)


if __name__ == '__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--flavor',choices=['full','ios2'],default='full')
    ap.add_argument('--check',action='store_true');ap.add_argument('output');args=ap.parse_args()
    if args.check: check(args.output)
    else: package(args.output,args.flavor)
