#!/usr/bin/env python3
"""Agent launch routing and strict presubmit-only legacy fallback."""
from types import SimpleNamespace
from unittest.mock import Mock, patch
import regress as r
q=object();dev=SimpleNamespace(qmp=q)
with patch.object(r.itqmp,'agent_alive',return_value=True), patch.object(r,'prepare_launcher') as ssh:
 control=r.prepare_app_control(None,None,dev,None)
 assert isinstance(control,r.AgentControl) and control.qmp is q
 ssh.assert_not_called()
with patch.object(r.itqmp,'agent_alive',return_value=False), patch.object(r,'prepare_launcher',return_value=22) as ssh:
 assert r.prepare_app_control(None,None,dev,None)==22
 ssh.assert_called_once()
for operation,request,response in (
 ('frontmost',':frontmost',b'org.example.App\nFriendly title\n'),
 ('lockstatus',':lock-status',b'locked=1 passcode=0\n'),
 ('launch','org.example.App',b''),
):
 with patch.object(r.itqmp,'agent',return_value=(0,response)) as rpc, patch.object(r,'guest_ssh') as ssh:
  result=r.springboard(None,control,request)
  assert result.returncode==0
  rpc.assert_called_once_with(q,operation,request if operation=='launch' else '',timeout=60)
  if operation=='frontmost': assert result.stdout=='sblaunch: frontmost=org.example.App'
  if operation=='lockstatus': assert result.stdout.strip()=='sblaunch: locked=1 passcode=0'
  ssh.assert_not_called()
for response in [(5,b'refused'),TimeoutError('reply lost'),EOFError('session ended')]:
 with patch.object(r.itqmp,'agent',side_effect=response if isinstance(response,Exception) else None,
                   return_value=response) as rpc, patch.object(r,'guest_ssh') as ssh:
  try:
   result=r.springboard(None,control,'org.example.App')
   assert result.returncode==5
  except (TimeoutError,EOFError):
   assert isinstance(response,Exception)
  rpc.assert_called_once();ssh.assert_not_called()
with patch.object(r.itqmp,'agent',return_value=(1,b'not running')) as rpc:
 result=r.control_exec(None,control,'killall Harness',timeout=10)
 assert result.returncode==1 and result.stdout=='not running'
 rpc.assert_called_once_with(q,'exec','killall Harness',timeout=10)
print('PASS: agent app control, explicit legacy fallback, exact foreground identity and no replay after submission')
