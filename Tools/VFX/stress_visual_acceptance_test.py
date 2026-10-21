"""CPU-only checks for the official stress visual protocol."""
import unittest
from unittest.mock import Mock

import stress_visual_acceptance as visual


class StressVisualTest(unittest.TestCase):
    def test_age_schedule_accounts_for_pause_and_natural_tail(self):
        schedule=visual.stages()
        age=0; host=0
        for label,target,paused in schedule:
            count=30 if paused else target-age
            self.assertGreaterEqual(count,0)
            host+=count
            if not paused: age=target
            if label.startswith('paused'): self.assertEqual(age,201)
            if label=='resumed': self.assertEqual((age,host),(231,291))
        self.assertEqual((age,host),(1431,1491))
        self.assertIn(('mid-plateau',600,False),schedule)
        self.assertEqual({name:age for name,age,_ in schedule if name.startswith('owner')},
                         {'owner-before':1199,'owner-after':1201})
        self.assertGreater(dict((name,age) for name,age,_ in schedule)['life-after'],1200+200)
        with self.assertRaises(ValueError): visual.stages(lifetime=180)

    def test_steps_are_bounded_and_do_not_round_up(self):
        session=Mock(); visual.step(session,201)
        self.assertEqual([call.args for call in session.call.call_args_list],
                         [('runtime.step',{'frames':120}),('runtime.step',{'frames':81})])
        with self.assertRaises(ValueError): visual.step(session,-1)

    def test_retirement_requires_cpu_and_gpu_ownership_zero(self):
        stats=dict(activeEffects={'current':0},cpuInstances={'current':0},
                   gpuAllocation=dict(allocatedEmitters=0,reservedParticles=0,reservedTrails=0))
        self.assertTrue(visual.retired(stats))
        for key in stats['gpuAllocation']:
            changed=dict(stats,gpuAllocation=dict(stats['gpuAllocation'],**{key:1}))
            self.assertFalse(visual.retired(changed))
        self.assertFalse(visual.retired(dict(stats,gpuAllocation={})))
        self.assertFalse(visual.retired(dict(stats,cpuInstances={'current':1})))

    def test_actual_fixture_lifetimes_are_not_guessed(self):
        import json
        manifest=json.loads((visual.SOURCE/'manifest.json').read_text())
        for nominal in (20000,50000):
            name='GpuSparks'+str(nominal//1000)+'k'
            metadata=json.loads((visual.SOURCE/name/'PROVENANCE.json').read_text())
            params=metadata['parameters']
            visual.stages(params['lifetime_frames'],params['owner_lifetime_frames'])
            entry=next(item for item in manifest['outputs'] if item['name']==name)
            self.assertEqual(entry['validation']['binaryVersion'],1810)
            identity=visual.stress_identity(visual.ROOT/'Project/Assets/Effects/Stress'/(name+'.efkpkg'),nominal)
            self.assertEqual(entry['package_sha256'],identity['packageSha256'])

    def test_owner_motion_explains_framing_without_claiming_bounds(self):
        motion=visual.owner_motion(visual.SOURCE/'GpuSparks20k/roundtrip.xml',.1,[0,-.4,0])
        self.assertEqual(motion['velocityYPerFrame'],.02)
        self.assertEqual(motion['rotationYDegreesPerFrame'],10.)
        self.assertAlmostEqual(motion['estimatedOwnerY']['1200'],2.)
        self.assertIn('not a particle bounding-box',motion['interpretation'])

    def test_world_gravity_camera_fit_and_custom_scale_rejection(self):
        framing=visual.framing_estimate(.35,[0,-.4,0])
        self.assertAlmostEqual(framing['gravityDisplacementBound'],12.06)
        self.assertAlmostEqual(framing['estimatedAabb']['lower'][1],-19.96)
        self.assertGreater(framing['verticalMarginFraction'],.13)
        self.assertAlmostEqual(visual.framing_estimate(.1,[0,-.4,0])['gravityDisplacementBound'],12.06)
        self.assertIn('not physical meters',framing['units'])
        for scale,position in [(1,[0,-.4,0]),(.35,[0,30,0]),(.35,[100,-.4,0]),(.35,[0,-.4,-35])]:
            with self.assertRaises(ValueError): visual.framing_estimate(scale,position)

    def test_scene_camera_matches_framing_assumptions(self):
        visual.validate_camera(visual.ROOT/'Project'/visual.SCENE)


if __name__=='__main__': unittest.main()
