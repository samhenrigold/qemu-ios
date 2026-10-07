#!/usr/bin/env python3
"""Maintained host QMP adapter contracts; native PMU verdict is a separate gate."""
import sys, unittest
from pathlib import Path
from unittest.mock import patch
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'imgtools'))
import itqmp

class Q:
    def __init__(self,machine='iPod-Touch-machine',states=('running','completed'),dark=True):
        self.machine,self.states,self.dark=machine,list(states),dark
        self.commands=[];self.cancelled=False;self.waited=False
    def cmd(self,name,**args):
        self.commands.append((name,args))
        if name=='qom-get':
            return self.machine if args['property']=='type' else self.dark
        if name=='query-input-sequence':
            return {'status':self.states.pop(0) if len(self.states)>1 else self.states[0]}
        if name=='input-cancel-sequence':self.cancelled=True
        return {}
    def wait_for_guest_shutdown(self,timeout):self.waited=True

class HostPower(unittest.TestCase):
    def test_n72_exact_virtual_sequence_and_reverse_key_order(self):
        q=Q();ident,first=itqmp.poweroff_sequence(q)
        self.assertFalse(first);self.assertGreater(ident,0)
        name,args=q.commands[-1];self.assertEqual(name,'input-send-sequence')
        e=args['events'];self.assertEqual(len(e),35)
        self.assertEqual([x['key'] for x in e[:3]],['meta_l','shift','h'])
        self.assertEqual([x['key'] for x in e[3:6]],['h','shift','meta_l'])
        self.assertTrue(all(x['at-ms']==0 for x in e[:3]))
        self.assertTrue(all(x['at-ms']==150 for x in e[3:6]))
        self.assertEqual(e[6]['at-ms'],2650);self.assertEqual(e[8]['at-ms'],6150)
        self.assertEqual(e[-1]['at-ms'],9570);self.assertEqual(e[-1]['phase'],'end')
        self.assertEqual(e[-1]['x'],295/320)
    def test_n45_original_long_hold_and_no_cable_change(self):
        q=Q('iPod-Touch-1G-machine',states=('completed',))
        ident,first=itqmp.poweroff_sequence(q)
        self.assertTrue(first)
        self.assertEqual(q.commands[-1][1]['events'][8]['at-ms'],8650)
        itqmp.finish_poweroff_sequence(q,ident,first)
        self.assertFalse(any(n=='qom-set' for n,_ in q.commands))
    def test_cable_only_after_input_completed_and_backlight_off(self):
        q=Q();ident,first=itqmp.poweroff_sequence(q)
        with patch.object(itqmp.time,'sleep'):
            itqmp.finish_poweroff_sequence(q,ident,first)
        self.assertEqual(q.commands[-1],('qom-set',{'path':'/machine','property':'usb-attached','value':False}))
        seen=[x for x in q.commands if x[0]=='qom-get' and x[1].get('property')=='display-sleeping']
        self.assertEqual(len(seen),1)
    def test_backlight_on_times_out_and_releases_owned_sequence(self):
        q=Q(states=('completed',),dark=False)
        with patch.object(itqmp.time,'monotonic',side_effect=[0,0,2]),patch.object(itqmp.time,'sleep'):
            with self.assertRaises(TimeoutError):itqmp.finish_poweroff_sequence(q,42,False,timeout=1)
        self.assertTrue(q.cancelled);self.assertFalse(any(n=='qom-set' for n,_ in q.commands))
    def test_interrupted_sequence_fails_and_cancels(self):
        q=Q(states=('cancelled',))
        with self.assertRaises(RuntimeError):itqmp.finish_poweroff_sequence(q,42,False)
        self.assertTrue(q.cancelled)
    def test_unsupported_machine_and_coordinate_fail_before_submission(self):
        for q,kwargs in [(Q('ipad1-machine'),{}),(Q(),{'knob_y':480})]:
            with self.assertRaises((RuntimeError,ValueError)):itqmp.poweroff_sequence(q,**kwargs)
            self.assertFalse(any(n=='input-send-sequence' for n,_ in q.commands))
    def test_eof_requires_retained_guest_origin_shutdown(self):
        q=Q()
        q.cmd=lambda *a,**k:(_ for _ in ()).throw(EOFError())
        itqmp.finish_poweroff_sequence(q,42,False)
        self.assertTrue(q.waited)
        def fail(_):raise EOFError('No guest shutdown')
        q.wait_for_guest_shutdown=fail
        with self.assertRaises(EOFError):itqmp.finish_poweroff_sequence(q,42,False)

if __name__=='__main__':unittest.main()
