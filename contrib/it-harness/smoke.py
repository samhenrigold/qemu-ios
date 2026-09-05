#!/usr/bin/env python3
"""Run this app through the existing isolated-overlay device harness.

Accepts regress.py options, e.g. --qemu ... --base-nand ... --out /tmp/new-run.
Installs only into the disposable guest; never use an existing --out directory.
"""
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/ipod'))
import regress

original_launch = regress.check_applaunch


def exercise(cfg, procs, dev, result):
    port = regress.prepare_launcher(cfg, procs, dev, result)
    if port is None:
        return False
    shim = str(Path(__file__).parent / 'build/MBXGLEngine')
    copied = regress.guest_ssh(cfg, port, None, timeout=300, scp_from=shim, scp_to='/tmp/HarnessMBXGLEngine')
    if copied.returncode:
        return result.set(False, 'could not copy matching GLES bridge')
    copied = regress.guest_ssh(cfg, port, [
        'cp /tmp/HarnessMBXGLEngine /System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine && '
        'chmod 755 /System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine'
    ])
    if copied.returncode:
        return result.set(False, 'could not stage GLES bridge in disposable overlay')
    if not original_launch(cfg, procs, dev, result):
        return False
    port = dev.ssh_port
    try:
        for name, x, y in [('storage', 150, 163), ('memory', 150, 247), ('gl', 150, 79)]:
            dev.qmp.tap(x, y)
            time.sleep(8)
            shot = dev.qmp.shot(str(Path(dev.dir) / (name + '.ppm')))
            regress.to_png(shot, str(Path(dev.dir) / (name + '.png')))
            if name == 'gl':
                magenta, cyan = regress.quad_signature(shot)
                if min(magenta, cyan) < 0.1:
                    return result.set(False, 'GLES screenshot is missing the cyan/magenta drawable')
                dev.qmp.tap(50, 39)
                time.sleep(2)
        response = regress.guest_ssh(cfg, port, [
            "find /var/mobile/Applications -path '*/Documents/results.log' -exec cat {} \\;"
        ])
        (Path(dev.dir) / 'harness-results.log').write_text(response.stdout)
        required = ['PASS write/fsync/rename/read', 'PASS CPU/memory', 'PASS GLES framebuffer/draw/present API', 'PASS GLES pixel readback']
        if response.returncode or any(text not in response.stdout for text in required):
            return result.set(False, 'missing guest checks; inspect harness-results.log')
        return result.set(True, 'foreground launch, storage, CPU/memory, GLES API and screenshot checks')
    finally:
        response = regress.guest_ssh(cfg, port, [
            "find /var/mobile/Applications -path '*/Documents/results.log' -exec cat {} \\;"
        ])
        (Path(dev.dir) / 'harness-results.log').write_text(response.stdout)
        dev.qmp.home()
        time.sleep(2)
        if not dev.powerdown():
            result.set(False, 'guest did not shut down cleanly')


if __name__ == '__main__':
    for i, value in enumerate(sys.argv[1:], 1):
        out = sys.argv[i + 1] if value == '--out' else value.split('=', 1)[1] if value.startswith('--out=') else None
        if out and Path(out).exists():
            sys.exit('Use a new --out directory; existing overlays are never overwritten.')
    regress.check_applaunch = exercise
    sys.argv += ['--checks', 'boot,appinstall,applaunch', '--ipa', str(Path(__file__).parent / 'build/Harness.ipa')]
    sys.exit(regress.main())
