#!/usr/bin/env python3
"""Host helper output must fail on deferred disk errors, without guest firmware."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[2]


def run(*args, ok=True, **kwargs):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True, **kwargs)
    if ok:
        assert result.returncode == 0, result.stderr
    else:
        assert result.returncode != 0 and 'cannot finish' in result.stderr, result
    return result


with tempfile.TemporaryDirectory(prefix='ltm-helper-output-') as temporary:
    work = Path(temporary)
    source = ROOT / 'contrib/macos-app/ipod-helper.c'
    helper, failing = work / 'helper', work / 'full-disk-helper'
    run('cc', '-Wall', '-Wextra', source, '-lz', '-o', helper)
    # Inject only the OS's deferred close error, without exhausting real disk.
    shim = work / 'close.c'
    shim.write_text('''#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
int test_fclose(FILE *stream) {
    int writing = (fcntl(fileno(stream), F_GETFL) & O_ACCMODE) != O_RDONLY;
    int result = fclose(stream);
    if (writing) { errno = ENOSPC; return EOF; }
    return result;
}
''')
    run('cc', '-Dfclose=test_fclose', '-c', source, '-o', work / 'helper.o')
    run('cc', work / 'helper.o', shim, '-lz', '-o', failing)

    page = b'fixture' + bytes(4160 - 7)
    manifest = zlib.compress(struct.pack('<BI', 0, 1))
    packed = work / 'fixture.itnand'
    packed.write_bytes(b'ITNANDP1' + struct.pack('<II', 1, len(manifest)) + manifest + zlib.compress(page))
    good_pages, bad_pages = work / 'pages', work / 'bad-pages'
    good_pages.mkdir()
    bad_pages.mkdir()
    run(helper, 'nand-unpack', packed, good_pages)
    assert (good_pages / 'cs0/1.page').read_bytes() == page
    run(failing, 'nand-unpack', packed, bad_pages, ok=False)
    assert not (bad_pages / 'cs0/1.page').exists()

    ipa = work / 'fixture.ipa'
    member = 'Payload/Fixture.app/Fixture'
    with zipfile.ZipFile(ipa, 'w') as archive:
        archive.writestr(member, b'fixture')
    output = work / 'prepared.ipa'
    run(helper, 'ipa-chmod', ipa, output, member)
    with zipfile.ZipFile(output) as archive:
        assert archive.getinfo(member).external_attr >> 16 == 0o100755
    run(failing, 'ipa-chmod', ipa, output, member, ok=False)
    assert not output.exists()

    data, blob = work / 'input', work / 'fixture.blob'
    data.write_bytes(b'authored fixture')
    run(helper, 'blob-pack', blob, f'fixture={data}')
    unpacked = work / 'unpacked'
    run(helper, 'blob-unpack', blob, unpacked)
    assert (unpacked / 'fixture').read_bytes() == data.read_bytes()
    run(failing, 'blob-unpack', blob, work / 'bad-unpacked', ok=False)
    assert not (work / 'bad-unpacked/fixture').exists()
    run(failing, 'blob-pack', work / 'bad.blob', f'fixture={data}', ok=False)
    assert not (work / 'bad.blob').exists()

    # A failed Terminal launch must leave no command directory behind.
    binaries, scratch = work / 'bin', work / 'scratch'
    binaries.mkdir()
    scratch.mkdir()
    for name, body in {'iproxy': 'exit 0', 'idevice_id': 'echo fixture', 'open': 'exit 7'}.items():
        path = binaries / name
        path.write_text('#!/bin/sh\n' + body + '\n')
        path.chmod(0o755)
    env = os.environ | {'PATH': str(binaries) + ':' + os.environ['PATH'],
                        'TMPDIR': str(scratch) + '/', 'DEVICE_PASSWORD': 'fixture',
                        'USBMUXD_SOCKET_ADDRESS': '127.0.0.1:1'}
    result = subprocess.run(['bash', ROOT / 'contrib/it-ssh-terminal.sh'], env=env,
                            capture_output=True, text=True)
    assert result.returncode == 7, result
    assert list(scratch.iterdir()) == [], list(scratch.iterdir())

print('PASS: helper round trips, deferred output errors, failed Terminal cleanup')
