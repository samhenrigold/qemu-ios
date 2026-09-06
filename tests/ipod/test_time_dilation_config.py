#!/usr/bin/env python3
"""Native QOM configuration checks; each paused machine owns a temporary overlay."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'imgtools'))
from itqmp import QMP

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--qemu', type=Path, default=ROOT / 'build-native14/qemu-build/qemu-system-arm')
parser.add_argument('--files', type=Path, default=ROOT.parent / 'qemu-ios-files')
args = parser.parse_args()
base_env = {k: v for k, v in os.environ.items() if not k.startswith('IT_')}
iboot = str(args.files / 'ios3/iBoot.bin')
cases = [
    ('boot-timer-default', {}, '', {'boot-args-delay-ms':2000,'boot-args-repeat':24,'boot-args-interval-ms':500}),
    ('boot-timer-aliases', {'IT_BOOT_ARGS_DELAY_MS':'1500','IT_BOOT_ARGS_REPEAT':'200','IT_BOOT_ARGS_INTERVAL_MS':'0xfa'}, '', {'boot-args-delay-ms':1500,'boot-args-repeat':200,'boot-args-interval-ms':250}),
    ('boot-args-delay-ms-explicit', {'IT_BOOT_ARGS_DELAY_MS':'bad'}, ',boot-args-delay-ms=0', {'boot-args-delay-ms':0}),
    ('boot-args-delay-ms-maximum', {}, ',boot-args-delay-ms=3600000', {'boot-args-delay-ms':3600000}),
    ('boot-args-delay-ms-overflow', {}, ',boot-args-delay-ms=3600001', None),
    ('boot-args-delay-ms-bad--1', {'IT_BOOT_ARGS_DELAY_MS':'-1'}, '', None),
    ('boot-args-delay-ms-bad-', {'IT_BOOT_ARGS_DELAY_MS':''}, '', None),
    ('boot-args-delay-ms-bad-100junk', {'IT_BOOT_ARGS_DELAY_MS':'100junk'}, '', None),
    ('boot-args-delay-ms-bad-18446744073709551616', {'IT_BOOT_ARGS_DELAY_MS':'18446744073709551616'}, '', None),
    ('boot-args-repeat-explicit', {'IT_BOOT_ARGS_REPEAT':'bad'}, ',boot-args-repeat=0', {'boot-args-repeat':0}),
    ('boot-args-repeat-maximum', {}, ',boot-args-repeat=1000000', {'boot-args-repeat':1000000}),
    ('boot-args-repeat-overflow', {}, ',boot-args-repeat=1000001', None),
    ('boot-args-repeat-bad--1', {'IT_BOOT_ARGS_REPEAT':'-1'}, '', None),
    ('boot-args-repeat-bad-', {'IT_BOOT_ARGS_REPEAT':''}, '', None),
    ('boot-args-repeat-bad-100junk', {'IT_BOOT_ARGS_REPEAT':'100junk'}, '', None),
    ('boot-args-repeat-bad-18446744073709551616', {'IT_BOOT_ARGS_REPEAT':'18446744073709551616'}, '', None),
    ('boot-args-interval-ms-explicit', {'IT_BOOT_ARGS_INTERVAL_MS':'bad'}, ',boot-args-interval-ms=1', {'boot-args-interval-ms':1}),
    ('boot-args-interval-ms-maximum', {}, ',boot-args-interval-ms=3600000', {'boot-args-interval-ms':3600000}),
    ('boot-args-interval-ms-overflow', {}, ',boot-args-interval-ms=3600001', None),
    ('boot-args-interval-ms-bad--1', {'IT_BOOT_ARGS_INTERVAL_MS':'-1'}, '', None),
    ('boot-args-interval-ms-bad-', {'IT_BOOT_ARGS_INTERVAL_MS':''}, '', None),
    ('boot-args-interval-ms-bad-100junk', {'IT_BOOT_ARGS_INTERVAL_MS':'100junk'}, '', None),
    ('boot-args-interval-ms-bad-18446744073709551616', {'IT_BOOT_ARGS_INTERVAL_MS':'18446744073709551616'}, '', None),
    ('boot-args-interval-ms-bad-0', {'IT_BOOT_ARGS_INTERVAL_MS':'0'}, '', None),
    ('boot-args-default', {}, '', {'boot-args':''}),
    ('boot-args-explicit', {'IT_BOOT_ARGS':'legacy'}, ',boot-args=-v serial=3', {'boot-args':'-v serial=3'}),
    ('boot-args-empty', {'IT_BOOT_ARGS':'legacy'}, ',boot-args=', {'boot-args':''}),
    ('boot-args-maximum', {}, ',boot-args='+'x'*255, {'boot-args':'x'*255}),
    ('boot-args-overflow', {}, ',boot-args='+'x'*256, None),
    ('direct-default', {}, '', {'direct-iboot':'', 'direct-llb':''}),
    ('direct-alias', {'IT_DIRECT_IBOOT':iboot}, '', {'direct-iboot':iboot}),
    ('direct-explicit', {'IT_DIRECT_IBOOT':'/missing'}, ',direct-iboot='+iboot, {'direct-iboot':iboot}),
    ('direct-empty', {'IT_DIRECT_IBOOT':'/missing'}, ',direct-iboot=', {'direct-iboot':''}),
    ('direct-llb-alone', {}, ',direct-llb=/missing', None),
    ('forge-default', {}, '', {'forge-sigcheck':False}),
    ('forge-alias', {'IT_FORGE_SIGCHECK':'0'}, '', {'forge-sigcheck':True}),
    ('forge-on', {}, ',forge-sigcheck=on', {'forge-sigcheck':True}),
    ('forge-off', {'IT_FORGE_SIGCHECK':'1'}, ',forge-sigcheck=off', {'forge-sigcheck':False}),
    ('lcd-default', {}, '', {'lcd-planes':False}),
    ('lcd-alias', {'IT_LCD_PLANES':'0'}, '', {'lcd-planes':True}),
    ('lcd-on', {}, ',lcd-planes=on', {'lcd-planes':True}),
    ('lcd-off', {'IT_LCD_PLANES':'1'}, ',lcd-planes=off', {'lcd-planes':False}),
    ('amc-default', {}, ',audio-hw=on', {'amc-mode':'registers'}),
    ('amc-state-alias', {'IT_AMC_STATE':'0'}, ',audio-hw=on', {'amc-mode':'handshake'}),
    ('amc-aac-alias', {'IT_AMC_AAC':'0'}, ',audio-hw=on', {'amc-mode':'decode'}),
    ('amc-decode-alias', {'IT_AMC_DECODE':'0','IT_AMC_STATE':'1'}, ',audio-hw=on', {'amc-mode':'decode'}),
    ('amc-registers', {'IT_AMC_DECODE':'1','IT_AMC_STATE':'1'}, ',audio-hw=on,amc-mode=registers', {'amc-mode':'registers'}),
    ('amc-handshake', {'IT_AMC_DECODE':'1'}, ',audio-hw=on,amc-mode=handshake', {'amc-mode':'handshake'}),
    ('amc-decode', {}, ',audio-hw=on,amc-mode=decode', {'amc-mode':'decode'}),
    ('amc-invalid', {}, ',amc-mode=invalid', None),
    ('mpvd-default', {}, '', {'mpvd-decode':False}),
    ('mpvd-alias', {'IT_MPVD_DECODE':'0'}, '', {'mpvd-decode':True}),
    ('mpvd-on', {}, ',mpvd-decode=on', {'mpvd-decode':True}),
    ('mpvd-off', {'IT_MPVD_DECODE':'1'}, ',mpvd-decode=off', {'mpvd-decode':False}),
    ('scaler-default', {}, '', {'scaler-decode':False}),
    ('scaler-alias', {'IT_SCALER_DECODE':'0'}, '', {'scaler-decode':True}),
    ('scaler-on', {}, ',scaler-decode=on', {'scaler-decode':True}),
    ('scaler-off', {'IT_SCALER_DECODE':'1'}, ',scaler-decode=off', {'scaler-decode':False}),
    ('h264-default', {}, '', {'h264-decode':False}),
    ('h264-alias', {'IT_H264_DECODE':'0'}, '', {'h264-decode':True}),
    ('h264-on', {}, ',h264-decode=on', {'h264-decode':True}),
    ('h264-off', {'IT_H264_DECODE':'1'}, ',h264-decode=off', {'h264-decode':False}),
    ('default', {}, '', {'time-dilation':1}),
    ('alias', {'IT_TIME_DILATION':'0x2'}, '', {'time-dilation':2}),
    ('explicit', {'IT_TIME_DILATION':'bad'}, ',time-dilation=3', {'time-dilation':3}),
    ('maximum', {}, ',time-dilation=1000000', {'time-dilation':1000000}),
    ('zero', {'IT_TIME_DILATION':'0'}, '', None),
    ('negative', {'IT_TIME_DILATION':'-1'}, '', None),
    ('overflow', {'IT_TIME_DILATION':'18446744073709551616'}, '', None),
    ('empty', {'IT_TIME_DILATION':''}, '', None),
    ('junk', {'IT_TIME_DILATION':'2junk'}, '', None),
    ('invalid-property', {}, ',time-dilation=1000001', None),
]
for label, overrides, options, expected in cases:
    with tempfile.TemporaryDirectory(prefix='it-time-config-') as tmp:
        out = Path(tmp)
        sock = str(out / 'qmp')
        machine = (f'iPod-Touch,bootrom={args.files}/bootrom_240_4,nand={args.files}/nand,'
                   f'nor={args.files}/nor_n72ap.bin,nandrw={out}/overlay' + options)
        with (out / 'qemu.log').open('w+') as log:
            child = subprocess.Popen([str(args.qemu), '-S', '-M', machine, '-m', '128M',
                '-display', 'none', '-serial', 'null', '-monitor', 'none',
                '-qmp', f'unix:{sock},server=on,wait=off'],
                stdout=log, stderr=subprocess.STDOUT, env=base_env | overrides)
            q = None
            try:
                if expected is None:
                    assert child.wait(timeout=15) != 0, label
                    log.seek(0)
                    message = log.read()
                    assert ('IT_TIME_DILATION must be' in message or
                            'time-dilation' in message or 'amc-mode' in message or 'direct-llb' in message or 'boot-args' in message or 'IT_BOOT_ARGS_' in message), message
                else:
                    deadline = time.monotonic() + 15
                    while not Path(sock).exists():
                        assert child.poll() is None and time.monotonic() < deadline, label
                        time.sleep(.05)
                    q = QMP(sock, timeout=10)
                    for prop, value in expected.items():
                        assert q.cmd('qom-get', path='/machine', property=prop) == value, label
                        if prop == 'direct-iboot':
                            devices=q.cmd('qom-list',path='/machine/unattached')
                            uarts=[d for d in devices if 'exynos4210.uart' in d['type']]
                            assert len(uarts)==4, devices
                            for uart in uarts:
                                assert q.cmd('qom-get',path='/machine/unattached/'+uart['name'],property='s5l8720-irq')==bool(value)
                        if prop == 'forge-sigcheck':
                            devices=q.cmd('qom-list',path='/machine/unattached')
                            device=next(d for d in devices if d['type']=='child<ipodtouch.pke>')
                            path='/machine/unattached/'+device['name']
                            assert q.cmd('qom-get',path=path,property='forge-sigcheck')==value
                        if prop == 'lcd-planes':
                            devices=q.cmd('qom-list',path='/machine/unattached')
                            device=next(d for d in devices if d['type']=='child<ipodtouch.lcd>')
                            path='/machine/unattached/'+device['name']
                            assert q.cmd('qom-get',path=path,property='planes')==value
                        if prop == 'amc-mode':
                            devices=q.cmd('qom-list',path='/machine/unattached')
                            device=next(d for d in devices if d['type']=='child<ipodtouch.amc>')
                            path='/machine/unattached/'+device['name']
                            assert q.cmd('qom-get',path=path,property='mode')==['registers','handshake','decode'].index(value)
                        if prop == 'mpvd-decode':
                            devices=q.cmd('qom-list',path='/machine/unattached')
                            device=next(d for d in devices if d['type']=='child<ipodtouch.mpvd>')
                            path='/machine/unattached/'+device['name']
                            assert q.cmd('qom-get',path=path,property='decode')==value
                        if prop in ('h264-decode','scaler-decode'):
                            devices=q.cmd('qom-list',path='/machine/unattached')
                            assert any(d['type']==f"child<ipodtouch.{prop.removesuffix('-decode')}>" for d in devices)==value, devices
                        try:
                            q.cmd('qom-set', path='/machine', property=prop, value=value)
                        except Exception as error:
                            assert 'before the machine starts' in str(error), str(error)
                        else:
                            raise AssertionError('runtime mutation accepted: ' + prop)
                    q.cmd('quit')
                    assert child.wait(timeout=10) == 0
                print('PASS:', label, flush=True)
            finally:
                if q:
                    q.close()
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()
