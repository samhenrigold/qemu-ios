#!/usr/bin/env python3
"""Opt-in 7E18 metadata/artwork import, retry and cold-boot proof in a fresh overlay.

--staging is a directory prepared by the production MediaSong code, containing
metadata.plist, audio.m4a and artwork.jpg. --probe is media_artwork_probe.c built
with contrib/armv6-toolchain and ad-hoc signed like other guest test executables.
"""
import argparse
import json
import plistlib
import shutil
import sqlite3
import time
from pathlib import Path
from types import SimpleNamespace
import regress as r

ap = argparse.ArgumentParser(description=__doc__)
for name in ('device', 'qemu', 'usbmuxd', 'guest-package', 'staging', 'probe', 'out', 'files'):
    ap.add_argument('--' + name, required=True)
args = ap.parse_args()
base, staging, out = map(Path, (args.device, args.staging, args.out))
out.mkdir()
(out / 'overlay').mkdir()
shutil.copyfile(base / 'nor.bin', out / 'nor.bin')
metadata = plistlib.loads((staging / 'metadata.plist').read_bytes())
assert metadata['filename'] == 'audio.m4a' and metadata['artwork'] == 'artwork.jpg'
cfg = SimpleNamespace(files=args.files, device=str(base), base_nand=str(base / 'nand'),
    nor=str(out / 'nor.bin'), overlay=str(out / 'overlay'), direct_iboot=str(base / 'iBoot.bin'),
    gid_blobs=str(base / 'gid-blobs.bin'), guest_package=args.guest_package, qemu=args.qemu,
    usbmuxd=args.usbmuxd, out=str(out), board='n72ap', wifi=False, cpu=None, mem='128M',
    device_machine={'aes-uid': 'engine'}, device_version=None, usb_port=r.free_port(21400, 21500),
    mux_port=r.free_port(27600, 27700), qmp_port=r.free_port(29300, 29400),
    amc_mode='decode', home_lit_min=10000, home_lit_max=145000)
r.START = time.time()
procs = r.Procs()
helper = Path(__file__).resolve().parents[2] / 'contrib/it-media/itmedia'
try:
    for boot in ('import', 'reboot'):
        device = r.Device(cfg, procs, boot)
        device.start()
        ok, detail, _ = device.wait_for_home(240)
        assert ok, detail
        control = r.prepare_app_control(cfg, procs, device, r.Result('artwork control'))
        ok, detail = r.unlock(cfg, control, device)
        assert ok, detail

        def rpc(op, argument='', data=b''):
            status, result = r.itqmp.agent(device.qmp, op, argument, data)
            assert status == 0, (op, argument, status, result)
            return result

        rpc('put', '/tmp/artprobe 755', Path(args.probe).read_bytes())
        rpc('put', '/tmp/itmedia 755', helper.read_bytes())
        rpc('put', '/tmp/metadata.plist 644', (staging / 'metadata.plist').read_bytes())
        if boot == 'import':
            rpc('exec', '/tmp/artprobe --setup')
            for name in ('audio.m4a', 'artwork.jpg'):
                rpc('put', '/var/mobile/Media/LightTouch/artwork-fixture/' + name + ' 644',
                    (staging / name).read_bytes())
            assert rpc('exec', '/tmp/itmedia /tmp/metadata.plist artwork-fixture').splitlines()[-1:] == [b'imported']
        assert rpc('exec', '/tmp/itmedia /tmp/metadata.plist artwork-fixture').splitlines()[-1:] == [b'already-imported']
        db_path = out / (boot + '.itdb')
        db_path.write_bytes(rpc('get', '/var/mobile/Media/iTunes_Control/iTunes/iTunes Library.itlp/Library.itdb'))
        with sqlite3.connect(db_path) as db:
            rows = db.execute('SELECT title,artist,album,album_artist,composer,track_number,track_count,'
                              'disc_number,disc_count,artwork_cache_id FROM item').fetchall()
            extra = db.execute('SELECT year,is_compilation,(SELECT genre FROM genre_map WHERE id=item.genre_id) FROM item').fetchall()
        for i, key in enumerate(('year', 'compilation', 'genre')):
            if key in metadata:
                assert len(extra) == 1 and extra[0][i] == metadata[key], (key, extra)
        expected = tuple(metadata[key] for key in ('title', 'artist', 'album', 'album_artist', 'composer',
                         'track_number', 'track_count', 'disc_number', 'disc_count'))
        assert len(rows) == 1 and rows[0][:-1] == expected and rows[0][-1] > 0, rows
        rpc('launch', 'com.apple.mobileipod')
        time.sleep(6)
        result = rpc('exec', '/tmp/artprobe')
        (out / (boot + '-probe.txt')).write_bytes(result)
        assert b'songs=1' in result and b'image=0x0' not in result, result
        image = rpc('get', '/tmp/guest-artwork.png')
        assert len(image) > 100 and image.startswith(b'\x89PNG\r\n\x1a\n')
        (out / (boot + '-artwork.png')).write_bytes(image)
        (out / (boot + '.json')).write_text(json.dumps({'rows': rows, 'extra_tags': extra}, indent=2))
        print('PASS:', boot, 'native tags, decoded artwork and one song after repeat import', flush=True)
        # Reopening after a hard stop also exercises persistent cache publication.
        procs.stop_all()
        device.qmp.close()
finally:
    procs.stop_all()
