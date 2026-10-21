import ast
import copy
import json
from pathlib import Path
import unittest

from character_acceptance import (ROOT, REQUIRED_CHARACTER, EXPECTED_CHECKPOINTS,
                                 validate_receipt, validate_checkpoint, validate_animation_observation)

# Exercise the production interval function without importing or simulating HEngine.
source=ast.parse((ROOT/'Project/Scripts/VFXCharacterAcceptance.py').read_text(encoding='utf-8'))
function=next(node for node in source.body if isinstance(node,ast.FunctionDef) and node.name=='expected_walk_events')
namespace={}
exec(compile(ast.Module(body=[function],type_ignores=[]),'<expected_walk_events>','exec'),namespace)
expected_walk_events=namespace['expected_walk_events']


class CharacterEvidenceTests(unittest.TestCase):
    def test_exact_intervals_partition_without_loss_or_duplicates(self):
        schedule=[.2,.1,.4,.2,.2,.2,.4,.4]
        elapsed=0.;events=[]
        for delta in schedule:
            events+=expected_walk_events(elapsed,elapsed+delta);elapsed+=delta
        self.assertEqual(events,expected_walk_events(0,2.1))
        self.assertEqual(events,[(0,'left_step'),(0,'right_step'),(1,'left_step'),(1,'right_step')])
        self.assertEqual(expected_walk_events(.25,.75),[(0,'right_step')])
        self.assertEqual(expected_walk_events(.99,1.01),[])

    def test_checkpoint_path_and_sequence_are_strict(self):
        value=dict(schema=1,nonce='a'*32,sequence=1,label='loop-before')
        self.assertEqual(validate_checkpoint(value,0),value)
        for key,other in [('label','../escape'),('sequence',65),('nonce','wrong')]:
            bad=dict(value);bad[key]=other
            with self.assertRaises(ValueError):validate_checkpoint(bad,0)
        with self.assertRaises(ValueError):validate_checkpoint(value,1)

    def test_missing_runtime_assertions_are_not_accepted(self):
        value=dict(status='passed',checks=list(REQUIRED_CHARACTER),instances=['a','b'],
                   events=[dict(name='attack'),dict(name='footstep')],positions=[dict(before=[0,0,0],after=[1,0,0],expected=[1,0,0])])
        validate_receipt('character',value)
        value['checks'].remove('destroyed_target_stops_actual_audio')
        with self.assertRaisesRegex(ValueError,'Missing character checks'):validate_receipt('character',value)

    def test_submitted_palettes_must_be_complete_and_finite(self):
        item=dict(currentPaletteCount=40,hashCoversFullPalettes=True,nonFiniteComparisons=0)
        value=dict(available=True,source='lastSuccessfullySubmittedMainRenderWorld',objectsTruncated=False,objects=[item,copy.copy(item)])
        validate_animation_observation(value)
        value['objects'][1]['nonFiniteComparisons']=1
        with self.assertRaises(ValueError):validate_animation_observation(value)

    def test_large_angle_fixture_keeps_rig_identity(self):
        base=ROOT/'Project/Assets/Effects/Authoring'
        original=json.loads((base/'CharacterDemo/Idle.hanim').read_text())
        for name in ('Yaw170','YawMinus170'):
            value=json.loads((base/('CharacterRuntimeProbe/'+name+'.hanim')).read_text())
            for key in ('boneInfos','rootNode','meshResPath','targetSkeletonPath'):
                self.assertEqual(value[key],original[key])
            root=next(c for c in value['clips'][0]['channels'] if c['name']=='CharacterArmature')
            q=root['rotationKeys'][0]
            self.assertAlmostEqual(sum(q[k]**2 for k in ('qw','qx','qy','qz')),1.)
        self.assertEqual(len(EXPECTED_CHECKPOINTS),16)

if __name__=='__main__':unittest.main()
