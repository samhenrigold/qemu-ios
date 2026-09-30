#!/usr/bin/env python3
"""LightTouchMac smoke #63, a 4.3.5 (8L1) firmware race, reproduced on demand. (8L1 addresses: a diagnostic, not a gate.)
 8L1's AppleEmbeddedUSBArbitrator::start requests power state 1 (changePowerStateToPriv(1),
805c0e7e) before it calls handleStart (805c0e8a), which maps USB_CTL into this+0x78. The PM work loop's
setPowerState(1) -> handleUSBCableTypeChange -> AppleS5L8930XUSBArbitrator's override writes USB_CTL through
this+0x78 (clearBits 809ba458). If the start thread is off the CPU in that window, PM gets there first: NULL.

Deterministic: a gdbstub breakpoint at 805c0e84 (changePowerStateToPriv returned; r0-r3 dead there) diverts the
start thread once through IOSleep(MS) and back, i.e. the thread is descheduled in the window for MS ms.

    usbarb-race.py DEVICE QEMU [MS]   DEVICE: a FirmwareKit-created k48ap-8L1; MS 0 = no diversion (control)
                                      prints the arbitrator/panic lines, then PANIC, OK or TIMEOUT

MS 50: panicked in both runs (09-30), fault_addr 0x0, pc 0x809ba45a, lr 0x809ba4e3, right after "setPowerState : calling
handleUSBCableTypeChange" (the #63 signature); MS 0: handleStart maps USB_CTL first and the boot goes on.
"""
import os, re, socket, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../imgtools'))
import ipad1_boot

BP, IOSLEEP = 0x805c0e84, 0x801cbecc   # 8L1 kernelcache


class RSP:
    def __init__(s, port):
        for _ in range(200):
            try:
                s.k = socket.create_connection(('127.0.0.1', port)); break
            except OSError:
                time.sleep(0.1)
        s.k.settimeout(None)
    def send(s, p):
        s.k.sendall(b'$%s#%02x' % (p.encode(), sum(p.encode()) & 0xff))
        return s.recv()
    def recv(s):
        buf = b''
        while True:
            c = s.k.recv(1)
            if c == b'+': continue
            if c == b'$': break
        while b'#' not in buf:
            buf += s.k.recv(1)
        s.k.recv(2); s.k.sendall(b'+')
        return buf[:-1].decode()
    def reg(s, n, v=None):
        if v is None:
            return int.from_bytes(bytes.fromhex(s.send('p%x' % n)), 'little')
        return s.send('P%x=%s' % (n, v.to_bytes(4, 'little').hex()))


def main():
    dev, qemu = sys.argv[1], sys.argv[2]
    ms = int(sys.argv[3]) if len(sys.argv) > 3 else 50
    td = tempfile.mkdtemp(prefix='r63-', dir='/tmp')
    os.mkdir(f'{td}/ov')
    cfg = type('c', (), {'device': dev})()
    machine = f'ipad1,{ipad1_boot.boot_options(cfg)},nand={dev}/nand,nand-overlay={td}/ov'
    port = 20000 + os.getpid() % 10000
    serial = f'{td}/serial.log'
    p = subprocess.Popen([qemu, '-machine', machine, '-display', 'none', '-audio', 'driver=none', '-monitor', 'none',
                          '-serial', f'file:{serial}', '-gdb', f'tcp:127.0.0.1:{port}', '-S'],
                         stdout=subprocess.DEVNULL, stderr=open(f'{td}/stderr', 'w'))
    try:
        g = RSP(port)
        g.send('?')
        if ms:
            assert g.send('Z0,%x,2' % BP) == 'OK'
            g.k.sendall(b'$c#63')
            stop = g.recv()
            print('stop', stop, 'pc', hex(g.reg(15)))
            assert g.reg(15) == BP
            g.send('z0,%x,2' % BP)
            g.reg(0, ms); g.reg(14, BP | 1); g.reg(15, IOSLEEP)
        g.k.sendall(b'$c#63')
        t0 = time.time()
        verdict = None
        while time.time() - t0 < 900 and verdict is None:
            time.sleep(2)
            t = open(serial, errors='replace').read() if os.path.exists(serial) else ''
            if 'panic(' in t or 'Debugger message' in t:
                verdict = 'PANIC'
            elif 'AppleEmbeddedUSBArbitrator::start : finished' in t and 'setPowerState : calling' in t:
                time.sleep(5); verdict = 'OK'
        t = open(serial, errors='replace').read()
        for l in t.splitlines():
            if re.search(r'USBArbitrator|USB_CTL|panic|fault_addr| pc:|lr:', l):
                print('  ', l[:200])
        print(verdict or 'TIMEOUT')
    finally:
        p.kill(); p.wait()
        subprocess.run(['rm', '-rf', td])


main()
