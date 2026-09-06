#!/usr/bin/env python3
"""Save halfway through a two-slice picture; restore and finish exact guest DMA."""
from pathlib import Path
import os, socket, subprocess, tempfile, time, sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'imgtools'))
from itqmp import QMP
files = ROOT.parent / 'qemu-ios-files'
with tempfile.TemporaryDirectory(prefix='it-h264-snapshot-') as temporary:
    out = Path(temporary)
    for phase in range(2):
        qpath, tpath = str(out / f'qmp{phase}'), str(out / f'qtest{phase}')
        argv = [str(ROOT / 'build-native14/qemu-build/qemu-system-arm'), '-S', '-M',
                f'iPod-Touch,bootrom={files}/bootrom_240_4,nand={files}/nand,nor={files}/nor_n72ap.bin,nandrw={out}/overlay,h264-decode=on',
                '-m', '128M', '-display', 'none', '-serial', 'null', '-monitor', 'none',
                '-qmp', f'unix:{qpath},server=on,wait=off', '-qtest', f'unix:{tpath},server=on,wait=off',
                '-qtest-log', str(out / f'qtest{phase}.log')]
        if phase:
            argv += ['-incoming', 'file:' + str(out / 'snapshot')]
        with (out / f'qemu{phase}.log').open('w') as log:
            child = subprocess.Popen(argv, stdout=log, stderr=log,
                env={k: v for k, v in os.environ.items() if not k.startswith('IT_')})
            q = t = stream = None
            try:
                deadline = time.monotonic() + 20
                while not Path(tpath).exists():
                    assert child.poll() is None and time.monotonic() < deadline, (out / f'qemu{phase}.log').read_text()
                    time.sleep(.05)
                q = QMP(qpath, timeout=10)
                t = socket.socket(socket.AF_UNIX)
                t.settimeout(10)
                t.connect(tpath)
                stream = t.makefile('rwb', buffering=0)
                deadline = time.monotonic() + 20
                while q.cmd('query-status')['status'] == 'inmigrate':
                    assert time.monotonic() < deadline
                    time.sleep(.05)
                def command(text):
                    stream.write((text + '\n').encode())
                    reply = stream.readline().decode().strip()
                    assert reply.startswith('OK'), reply
                    return reply
                def write(offset, value):
                    command(f'writel {0x38f00000 + offset:#x} {value:#x}')
                def read(offset):
                    return int(command(f'readl {0x38f00000 + offset:#x}').split()[1], 0)
                def memory(address, data):
                    for offset in range(0, len(data), 256):
                        chunk = data[offset:offset + 256]
                        command(f'write {address + offset:#x} {len(chunk)} 0x{chunk.hex()}')
                def pixels():
                    return bytes.fromhex(command('read 0x08000000 6144').split()[1].removeprefix('0x'))
                def slice_job():
                    memory(0x08008000, b'\x61\x13')  # P slice: eight skipped macroblocks.
                    write(0x1200, 0x08008000 >> 10)
                    write(0x180c, 0)
                    write(0x1810, 2)
                    write(0x1600, 0x801)
                    write(0x1000, 1)
                    assert read(0x1074) == 1, 'slice decode failed'
                if not phase:
                    for offset, value in {
                        0x1030: 4, 0x1034: 4, 0x1028: 26, 0x105c: 1, 0x100c: 1,
                        0x10d4: 2, 0x1040: 1, 0x106c: 0x0201,
                        0x1204: 0x08000000 >> 10, 0x1208: 0x08001000 >> 10,
                        0x120c: 0x08002000 >> 10, 0x1210: 0x08003000 >> 10,
                        0x1214: 0x08004000 >> 10, 0x1218: 0x08005000 >> 10,
                        0x100: 0x0403, 0x104: 0x0605,
                    }.items():
                        write(offset, value)
                    memory(0x08000000, b'\xa5' * 6144)
                    memory(0x08002000, bytes([80]) * 4096 + bytes([128]) * 2048)
                    memory(0x08004000, bytes([180]) * 4096 + bytes([128]) * 2048)
                    slice_job()
                    assert pixels() == b'\xa5' * 6144, 'partial picture published early'
                    q.cmd('migrate', uri='file:' + str(out / 'snapshot'))
                    deadline = time.monotonic() + 30
                    while True:
                        state = q.cmd('query-migrate')
                        assert state['status'] != 'failed', state
                        if state['status'] == 'completed':
                            break
                        assert time.monotonic() < deadline, state
                        time.sleep(.1)
                else:
                    assert pixels() == b'\xa5' * 6144
                    write(0x1038, 2)
                    write(0x100, 0x0605)
                    write(0x104, 0x0403)
                    slice_job()
                    assert pixels() == bytes([80]) * 2048 + bytes([180]) * 2048 + bytes([128]) * 2048
                    print('PASS: native H264 mid-picture migration, replayed references and exact completed DMA', flush=True)
                q.cmd('quit')
                assert child.wait(timeout=10) == 0
            finally:
                if stream: stream.close()
                if t: t.close()
                if q: q.close()
                if child.poll() is None:
                    child.kill()
                    child.wait()
