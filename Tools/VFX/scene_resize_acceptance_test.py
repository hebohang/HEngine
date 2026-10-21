"""Pure driver control-flow regression; no runtime/process or GPU calls."""
import struct
import tempfile
import unittest
from pathlib import Path

from scene_resize_acceptance import Acceptance, FAILURES, RESERVED, SCENES


class FakeSession:
    def __init__(self,output,retain_handles):
        self.output=output
        self.raw=output/'fake-captures'; self.raw.mkdir()
        self.live=set(); self.next_handle=1; self.width=1280; self.height=720
        self.generation=1; self.scene=SCENES[0]; self.pending=None
        self.draws=0; self.dispatches=0; self.retain_handles=retain_handles

    def call(self,method,params):
        if method=='runtime.step':
            if self.pending:
                self.scene=self.pending; self.pending=None; self.generation+=1
                if not self.retain_handles: self.live.clear()
            if self.live:
                self.draws+=params['frames']; self.dispatches+=params['frames']
            return {'ok':True}
        if method=='vfx.stats':
            allocation={key:0 for key in FAILURES}
            allocation.update({key:len(self.live)*256 for key in RESERVED})
            return {'gpuAllocation':allocation,'gpuEnabled':True,
                'totalRecordedDrawCount':self.draws,'totalRecordedDispatchCount':self.dispatches}
        if method=='vfx.play':
            handle=self.next_handle; self.next_handle+=1; self.live.add(handle)
            return {'handle':handle,'playing':True}
        if method=='vfx.control':
            if params['action']=='stop': self.live.discard(params['handle'])
            return {'handle':params['handle'],'accepted':True,'playing':params['handle'] in self.live}
        if method=='window.resize':
            self.width=params['width']; self.height=params['height']
            return {'ok':True,'clientWidth':self.width,'clientHeight':self.height}
        if method=='session.status':
            return {'renderWidth':self.width,'renderHeight':self.height,'runtimeSceneRequestPending':self.pending is not None}
        if method=='world.summary':
            return {'scene':self.scene,'sceneGeneration':self.generation,'available':True}
        if method=='runtime.scene.load':
            self.pending=params['path']; return {'ok':True}
        if method=='capture.frame':
            path=self.raw/params['filename']
            path.write_bytes(b'\x89PNG\r\n\x1a\n'+struct.pack('>I',13)+b'IHDR'+struct.pack('>II',self.width,self.height))
            return {'path':str(path)}
        raise AssertionError('Unexpected mock method: '+method)


class ResizeDriverTests(unittest.TestCase):
    def test_complete_resize_switch_control_flow(self):
        for retain_handles in [False,True]:
            with self.subTest(retain_handles=retain_handles), tempfile.TemporaryDirectory() as temporary:
                session=FakeSession(Path(temporary),retain_handles)
                report={'checks':[]}
                test=Acceptance(session,report)
                report['baseline']=test.stats()
                test.capture('empty',[1280,720])
                test.play_pair('initial')
                test.resize(960,540,'resize-small')
                test.resize(1280,720,'resize-restore')
                test.switch(SCENES[1],'switch-game')
                test.resize(1024,768,'resize-aspect')
                test.switch(SCENES[0],'switch-neutral')
                test.resize(1280,720,'final-restore')
                test.stop_pair('final')
                test.capture('final-retired',[1280,720])
                self.assertTrue(all(c['status']=='PASS' for c in report['checks']))
                self.assertFalse(session.live)
                self.assertEqual(len(report['oldHandleObservations']),2)
                for observation in report['oldHandleObservations']:
                    self.assertTrue(all(state['playing'] is retain_handles for state in observation['handles']))
                extent=next(c for c in report['checks'] if c['name']=='resize-small.render_extent')
                self.assertEqual(extent['runtimeState']['renderWidth'],960)
                committed=next(c for c in report['checks'] if c['name']=='switch-game.committed')
                self.assertFalse(committed['runtimeState']['runtimeSceneRequestPending'])

    def test_failed_assertion_still_records_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            report={'checks':[]}
            test=Acceptance(FakeSession(Path(temporary),False),report)
            with self.assertRaises(RuntimeError): test.check('negative',False,runtimeState={'available':False})
            self.assertEqual(report['checks'][0]['status'],'FAIL')


if __name__=='__main__': unittest.main()
