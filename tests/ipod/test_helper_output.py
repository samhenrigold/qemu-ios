#!/usr/bin/env python3
"""Host helper output must fail on deferred disk errors, without guest firmware."""
from pathlib import Path
import subprocess
import tempfile
import zipfile

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
    run('cc', '-Wall', '-Wextra', source, '-o', helper)
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
    run('cc', work / 'helper.o', shim, '-o', failing)

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

print('PASS: ipa-chmod, deferred output errors')
