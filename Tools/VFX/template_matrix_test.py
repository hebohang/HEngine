"""Pure matrix contract checks; no engine or image renderer is launched."""
import copy
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import template_matrix as matrix


class Session:
    def __init__(self, recipe, supports=True, draws=1):
        self.frame=0; self.end=matrix.stages(recipe)[-1][0]
        self.supports=supports; self.draws=draws; self.position=[0,0,0]; self.stopped=False
    def call(self, method, params=None):
        params=params or {}
        if method=='runtime.step': self.frame+=params['frames']; return {}
        if method=='vfx.play':
            self.position=params['position']; self.speed=params['speed']; self.intensity=1
            return dict(handle=1,playing=True)
        if method=='vfx.control':
            action=params['action']
            if action=='stop': self.stopped=True
            if action=='transform': self.position=params['position']
            if action=='parameter': self.intensity=params['value']
            return dict(accepted=True,playing=not self.stopped and self.frame<self.end,
                        supportsGlobalColor=self.supports,position=self.position,speed=self.speed,intensity=self.intensity)
        raise AssertionError(method)


class MatrixTest(unittest.TestCase):
    def test_visual_session_can_omit_csv_without_changing_default(self):
        for options, expected in [({}, True), ({'render_stats': False}, False)]:
            session = matrix.Session(Path('unused'), 'Vulkan', 'candidate', **options)
            with patch.object(session, 'run', side_effect=RuntimeError('before launch')) as run:
                with self.assertRaisesRegex(RuntimeError, 'before launch'):
                    session.start()
            arguments = run.call_args.args[0]
            self.assertEqual('-RenderStats' in arguments, expected)
            self.assertIn('-RenderValidation', arguments)
            self.assertIn('-ExistingBuildSha256', arguments)

    def test_recipe_quality_groups(self):
        groups=dict(matrix.recipe_groups(matrix.selected_recipes(None)))
        self.assertEqual(set(groups),{('cpu','High'),('cpu','Low'),('gpu','High')})
        self.assertEqual(len(groups[('cpu','Low')]),6)
        self.assertEqual(sum(map(len,groups.values())),24)
        for (simulation,quality),recipes in groups.items():
            self.assertTrue(all(r['compile']['quality']==quality and
                                r['compile']['simulation']==simulation for r in recipes))

    def test_quality_selected_and_queried_before_capture(self):
        from unittest.mock import Mock
        empty=dict(activeEffects={'current':0},cpuInstances={'current':0})
        for simulation,quality in [('cpu','Low'),('cpu','High'),('gpu','High')]:
            policy=dict(accepted=True,tier=quality,requested=simulation=='gpu',enabled=simulation=='gpu')
            session=Mock()
            session.call.side_effect=[empty,policy,policy]
            result=matrix.configure_quality(session,simulation,quality)
            self.assertEqual(result['observed']['tier'],quality)
            self.assertEqual([call.args for call in session.call.call_args_list],
                             [('vfx.stats',),('vfx.quality',{'tier':quality}),('vfx.quality',)])

    def test_quality_rejection_busy_or_wrong_policy_fails(self):
        from unittest.mock import Mock
        empty=dict(activeEffects={'current':0},cpuInstances={'current':0})
        low=dict(accepted=True,tier='Low',requested=False,enabled=False)
        responses=[
            [dict(activeEffects={'current':1},cpuInstances={'current':0})],
            [dict(activeEffects={'current':0},cpuInstances={'current':1})],
            [empty,dict(low,accepted=False)],
            [empty,dict(low,tier='High')],
            [empty,dict(low,requested=True)],
            [empty,dict(low,enabled=True)],
            [empty,low,dict(low,tier='High')],
        ]
        for values in responses:
            session=Mock(); session.call.side_effect=values
            with self.assertRaises(RuntimeError): matrix.configure_quality(session,'cpu','Low')

    def run_effect(self, recipe, scene, supports=True, draws=1):
        session=Session(recipe,supports,draws)
        with tempfile.TemporaryDirectory() as temp, patch.object(matrix,'digest',return_value='test'), \
             patch.object(matrix,'capture',side_effect=lambda *args:dict(stats={'drawCalls':{'current':draws}})):
            result=matrix.effect_capture(session,Path(temp)/'effect',recipe,scene)
        self.assertTrue(session.stopped)
        return result

    def test_all_recipes_two_scenes(self):
        recipes=matrix.selected_recipes(None)
        self.assertEqual(len(recipes),24)
        self.assertEqual(sum(r['compile']['simulation']=='gpu' for _,r in recipes),6)
        for _,recipe in recipes:
            for scene in matrix.SCENES:
                with self.subTest(recipe=recipe['id'],scene=scene):
                    result=self.run_effect(recipe,scene)
                    self.assertEqual(result['commandStatus'],'PASS',result.get('error'))
                    self.assertEqual(result['visualStatus'],'NOT_REVIEWED')
                    self.assertEqual({label for c in result['captures'] for label in c['stages']}, {'start','peak','decay','end'})
                    if scene=='game' and recipe['template'] in ('aura','telegraph'):
                        self.assertAlmostEqual(result['playPosition'][1],.035)
                    for motion in result['motion']:
                        self.assertAlmostEqual(motion['position'][1],.52 if scene=='game' else .5)

    def test_unsupported_or_empty_draw_fails_and_cleans(self):
        recipe=matrix.selected_recipes(['explosion-ember'])[0][1]
        for supports,draws in [(False,1),(True,0)]:
            result=self.run_effect(recipe,'game',supports,draws)
            self.assertEqual(result['commandStatus'],'FAIL')
            self.assertTrue(any(check['status']=='FAIL' for check in result['checks']))


if __name__=='__main__':unittest.main()
