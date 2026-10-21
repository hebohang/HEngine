"""CPU-only parsing and owned-session cleanup checks."""
import json
import unittest
from unittest.mock import Mock, patch

import memory_plateau_probe as probe


def rows():
    return [dict(pid=123,segment=i//100,sample=i%100,elapsedMs=i*100,
                 privateBytes=4096+(i%2)*4096,workingSetBytes=2048) for i in range(300)]


class MemoryPlateauTest(unittest.TestCase):
    def test_complete_ordered_samples_and_unfiltered_summary(self):
        values=rows()
        parsed=probe.parse_samples('\n'.join(map(json.dumps,values)),123,3)
        self.assertEqual(parsed,values)
        self.assertEqual(len(probe.summarize(parsed)),3)
        self.assertEqual(probe.summarize(parsed)[0]['maximum'],8192)
        self.assertEqual(probe.summarize(parsed)[0]['minimum'],4096)
        self.assertEqual(probe.summarize(parsed)[0]['median'],6144)

    def test_partial_identity_order_and_invalid_values_rejected(self):
        cases=[rows()[:-1]]
        for field,value in [('pid',456),('sample',55),('segment',2),('elapsedMs',-1),
                            ('privateBytes',float('nan')),('workingSetBytes',True)]:
            data=rows(); data[10][field]=value; cases.append(data)
        for data in cases:
            with self.assertRaises(ValueError): probe.parse_samples('\n'.join(map(json.dumps,data)),123,3)

    def test_sampler_failure_still_stops_only_owned_session(self):
        empty=dict(gpuEnabled=True,activeEffects={'current':0},cpuInstances={'current':0},
                   gpuAllocation=dict(allocatedEmitters=0,reservedParticles=0,reservedTrails=0))
        session=Mock()
        def call(method,params=None):
            if method=='vfx.play': return {'handle':7}
            if method=='vfx.stats': return empty
            return {}
        session.call.side_effect=call
        sampler=Mock(side_effect=TimeoutError('owned sampler timeout'))
        with patch.object(probe,'live_cycle_valid',return_value=True),self.assertRaises(TimeoutError):
            probe.execute(session,{},3,sampler)
        session.start.assert_called_once()
        session.stop.assert_called_once()
        self.assertEqual([item.args[1]['frames'] for item in session.call.call_args_list if item.args[0]=='runtime.step'],[120,81,30])
        plays=[item for item in session.call.call_args_list if item.args[0]=='vfx.play']
        self.assertEqual(len(plays),1)

    def test_start_failure_still_uses_session_cleanup(self):
        session=Mock();session.start.side_effect=RuntimeError('failed startup')
        with self.assertRaises(RuntimeError):probe.execute(session,{},1,Mock())
        session.stop.assert_called_once()
        session.call.assert_not_called()


if __name__=='__main__': unittest.main()
