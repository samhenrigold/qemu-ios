#!/usr/bin/env python3
"""
A GDB-remote shim that lets a modern lldb drive the 2009 armv6 debugserver on
the emulated iPod touch 2G (iOS 3.1.3).

Why it exists
-------------
The debugserver from the 3.1.3 DeveloperDiskImage answers an EMPTY packet to
everything modern lldb opens a session with -- qSupported, qHostInfo,
qProcessInfo, qRegisterInfo, vCont?, qVAttachOrWaitSupported -- and answers
"OK" (not a thread list) to qfThreadInfo.  Modern lldb has no architecture,
pointer size, byte order, thread list or register layout to work from, and
hangs.  Measured: a naive `gdb-remote` + `process attach` never returns.

This process sits between them.  It answers those packets synthetically and
passes everything else through unchanged over ONE persistent device connection
-- which also avoids the operational trap that many short-lived connections
wedge the emulated USB link.

The register layout below is the real one, derived from a live `g` reply
(300 bytes) plus the register numbers that appear in a live stop reply
(00-0f and 19).  It is the classic GDB ARM numbering, not lldb's:

    byte   0..63    r0..r15           (gdb regnum 0..15)
    byte  64..159   f0..f7            legacy FPA, 12 bytes each (16..23)
    byte 160..163   fps               (24)
    byte 164..167   cpsr              (25)
    byte 168..295   s0..s31           VFP (26..57)
    byte 296..299   fpscr             (58)

Usage
-----
    idevicedebugserverproxy <P>                 # lockdownd spawns debugserver
    ./lldb_shim.py --device-port <P> --listen <L>
    lldb -o "target create --arch armv6 <binary>" \
         -o "gdb-remote 127.0.0.1:<L>" -o "process attach --pid <pid>"
"""
import argparse
import socket

CPUTYPE = 12     # CPU_TYPE_ARM
CPUSUBTYPE = 6   # CPU_SUBTYPE_ARM_V6


def build_reginfo():
    regs = []

    def add(name, bitsize, offset, gdbnum, group, fmt='hex', enc='uint', generic=None):
        s = ('name:%s;bitsize:%d;offset:%d;encoding:%s;format:%s;set:%s;'
             'gcc:%d;dwarf:%d;' % (name, bitsize, offset, enc, fmt, group, gdbnum, gdbnum))
        if generic:
            s += 'generic:%s;' % generic
        regs.append(s)

    G = 'General Purpose Registers'
    F = 'Floating Point Registers'
    for i in range(13):
        add('r%d' % i, 32, i * 4, i, G,
            generic=('arg%d' % (i + 1)) if i < 4 else None)
    add('sp', 32, 52, 13, G, generic='sp')
    add('lr', 32, 56, 14, G, generic='ra')
    add('pc', 32, 60, 15, G, generic='pc')
    for i in range(8):                       # legacy FPA registers, 12 bytes each
        add('f%d' % i, 96, 64 + i * 12, 16 + i, F, fmt='vector-uint8', enc='vector')
    add('fps', 32, 160, 24, F)
    add('cpsr', 32, 164, 25, G, generic='flags')
    for i in range(32):                      # VFP single-precision
        add('s%d' % i, 32, 168 + i * 4, 26 + i, F, fmt='float', enc='ieee754')
    add('fpscr', 32, 296, 58, F)
    return regs


REGINFO = build_reginfo()


def csum(body):
    c = 0
    for ch in body:
        c = (c + ord(ch)) & 0xff
    return '$%s#%02x' % (body, c)


class Link:
    def __init__(self, sock):
        self.s = sock
        self.buf = b''
        self.auto_ack = False

    def read_packet(self, timeout=None):
        self.s.settimeout(timeout)
        while True:
            while self.buf[:1] in (b'+', b'-'):
                self.buf = self.buf[1:]
            if self.buf[:1] == b'\x03':
                self.buf = self.buf[1:]
                return '\x03'
            if self.buf.startswith(b'$'):
                i = self.buf.find(b'#')
                if i >= 0 and len(self.buf) >= i + 3:
                    body = self.buf[1:i].decode('latin-1')
                    self.buf = self.buf[i + 3:]
                    if self.auto_ack:
                        self.s.sendall(b'+')
                    return body
            d = self.s.recv(65536)
            if not d:
                raise EOFError('link closed')
            self.buf += d

    def send_packet(self, body):
        self.s.sendall(csum(body).encode('latin-1'))

    def ack(self):
        self.s.sendall(b'+')


def parse_stop(reply):
    """Pull the thread id out of a T stop reply, e.g. T11...;thread:2c03;..."""
    if reply and reply[:1] in ('T', 'S'):
        for f in reply[3:].split(';'):
            if f.startswith('thread:'):
                return f[len('thread:'):]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--device-port', type=int, required=True)
    ap.add_argument('--listen', type=int, default=0)
    ap.add_argument('--attach-pid', type=int, default=None)
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()

    # NOTE: do NOT put the device side into no-ack mode.  This debugserver
    # replies OK to QStartNoAckMode and then still waits for a '+' before it
    # will process the next packet -- measured: the very next packet (qC) never
    # gets an answer.  Keep acking it, always.
    dev = Link(socket.create_connection(('127.0.0.1', a.device_port), 30))
    dev.auto_ack = True

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', a.listen))
    srv.listen(1)
    print('shim: listening on 127.0.0.1:%d, device on :%d'
          % (srv.getsockname()[1], a.device_port), flush=True)

    state = {'pid': None, 'tid': None, 'stop': None}

    def to_dev(p, timeout=300):
        dev.send_packet(p)
        return dev.read_packet(timeout)

    if a.attach_pid is not None:
        r = to_dev('vAttach;%x' % a.attach_pid)
        print('shim: vAttach;%x -> %s' % (a.attach_pid, r), flush=True)
        if r and not r.startswith('E'):
            state['pid'] = a.attach_pid
            state['tid'] = parse_stop(r)
            state['stop'] = r

    cli_sock, _ = srv.accept()
    cli = Link(cli_sock)
    noack_cli = False

    while True:
        try:
            p = cli.read_packet()
        except (EOFError, OSError):
            break
        if a.verbose:
            print('lldb -> %s' % p[:140], flush=True)
        if not noack_cli:
            cli.ack()

        def reply(body):
            if a.verbose:
                print('     <- %s' % body[:140], flush=True)
            cli.send_packet(body)

        if p == '\x03':
            dev.s.sendall(b'\x03')
            reply(dev.read_packet(300) or '')
            continue
        if p.startswith('QStartNoAckMode'):
            reply('OK')
            noack_cli = True
            continue
        if p.startswith('qSupported'):
            reply('PacketSize=1000;QStartNoAckMode+')
            continue
        if p == 'qHostInfo':
            reply('cputype:%d;cpusubtype:%d;ostype:ios;vendor:apple;endian:little;'
                  'ptrsize:4;watchpoint_exceptions_received:before;'
                  % (CPUTYPE, CPUSUBTYPE))
            continue
        if p == 'qProcessInfo':
            if state['pid'] is None:
                reply('E01')
            else:
                reply('pid:%x;parent-pid:1;real-uid:1f5;real-gid:1f5;effective-uid:1f5;'
                      'effective-gid:1f5;cputype:%x;cpusubtype:%x;ostype:ios;vendor:apple;'
                      'endian:little;ptrsize:4;' % (state['pid'], CPUTYPE, CPUSUBTYPE))
            continue
        if p.startswith('qRegisterInfo'):
            try:
                n = int(p[len('qRegisterInfo'):], 16)
            except ValueError:
                reply('E45')
                continue
            reply(REGINFO[n] if n < len(REGINFO) else 'E45')
            continue
        if p in ('qfThreadInfo', 'qThreadInfo'):
            # the device answers "OK" here, which lldb cannot parse
            reply(('m%s' % state['tid']) if state['tid'] else 'l')
            continue
        if p == 'qsThreadInfo':
            reply('l')
            continue
        if p.startswith('qThreadStopInfo'):
            reply(state['stop'] or '')
            continue
        if p.startswith('vAttach'):
            try:
                state['pid'] = int(p.split(';')[1], 16)
            except (IndexError, ValueError):
                pass
            r = to_dev(p)
            if r and r[:1] in 'TS':
                state['tid'] = parse_stop(r)
                state['stop'] = r
            elif r and r.startswith('E'):
                state['pid'] = None
            reply(r or '')
            continue
        if p[:1] == 'H' and p[1:].endswith('-1') and state['tid']:
            # lldb says "all threads" as Hc-1 / Hg-1; this debugserver then
            # fails the following 's' with E33.  Name the thread explicitly.
            newp = p[:2] + state['tid']
            print('shim: rewrote %s -> %s' % (p, newp), flush=True)
            p = newp
        if p == '?':
            reply(state['stop'] if state['stop'] else (to_dev(p) or ''))
            continue
        # Packets a 2009 debugserver has never heard of: answered here so lldb
        # does not pay a device round trip for each one.
        if p.split(':')[0] in ('jGetLoadedDynamicLibrariesInfos', 'qXfer',
                               'QThreadSuffixSupported', 'QListThreadsInStopReply',
                               'qGDBServerVersion', 'QEnableErrorStrings',
                               'jThreadsInfo', 'qVAttachOrWaitSupported',
                               'QSetDetachOnError', 'qEcho', 'vCont?',
                               'qMemoryRegionInfo', 'qSymbol', 'qWatchpointSupportInfo',
                               'QSetSTDIN', 'QSetSTDOUT', 'QSetSTDERR',
                               'QSetDisableASLR', 'QSetWorkingDir', 'QLaunchArch'):
            reply('')
            continue
        try:
            r = to_dev(p)
        except (EOFError, OSError) as e:
            print('shim: device link lost (%s)' % e, flush=True)
            break
        if r and r[:1] in 'TS' and p[:1] in 'cCsSv':
            state['tid'] = parse_stop(r) or state['tid']
            state['stop'] = r
        reply(r if r is not None else '')

    print('shim: client gone', flush=True)


main()
