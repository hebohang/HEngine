"""CPU-only client protocol tests; no engine, mailbox or GPU is started."""
import unittest
from editor_vfx_acceptance import Protocol, retired

class Fake:
    def __init__(self):
        self.serial=10; self.total=0; self.ticket=0; self.pending=0; self.bad_delta=False
    def __call__(self, method, params):
        action=params.get('action')
        if action=='stats': result={'updateSerial':self.serial}
        elif action=='step':
            self.ticket+=1; self.pending=params['frames']
            result=dict(active=True,ticket=self.ticket)
        elif action=='state':
            if self.pending:
                self.serial+=self.pending+(1 if self.bad_delta else 0)
                self.total+=self.pending; self.pending=0
            result=dict(active=True,cleanupFailed=False,ticket=self.ticket,
                        completedTicket=self.ticket,remainingFrames=0,totalFrames=self.total)
        else: return dict(protocolVersion=1,ok=False,error={'code':'rejected'})
        return dict(protocolVersion=1,ok=True,result=result)

class ProtocolTests(unittest.TestCase):
    def client(self, fake=None): return Protocol(fake or Fake(), sleep=lambda _:None)
    def test_exact_step_and_ticket(self):
        p=self.client(); result=p.step(30)
        self.assertEqual(result['stats']['updateSerial'],40)
        self.assertEqual(p.total_frames,30)
    def test_wrong_update_delta_rejected(self):
        f=Fake(); f.bad_delta=True
        with self.assertRaises(AssertionError): self.client(f).step(5)
    def test_paused_does_not_update(self):
        self.assertEqual(self.client().paused(),dict(before=10,after=10))
    def test_real_negative_response_required(self):
        p=self.client(); p.negative(dict(action='invalid'))
        p=Protocol(lambda *_:dict(protocolVersion=1,ok=True,result={}),sleep=lambda _:None)
        with self.assertRaises(AssertionError): p.call('editor.vfx',reject=True)
    def test_negative_state_mutation_rejected(self):
        f=Fake()
        def mutate(method,params):
            response=f(method,params)
            if params.get('action')=='invalid': f.serial+=1
            return response
        with self.assertRaises(AssertionError): self.client(mutate).negative(dict(action='invalid'))
    def test_no_boolean_or_overlong_steps(self):
        for n in (True,False,0,3601):
            p=self.client()
            with self.assertRaises(AssertionError): p.step(n)
            self.assertEqual(p.requests,0)
    def test_total_budget_not_reset(self):
        p=self.client(); p.total_frames=36000
        with self.assertRaises(AssertionError): p.step(1)
    def test_cleanup_request_reserve(self):
        p=self.client(); p.requests=240
        with self.assertRaises(AssertionError): p.vfx('state')
        p.call('editor.vfx',dict(action='state'),cleanup=True)
        p.requests=256
        with self.assertRaises(AssertionError): p.call('session.stop',cleanup=True)
    def test_unavailable_live_counter_not_zero_pass(self):
        s=dict(activeEffects=dict(status=0,current=0),cpuInstances=dict(status=1,current=0),gpuAllocation={})
        with self.assertRaises(AssertionError): retired(s)
    def test_normal_end_waits_for_retirement(self):
        calls=[]
        def exchange(method,params):
            calls.append(params['action'])
            if len(calls)==1:
                result=dict(active=True,cleanupFailed=False)
            elif params['action']=='end':
                result=dict(active=True,retiring=True,retired=False,ticket=4)
            else:
                result=dict(active=False,retiring=False,retired=True,completedTicket=4,cleanupFailed=False)
            return dict(protocolVersion=1,ok=True,result=result)
        p=self.client(exchange)
        self.assertTrue(p.end()['retired'])
        self.assertEqual(calls,['state','end','state'])
    def test_finally_does_not_repeat_inactive_end(self):
        calls=[]
        def exchange(method,params):
            calls.append(params['action'])
            return dict(protocolVersion=1,ok=True,result=dict(active=False,cleanupFailed=False,retired=True))
        self.client(exchange).end(cleanup=True)
        self.assertEqual(calls,['state'])

if __name__=='__main__': unittest.main()
