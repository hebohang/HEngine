import copy
import unittest
import json
from pathlib import Path
import tempfile

from editor_animation_acceptance import dependency_inputs, require_ready, verify_advance, graph_fit_result, require_controller_fits


class PreviewReceiptTests(unittest.TestCase):
    def test_controller_initial_clipping_is_not_hidden_by_later_focus(self):
        state=dict(nodeCount=5,lastUiFrame=20,applicationFrame=21,
                   visibleMin=[0,0],visibleMax=[100,100],boundsMin=[1,1],boundsMax=[120,90],
                   visibilityToleranceCanvasUnits=0,nodesFullyVisible=False)
        self.assertEqual(graph_fit_result(state)['status'],'FAIL')
        state['visibleMax']=[130,100];state['nodesFullyVisible']=True
        self.assertEqual(graph_fit_result(state)['status'],'PASS')
        state['nodesFullyVisible']=False
        with self.assertRaisesRegex(AssertionError,'disagrees'): graph_fit_result(state)

    def test_actual_clipped_bounds_block_overall_gate_even_if_other_fit_passes(self):
        initial=dict(nodeCount=5,lastUiFrame=950,applicationFrame=951,
            visibleMin=[0,0],visibleMax=[1029,827],boundsMin=[0,0],boundsMax=[1225,537],
            visibilityToleranceCanvasUnits=0,nodesFullyVisible=False)
        fitted=dict(initial,visibleMin=[-61.25,-272.99],visibleMax=[1286.25,809.99],nodesFullyVisible=True)
        for failed_key in ('controllerInitialFit','controllerExplicitFocusFit'):
            report={key:graph_fit_result(initial if key==failed_key else fitted)
                for key in ('controllerInitialFit','controllerExplicitFocusFit')}
            saved=copy.deepcopy(report)
            with self.assertRaisesRegex(AssertionError,failed_key): require_controller_fits(report)
            self.assertEqual(report,saved)
        require_controller_fits({key:graph_fit_result(fitted)
            for key in ('controllerInitialFit','controllerExplicitFocusFit')})

    def test_dependency_closure_follows_buffers_textures_and_deduplicates_cycles(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); assets=root/'Project/Assets'; assets.mkdir(parents=True)
            (assets/'a.hanim').write_text(json.dumps(dict(meshResPath='Assets/m.hmesh')))
            (assets/'m.hmesh').write_text(json.dumps(dict(bufferVec=[dict(uri='buffer.bin')],material='Assets/m.hmat')))
            (assets/'m.hmat').write_text(json.dumps(dict(texture='Assets/t.png',cycle='Assets/a.hanim')))
            for name in ('buffer.bin','t.png'): (assets/name).write_bytes(b'fixture')
            paths=dependency_inputs([assets/'a.hanim'],root)
            self.assertEqual({p.name for p in paths},{'a.hanim','m.hmesh','m.hmat','buffer.bin','t.png'})

    def test_missing_or_escaping_dependency_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); assets=root/'Project/Assets'; assets.mkdir(parents=True)
            source=assets/'m.hmesh'
            for reference in ('missing.bin','../../outside.bin'):
                source.write_text(json.dumps(dict(bufferVec=[dict(uri=reference)])))
                with self.assertRaises(ValueError): dependency_inputs([source],root)

    def state(self):
        return dict(ready=True,loadError='',applicationFrame=40,lastUiFrame=39,
                    progress=.15,seconds=.15,durationSeconds=1.,fixedDeltaSeconds=1/60,
                    playing=False,framesRemaining=0,poseUpdates=30)

    def test_both_rates_and_loop_boundary(self):
        for speed in (.5,2.):
            before=self.state(); before['seconds']=.9
            after=copy.deepcopy(before)
            after.update(seconds=(.9+12/60*speed)%1.,poseUpdates=42)
            verify_advance(before,after,12,speed)

    def test_wrong_elapsed_time_rejected(self):
        before=self.state(); after=copy.deepcopy(before)
        after.update(seconds=.151,poseUpdates=42)
        with self.assertRaisesRegex(AssertionError,'progression'):
            verify_advance(before,after,12,.5)

    def test_unfinished_play_rejected(self):
        before=self.state(); after=copy.deepcopy(before)
        after.update(seconds=.25,poseUpdates=42,framesRemaining=1)
        with self.assertRaisesRegex(AssertionError,'frame boundary'):
            verify_advance(before,after,12,.5)

    def test_stale_or_nonfinite_ui_state_rejected(self):
        for field,value in [('lastUiFrame',1),('seconds',float('nan')),('ready',False)]:
            state=self.state(); state[field]=value
            with self.assertRaises(AssertionError): require_ready(state)

    def test_progress_without_pose_updates_rejected(self):
        before=self.state(); after=copy.deepcopy(before); after['seconds']=.25
        with self.assertRaisesRegex(AssertionError,'pose updates'):
            verify_advance(before,after,12,.5)


if __name__=='__main__': unittest.main()
