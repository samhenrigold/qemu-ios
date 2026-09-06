#!/usr/bin/env python3
"""Boot capture preserves evidence and only stops the process it owns."""
import json
import os
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'imgtools'))
import bootshot
processes=[]
class Process:
    pid=12345
    returncode=None
    def __init__(self,args,**kwargs):self.args=args;processes.append(self)
    def poll(self):return self.returncode
    def terminate(self):self.returncode=-15
    def wait(self,**kwargs):return self.returncode
class QMP:
    wrong=False
    def __init__(self,*args,**kwargs):pass
    def cmd(self,name):
        assert name=='query-name'
        return {'name':'other' if self.wrong else processes[-1].args[2]}
    def shot(self,path):Path(path).write_bytes(b'P6\n1 1\n1\n\x01\x00\x01')
    def close(self):pass
with tempfile.TemporaryDirectory() as temp, patch.object(bootshot.subprocess,'Popen',Process),patch.object(bootshot.itqmp,'QMP',QMP),patch.dict(os.environ,{'SHOT_INTERVAL':'0.005'}):
    output=Path(temp)/'normal'
    bootshot.main(['28400','test','0.03','--out',str(output)])
    rows=json.loads((output/'frames.json').read_text())
    assert rows and rows[0][2:]==[1,2,3]
    assert processes[-1].returncode==-15 and not (output/'qemu.pid').exists()
    count=len(processes)
    try:bootshot.main(['28400','test','0.03','--out',str(output)])
    except FileExistsError:pass
    else:raise AssertionError('reused an existing overlay')
    assert len(processes)==count
    kept=Path(temp)/'kept'
    bootshot.main(['28400','test','0.03','--out',str(kept),'--keep-running'])
    assert processes[-1].poll() is None and (kept/'qemu.pid').read_text().strip()=='12345'
    QMP.wrong=True
    try:bootshot.main(['28400','test','0.03','--out',str(Path(temp)/'wrong')])
    except RuntimeError as error:assert 'another guest' in str(error)
    else:raise AssertionError('accepted an unrelated QMP guest')
    assert processes[-1].returncode==-15
print('PASS: bounded captures, fresh overlays, explicit retention and owned-process cleanup')
