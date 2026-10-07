#!/usr/bin/env python3
"""Agent-only app control: v2 readiness, typed SBS ops, launchd app stop, no replay."""
from types import SimpleNamespace
from unittest.mock import patch
import regress as r
q=object();dev=SimpleNamespace(qmp=q)
with patch.object(r,'ensure_agent',return_value=(True,'it_agent v2')):
 control=r.prepare_app_control(None,None,dev,None)
 assert isinstance(control,r.AgentControl) and control.qmp is q
result=r.Result('x')
with patch.object(r,'ensure_agent',return_value=(False,'no agent')), patch.object(r,'log'):
 assert r.prepare_app_control(None,None,dev,result) is None and result.ok is False and result.detail=='no agent'
for operation,request,response in (
 ('frontmost',':frontmost',b'org.example.App\nFriendly title\n'),
 ('lockstatus',':lock-status',b'locked=1 passcode=0\n'),
 ('launch','org.example.App',b''),
):
 with patch.object(r.itqmp,'agent',return_value=(0,response)) as rpc:
  result=r.springboard(None,control,request)
  assert result.returncode==0
  rpc.assert_called_once_with(q,operation,request if operation=='launch' else '',timeout=60)
  if operation=='frontmost': assert result.stdout=='sblaunch: frontmost=org.example.App'
  if operation=='lockstatus': assert result.stdout.strip()=='sblaunch: locked=1 passcode=0'
for response in [(5,b'refused'),TimeoutError('reply lost'),EOFError('session ended')]:
 with patch.object(r.itqmp,'agent',side_effect=response if isinstance(response,Exception) else None,
                   return_value=response) as rpc:
  try:
   result=r.springboard(None,control,'org.example.App')
   assert result.returncode==5
  except (TimeoutError,EOFError):
   assert isinstance(response,Exception)
  rpc.assert_called_once()
listing=(0,b'PID\tStatus\tLabel\n12\t-\tUIKitApplication:org.example.App[0x1a2b]\n13\t-\tUIKitApplication:org.example.Apple[0x1]\n')
with patch.object(r.itqmp,'spawn',side_effect=[listing,(0,b'')]) as rpc:
 assert r.stop_app(control,'org.example.App').returncode==0
 assert rpc.call_args_list[0].args==(q,['/bin/launchctl','list'])
 assert rpc.call_args_list[1].args==(q,['/bin/launchctl','stop','UIKitApplication:org.example.App[0x1a2b]'])
with patch.object(r.itqmp,'spawn',return_value=(0,b'PID\tStatus\tLabel\n')) as rpc:
 assert r.stop_app(control,'org.example.App').returncode==0 and rpc.call_count==1
print('PASS: agent app control, launchd app stop, exact foreground identity and no replay after submission')
