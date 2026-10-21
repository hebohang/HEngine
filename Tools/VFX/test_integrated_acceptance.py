"""CPU-only protocol/derivation checks; no engine or visual acceptance claim."""
import copy
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
import integrated_acceptance as driver

class IntegratedProtocolTests(unittest.TestCase):
    def test_derivation_preserves_every_non_event_field(self):
        original={'version':1,'meshResPath':'mesh.hmesh','targetSkeletonPath':'skeleton.hskel',
            'clips':[{'duration':60,'ticksPerSecond':30,'channels':[{'positions':[[1,2,3]]}]}]}
        saved=copy.deepcopy(original)
        derived,duration=driver.derive_clip(original)
        self.assertEqual(original,saved)
        self.assertEqual(duration,2)
        self.assertEqual(derived['clips'][0].pop('events'),[dict(id='integrated-step',name='footstep',timeStamp=15,payload='socket')])
        self.assertEqual(derived,original)

    def test_existing_events_and_multiple_clips_are_not_silently_replaced(self):
        for clips in ([dict(duration=30,ticksPerSecond=30,events=[{'id':'authored'}])],[],[{},{}]):
            with self.assertRaises(ValueError): driver.derive_clip(dict(clips=clips))

    def test_step_waits_for_completed_not_just_accepted(self):
        class Session:
            calls=0
            def call(self,method,params=None):
                if method=='runtime.step': return dict(targetCompletedSteps=8)
                self.calls+=1
                return {'world':{'runtimeControl':dict(completedSteps=7 if self.calls==1 else 8,pendingSteps=1 if self.calls==1 else 0,stepInProgress=False)}}
        session=Session()
        with patch.object(driver.time,'sleep'): driver.settled_step(session,8)
        self.assertEqual(session.calls,2)

    def test_reload_keeps_same_path_and_requires_new_generation(self):
        class Session:
            def __init__(self): self.generation=1;self.request=None
            def call(self,method,params=None):
                if method=='world.summary': return dict(sceneGeneration=self.generation)
                if method=='runtime.scene.load': self.request=params
        session=Session()
        def step(_): session.generation+=1
        with patch.object(driver,'settled_step',step): result=driver.reload_scene(session,'Assets/Imported/fixture/Scene.he')
        self.assertEqual(session.request,dict(path='Assets/Imported/fixture/Scene.he',forceReload=True))
        self.assertEqual(result['sceneGeneration'],2)

    def test_versioned_controller_references_derived_clip(self):
        value=driver.controller('Assets/IntegratedAcceptance/id/Walk-v2.hanim','Assets/Imported/id/Resources/skeleton.hskel')
        self.assertEqual(value['nodes'][1]['clipPath'],'Assets/IntegratedAcceptance/id/Walk-v2.hanim')
        self.assertEqual(value['targetSkeletonPath'],'Assets/Imported/id/Resources/skeleton.hskel')
        self.assertEqual(value['transitions'][0]['toNodeId'],value['nodes'][1]['id'])

    def test_complete_presentation_tracks_preserved(self):
        result=driver.composition_for('Assets/Effects/Authoring/owned.efkpkg','owned')
        original=driver.read(driver.PROJECT/'Assets/Effects/Authoring/CharacterDemo/Footstep.json')
        self.assertEqual({t['kind'] for t in result['tracks']},{'vfx','audio','light','shake'})
        for track in original['tracks']:
            if track['kind']!='vfx': self.assertIn(track,result['tracks'])

    def test_importer_pin_rejects_changed_binary(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'importer.exe';path.write_bytes(b'candidate')
            import hashlib
            pin=hashlib.sha256(b'candidate').hexdigest()
            driver.verify_pin(path,pin)
            path.write_bytes(b'changed')
            with self.assertRaises(ValueError): driver.verify_pin(path,pin)

    def test_dependency_closure_includes_runtime_and_tool_chain(self):
        inputs=driver.input_hashes(Path(driver.__file__))
        names={Path(path).name for path in inputs}
        self.assertTrue({'authoring.py','content_sync.py','baked_model_adapter.py','preview.py',
            'PresentationPlayer.py','PresentationSchema.py','Session.ps1','Build.ps1','Storage.ps1',
            'Effekseer.exe','EffekseerCore.dll','monster_hit_01.wav','temporary-light.hprefab'}.issubset(names))
        self.assertLessEqual(len(inputs),512)

    def test_runtime_cleanup_checks_actual_handles(self):
        import importlib.util
        import sys
        from types import SimpleNamespace
        fake=SimpleNamespace(VFX=SimpleNamespace(IsEffectPlaying=lambda h:False),Audio=SimpleNamespace(IsPlaying=lambda h:False))
        with patch.dict(sys.modules,{'HEngine':fake,'Scripts.PresentationPlayer':SimpleNamespace(PresentationService=object)}):
            spec=importlib.util.spec_from_file_location('integrated_runtime_test',driver.PROJECT/'Scripts/VFXIntegratedAcceptance.py')
            module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        class Light:
            def GetUUID(self): raise RuntimeError('destroyed')
        for audio_live in (False,True):
            tracks={'particles':{'handle':1},'sound':{'handle':2},'light':{'handle':Light()}}
            service=SimpleNamespace(requests={'one':{'active':tracks}})
            service.stop=lambda:service.requests['one'].update(active={})
            obj=module.VFXIntegratedAcceptance.__new__(module.VFXIntegratedAcceptance)
            obj.service=service;obj.camera_baseline=[0,0,0]
            obj.owner=SimpleNamespace(GetComponent=lambda name:SimpleNamespace(Translation=SimpleNamespace(x=0,y=0,z=0)))
            values=[];obj._write=values.append
            fake.Audio.IsPlaying=lambda h:audio_live
            job=obj._probe(dict(action='cleanup',sequence=1,nonce='test'))
            next(job)
            if audio_live:
                with self.assertRaisesRegex(RuntimeError,'survived cancellation'): next(job)
            else:
                with self.assertRaises(StopIteration): next(job)
                self.assertTrue(values[0]['cleanup']['cameraRestored'])
                self.assertEqual(values[0]['cleanup']['checkedRequests'],1)

    def test_saved_environment_create_omits_all_readonly_fields(self):
        environment=driver.read(driver.PROJECT/'Assets/Scenes/VFXAuthoringPreview.he')
        checked=set()
        for entity in environment.values():
            if not {'CameraComponent','DirectionalLightComponent'} & entity.keys(): continue
            original={k:v for k,v in entity.items() if k!='TagComponent'}
            before=copy.deepcopy(original)
            editable=driver.editable_create_components(original)
            self.assertEqual(original,before)
            for component,fields in editable.items():
                checked.add(component)
                self.assertFalse({'ParentUuid','MaterialOverrides','meshResPath'}&set(fields))
                if component=='CameraComponent':
                    self.assertNotIn('mAspectRatio',fields['Camera'])
                    self.assertEqual(fields['Camera']['mPerspectiveFOV'],original[component]['Camera']['mPerspectiveFOV'])
                elif component=='DirectionalLightComponent': self.assertEqual(fields,original[component])
            self.assertEqual(editable['TransformComponent']['Translation'],original['TransformComponent']['Translation'])
        self.assertTrue({'CameraComponent','DirectionalLightComponent','TransformComponent'}.issubset(checked))

    def test_create_does_not_silently_drop_parent_or_mesh_authoring(self):
        for value in ({'TransformComponent':{'ParentUuid':'existing-parent'}},
                      {'MeshComponent':{'meshResPath':'model.hmesh'}},
                      {'MeshComponent':{'MaterialOverrides':['surface.hmat']}}):
            with self.assertRaises(ValueError): driver.editable_create_components(value)
        socket={'TransformComponent':{'Translation':[0,.1,0],'Rotation':[0,0,0],'Scale':[1,1,1]}}
        self.assertEqual(driver.editable_create_components(socket),socket)

    def test_actual_stop_hook_uses_saved_identity_after_owner_dies(self):
        import ast
        from types import SimpleNamespace
        for filename,typename,registry in [('VFXCharacterAcceptance.py','VFXCharacterAcceptance','STARTED_ACTORS'),
                                           ('VFXIntegratedAcceptance.py','VFXIntegratedAcceptance','STARTED')]:
            tree=ast.parse((driver.PROJECT/'Scripts'/filename).read_text(encoding='utf-8'))
            cls=next(node for node in tree.body if isinstance(node,ast.ClassDef) and node.name==typename)
            self.assertIn('Stop',[node.name for node in cls.body if isinstance(node,ast.FunctionDef)])
            stopped=[];destroyed=[]
            environment={registry:set(),'PresentationService':lambda:SimpleNamespace(stop=lambda:stopped.append(True)),
                'HEngine':SimpleNamespace(Destroy=destroyed.append),'xyz':lambda v:[0,0,0]}
            exec(compile(ast.Module(body=[cls],type_ignores=[]),filename,'exec'),environment)
            runtime=environment[typename]()
            class Owner:
                alive=True
                def GetUUID(self):
                    if not self.alive: raise RuntimeError('dead owner')
                    return 'saved-owner'
                def GetComponent(self,name): return SimpleNamespace(Tag='CharacterActorA',Translation=None)
            owner=Owner();runtime.Start(owner)
            self.assertEqual(environment[registry],{'saved-owner'})
            owner.alive=False
            # Also exercise a coordinator carrying resources while its owner is dead.
            runtime.service=SimpleNamespace(stop=lambda:stopped.append(True))
            runtime.owned_instances=['task-instance']
            runtime.Stop()
            self.assertFalse(environment[registry])
            self.assertEqual(len(stopped),1)
            if typename=='VFXCharacterAcceptance': self.assertEqual(destroyed,['task-instance'])

    def test_inherited_presentation_stop_tolerates_partial_start(self):
        import ast
        from types import SimpleNamespace
        tree=ast.parse((driver.PROJECT/'Scripts/PresentationPlayer.py').read_text(encoding='utf-8'))
        cls=next(node for node in tree.body if isinstance(node,ast.ClassDef) and node.name=='PresentationPlayer')
        method=next(node for node in cls.body if isinstance(node,ast.FunctionDef) and node.name=='Stop')
        environment={};exec(compile(ast.Module(body=[method],type_ignores=[]),'PresentationPlayer.Stop','exec'),environment)
        environment['Stop'](SimpleNamespace())
        called=[];environment['Stop'](SimpleNamespace(service=SimpleNamespace(stop=lambda:called.append(True))))
        self.assertEqual(called,[True])
        demo=ast.parse((driver.PROJECT/'Scripts/PresentationDemo.py').read_text(encoding='utf-8'))
        demo_class=next(node for node in demo.body if isinstance(node,ast.ClassDef))
        self.assertEqual(demo_class.bases[0].id,'PresentationPlayer')
        self.assertNotIn('Stop',[node.name for node in demo_class.body if isinstance(node,ast.FunctionDef)])

    def test_actual_environment_operations_include_fixed_exposure(self):
        source=driver.read(driver.PROJECT/'Assets/Scenes/VFXAuthoringPreview.he')
        operations=driver.environment_operations(source)
        self.assertEqual(len(operations),2)
        self.assertFalse(any('EnvironmentLightComponent' in op['components'] for op in operations))
        camera=next(op['components']['CameraComponent'] for op in operations if 'CameraComponent' in op['components'])
        self.assertIs(camera['bOverrideExposure'],True)
        self.assertEqual(camera['ExposureEV100'],.3219280948873623)
        self.assertEqual(camera['ExposureCompensation'],0.0)
        self.assertNotIn('mAspectRatio',camera['Camera'])
        for op in operations:
            self.assertNotIn('ParentUuid',op['components']['TransformComponent'])
            self.assertTrue(set(op['components']).issubset({'CameraComponent','DirectionalLightComponent','TransformComponent','PythonScriptComponent'}))
        self.assertEqual(sum('DirectionalLightComponent' in op['components'] for op in operations),1)
        self.assertEqual(source,driver.read(driver.PROJECT/'Assets/Scenes/VFXAuthoringPreview.he'))
        self.assertIn(str((driver.PROJECT/'Assets/Scenes/VFXAuthoringPreview.he').resolve()),driver.input_hashes(Path(driver.__file__)))

    def test_actual_display_key_and_actor_yaw_are_authored(self):
        import math
        source=driver.read(driver.PROJECT/'Assets/Scenes/VFXAuthoringPreview.he')
        operations=driver.environment_operations(source)
        lights=[op for op in operations if 'DirectionalLightComponent' in op['components']]
        self.assertEqual(len(lights),1)
        light=lights[0]['components']
        self.assertEqual(light['DirectionalLightComponent']['Intensity'],4.0)
        self.assertEqual(light['DirectionalLightComponent']['Color'],[1,1,1])
        self.assertIs(light['DirectionalLightComponent']['bCastShadows'],True)
        self.assertEqual(light['TransformComponent']['Rotation'],[1.919862177,-.436332313,0])
        actor=driver.actor_display_operation('source-actor')
        self.assertEqual(actor,dict(op='transform',entity='source-actor',patch=dict(Scale=[.01,.01,.01],Rotation=[0,math.pi,0]),space='local',mode='absolute'))
        self.assertEqual(source,driver.read(driver.PROJECT/'Assets/Scenes/VFXAuthoringPreview.he'))

    def test_glm_equivalent_serialized_euler_passes_but_wrong_direction_fails(self):
        import math
        actual=dict(Rotation=[3.1415927410125732,-8.742277657347586e-08,3.1415927410125732],Scale=[.009999999776]*3)
        self.assertTrue(driver.display_transform_matches(actual,[.01]*3,[0,math.pi,0]))
        for wrong in ([0,0,0],[0,math.pi*.5,0],[math.pi,0,0]):
            self.assertFalse(driver.display_transform_matches(dict(actual,Rotation=wrong),[.01]*3,[0,math.pi,0]))
        self.assertFalse(driver.display_transform_matches(dict(actual,Scale=[.011]*3),[.01]*3,[0,math.pi,0]))
        self.assertFalse(driver.display_transform_matches(dict(actual,Rotation=[float('nan'),0,0]),[.01]*3,[0,math.pi,0]))
        # GLM's +90 degree yaw maps local +Z to world +X.
        matrix=driver.rotation_matrix([0,math.pi*.5,0])
        self.assertAlmostEqual(matrix[2],1)
        self.assertAlmostEqual(matrix[6],-1)

    def test_controller_missing_skeleton_cannot_silently_create_unloadable_fixture(self):
        for skeleton in ('',None,'../outside.hskel','Assets/wrong.hanim'):
            with self.assertRaises(ValueError): driver.controller('Assets/IntegratedAcceptance/id/Walk.hanim',skeleton)
        for version in ('v1','v2'):
            skeleton='Assets/Imported/id/Resources/'+version+'.hskel'
            value=driver.controller('Assets/IntegratedAcceptance/id/Walk-'+version+'.hanim',skeleton)
            self.assertEqual(value['targetSkeletonPath'],skeleton)

    def test_runtime_positions_follow_public_return_types(self):
        import ast
        from types import SimpleNamespace
        tree=ast.parse((driver.PROJECT/'Scripts/VFXIntegratedAcceptance.py').read_text())
        function=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='xyz')
        env={};exec(compile(ast.Module(body=[function],type_ignores=[]),'position conversion','exec'),env)
        for value in ((1,2,3),SimpleNamespace(x=1,y=2,z=3)):
            self.assertEqual(env['xyz'](value),[1.0,2.0,3.0])
        with self.assertRaises(ValueError): env['xyz']((1,2))

    def test_runtime_controller_readiness_is_bounded_and_observed(self):
        import ast
        from types import SimpleNamespace
        tree=ast.parse((driver.PROJECT/'Scripts/VFXIntegratedAcceptance.py').read_text())
        function=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='wait_for_controller')
        env={};exec(compile(ast.Module(body=[function],type_ignores=[]),'controller readiness','exec'),env)
        loaded=False
        actor=SimpleNamespace(GetClipCount=lambda:1 if loaded else 0,get_current_state=lambda:'Walk' if loaded else '',GetProgress=lambda:0)
        pending=env['wait_for_controller']([actor,actor]);next(pending)
        loaded=True
        with self.assertRaises(StopIteration) as done: next(pending)
        self.assertEqual([v['clipCount'] for v in done.exception.value],[1,1])
        loaded=False;pending=env['wait_for_controller']([actor,actor])
        for _ in range(30): next(pending)
        with self.assertRaisesRegex(RuntimeError,'did not load'): next(pending)

    def test_source_owned_conflict_requires_exact_guarded_restore(self):
        baseline=dict(Translation=[0,0,0],Rotation=[0,0,0],Scale=[1,1,1],ParentUuid='')
        previous={'actor':{'TransformComponent':baseline}}
        current=copy.deepcopy(previous)
        current['actor']['TransformComponent'].update(driver.actor_display_operation('actor')['patch'])
        with self.assertRaises(driver.content_sync.Failure) as failed:
            driver.content_sync.merge_scene(previous,current,previous,{}, {})
        self.assertEqual(failed.exception.diagnostic['code'],'owned_field_conflict')
        authored=copy.deepcopy(current['actor']['TransformComponent'])
        operation=driver.restore_source_display_operation('actor',authored,authored,baseline)
        self.assertEqual(operation,dict(op='transform',entity='actor',patch=dict(Scale=[1,1,1],Rotation=[0,0,0]),space='local',mode='absolute'))
        self.assertNotIn('Translation',operation['patch'])
        self.assertNotIn('ParentUuid',operation['patch'])
        current['actor']['TransformComponent'].update(operation['patch'])
        merged,_=driver.content_sync.merge_scene(previous,current,previous,{}, {})
        self.assertEqual(merged,previous)
        changed=copy.deepcopy(authored);changed['Rotation']=[0,.3,0]
        with self.assertRaisesRegex(ValueError,'changed externally'):
            driver.restore_source_display_operation('actor',changed,authored,baseline)
        self.assertEqual(driver.actor_display_operation('actor')['patch']['Rotation'],driver.DISPLAY_ACTOR_ROTATION)

    def test_fresh_effect_updates_before_pause_and_capture(self):
        from types import SimpleNamespace
        calls=[]
        checks=SimpleNamespace(session=object(),control=lambda h,action,**kw:calls.append((action,h,kw)),
                               state=lambda h:dict(playing=True))
        with patch.object(driver,'settled_step',lambda session,frames:calls.append(('step',frames))):
            result=driver.warm_effect(checks,17,[0,.8,-2])
        self.assertEqual([c[0] for c in calls],['transform','step','pause'])
        self.assertEqual(calls[1],('step',8))
        self.assertEqual(result['frames'],8)
        self.assertEqual(calls[0][1],calls[2][1])

    def test_visible_socket_and_warmup_fit_actual_camera_and_track_window(self):
        import math,ast
        matrix=driver.rotation_matrix(driver.DISPLAY_ACTOR_ROTATION)
        local=[v*.01 for v in driver.SOCKET_LOCAL]
        offset=[sum(matrix[i*3+k]*local[k] for k in range(3)) for i in range(3)]
        for a,b in zip(offset,[0,.8,-1.8]): self.assertAlmostEqual(a,b)
        source=driver.read(driver.PROJECT/'Assets/Scenes/VFXAuthoringPreview.he')
        camera=next(v for v in source.values() if 'CameraComponent' in v)
        rotation=driver.rotation_matrix(camera['TransformComponent']['Rotation'])
        pos=camera['TransformComponent']['Translation']
        fov=camera['CameraComponent']['Camera']['mPerspectiveFOV']
        for x in driver.INSTANCE_X:
            delta=[x+offset[0]-pos[0],offset[1]-pos[1],offset[2]-pos[2]]
            view=[sum(rotation[k*3+i]*delta[k] for k in range(3)) for i in range(3)]
            self.assertGreater(-view[2],0)
            self.assertLess(abs(view[0]/(-view[2]*math.tan(fov/2)*16/9)),.9)
            self.assertLess(abs(view[1]/(-view[2]*math.tan(fov/2))),.9)
        self.assertEqual(driver.INSTANCE_X,[ -3,3 ])
        composition=driver.composition_for('Assets/Effects/Authoring/task.efkpkg','task')
        self.assertLess(.01+driver.VISUAL_WARMUP_FRAMES/60,min(t['duration'] for t in composition['tracks']))
        tree=ast.parse((driver.PROJECT/'Scripts/VFXIntegratedAcceptance.py').read_text())
        declaration=next(n for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='VISUAL_WARMUP_FRAMES' for t in n.targets))
        self.assertEqual(ast.literal_eval(declaration.value),driver.VISUAL_WARMUP_FRAMES)

    def test_visible_capture_requires_actual_draw_and_vertex_metrics(self):
        from types import SimpleNamespace
        for stats,passes in [({'drawCalls':{'current':1},'vertices':{'current':4}},True),({},False),
                             ({'drawCalls':{'current':0},'vertices':{'current':0}},False)]:
            checks=SimpleNamespace(capture=lambda name:dict(path=name),call=lambda name:stats)
            if passes: self.assertEqual(driver.visible_capture(checks,'test')['stats'],stats)
            else:
                with self.assertRaises(RuntimeError): driver.visible_capture(checks,'test')

if __name__=='__main__': unittest.main()
