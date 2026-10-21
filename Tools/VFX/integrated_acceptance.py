"""Single-scene, single-session integrated acceptance. Never builds; runtime evidence only.

Generated recipes stay outside the canonical recipe library. This is not Editor
UI, animation hot reload, bone socket, or shipping performance evidence.
"""
from __future__ import annotations
import argparse
import ast
import copy
import json
import math
from pathlib import Path
import shutil
import subprocess
import re
import sys
import time
from types import SimpleNamespace
import uuid

from preview import ROOT, Session, Storage, digest, write_json
from prefab_acceptance import Transactions
from parameter_acceptance import Checks
from character_acceptance import owned_run_directory, validate_animation_observation
from resource_failure_acceptance import mutation
import authoring
sys.path.insert(0,str(ROOT/'Tools/Content'))
import content_sync
from baked_model_acceptance import stretch_clip, move_mouth_bind

PROJECT=ROOT/'Project'
SOURCE=PROJECT/'SourceAssets/Quaternius/CuteAnimatedMonsters/Derived/Cactus/Cactus.gltf'
DISPLAY_EXPOSURE_EV100=.3219280948873623
DISPLAY_KEY_ROTATION=[1.919862177,-.436332313,0]
DISPLAY_ACTOR_ROTATION=[0,math.pi,0]
VISUAL_WARMUP_FRAMES=8
INSTANCE_X=[-3.0,3.0]
SOCKET_LOCAL=[0,80,180]


def verify_pin(path,expected):
    if not re.fullmatch(r"[0-9a-fA-F]{64}",expected) or digest(path).lower()!=expected.lower():
        raise ValueError("Pinned input hash mismatch: "+str(path))


def input_hashes(importer):
    """Bounded static local import/control-script closure plus actual source assets."""
    seeds=[Path(__file__),PROJECT/'Scripts/VFXIntegratedAcceptance.py',SOURCE,Path(importer),
        ROOT/'Tools/Content/baked_model_adapter.py',ROOT/'Tools/VFX/recipes/hit-ember.recipe.json',
        ROOT/'Tools/VFX/recipes/toolchain.lock.json',ROOT/'Tools/VFX/recipes/templates/gpu-sprite.xml',
        ROOT/'Tools/VFX/recipes/InspectCompiled.ps1',ROOT/authoring.stress.SOURCE_LICENSE,
        ROOT/'Tools/Agent/hectl.ps1',PROJECT/'Assets/Effects/Authoring/CharacterDemo/Actor.hprefab',
        PROJECT/'Assets/Effects/Authoring/CharacterDemo/Footstep.json',
        PROJECT/'Assets/Effects/Authoring/Compositions/impact.json',
        PROJECT/'Assets/Scenes/VFXAuthoringPreview.he']
    seeds += [ROOT/path for path in read(ROOT/'Tools/VFX/recipes/toolchain.lock.json')['files']]
    search=[ROOT/'Tools/VFX',ROOT/'Tools/Content',ROOT/'Tools/Agent',PROJECT]
    pending=list(seeds);visited=set()
    while pending:
        path=pending.pop().resolve()
        if path in visited: continue
        if not path.is_file(): raise ValueError('Missing evidence input: '+str(path))
        visited.add(path)
        if len(visited)>512: raise ValueError('Input dependency closure exceeds bound')
        if path.suffix=='.py':
            tree=ast.parse(path.read_text(encoding='utf-8-sig'))
            for node in ast.walk(tree):
                names=[x.name for x in node.names] if isinstance(node,ast.Import) else [node.module] if isinstance(node,ast.ImportFrom) and node.module else []
                for name in names:
                    for base in [path.parent,*search]:
                        candidate=base/(name.replace('.', '/')+'.py')
                        if candidate.is_file(): pending.append(candidate);break
        elif path.suffix=='.ps1':
            for name in re.findall(r'[A-Za-z0-9_-]+\.ps1',path.read_text(encoding='utf-8-sig')):
                for base in [path.parent,ROOT/'Tools/Agent']:
                    candidate=base/name
                    if candidate.is_file(): pending.append(candidate);break
        elif path.suffix in ('.json','.hprefab','.he') and path.stat().st_size<=2*1024*1024:
            def paths(value):
                if isinstance(value,str) and value.startswith(('Assets/','Scripts/')) and (PROJECT/value).is_file(): pending.append(PROJECT/value)
                elif isinstance(value,dict):
                    for child in value.values(): paths(child)
                elif isinstance(value,list):
                    for child in value: paths(child)
            paths(read(path))
    return {str(path):digest(path) for path in sorted(visited)}


def editable_create_components(components):
    """Project saved values are not patches: omit viewport/parent-owned fields.

    Nonempty parent or mesh metadata needs dedicated operations, not silent loss.
    Mirrors the existing public SceneEditingService::PatchComponent contract.
    """
    result=copy.deepcopy(components)
    for component,fields in result.items():
        if 'ParentUuid' in fields:
            if fields['ParentUuid']:
                raise ValueError('Use create.parent for a nonempty parent reference')
            del fields['ParentUuid']
        if 'MaterialOverrides' in fields or 'meshResPath' in fields:
            raise ValueError('Use dedicated mesh/material author operations')
        if component=='CameraComponent' and 'Camera' in fields:
            fields['Camera'].pop('mAspectRatio',None)
    return result


def environment_operations(environment):
    operations=[]
    for index,value in enumerate(environment.values()):
        if not {'CameraComponent','DirectionalLightComponent'} & value.keys():
            continue
        components=copy.deepcopy({k:v for k,v in value.items() if k!='TagComponent'})
        if 'CameraComponent' in value:
            components['CameraComponent'].update(bOverrideExposure=True,ExposureEV100=DISPLAY_EXPOSURE_EV100,ExposureCompensation=0.0)
            components['PythonScriptComponent']=dict(Path='Scripts/VFXIntegratedAcceptance.py')
            name='IntegratedCoordinator'
        else:
            components['DirectionalLightComponent'].update(Intensity=4.0,Color=[1.0,1.0,1.0],bCastShadows=True)
            components['TransformComponent']['Rotation']=list(DISPLAY_KEY_ROTATION)
            name='IntegratedLight'
        operations.append(dict(op='create',temporaryId='$environment'+str(index),name=name,
            components=editable_create_components(components)))
    return operations


def rotation_matrix(euler):
    """Rows of glm::toMat3(glm::quat(vec3)): Euler XYZ in radians."""
    if len(euler)!=3 or not all(math.isfinite(v) for v in euler):
        raise ValueError('Expected three finite Euler angles')
    cx,cy,cz=[math.cos(v*.5) for v in euler]
    sx,sy,sz=[math.sin(v*.5) for v in euler]
    w=cx*cy*cz+sx*sy*sz
    x=sx*cy*cz-cx*sy*sz
    y=cx*sy*cz+sx*cy*sz
    z=cx*cy*sz-sx*sy*cz
    return [1-2*(y*y+z*z),2*(x*y-w*z),2*(x*z+w*y),
            2*(x*y+w*z),1-2*(x*x+z*z),2*(y*z-w*x),
            2*(x*z-w*y),2*(y*z+w*x),1-2*(x*x+y*y)]


def display_transform_matches(actual,scale,rotation,tolerance=1e-6):
    actual_scale=actual.get('Scale',[])
    if len(actual_scale)!=3 or len(scale)!=3 or not all(math.isfinite(v) for v in actual_scale): return False
    if any(abs(a-b)>tolerance for a,b in zip(actual_scale,scale)): return False
    try:
        actual_matrix=rotation_matrix(actual.get('Rotation',[]))
        desired_matrix=rotation_matrix(rotation)
    except (ValueError,TypeError): return False
    return all(abs(a-b)<=tolerance for a,b in zip(actual_matrix,desired_matrix))


def actor_display_operation(entity):
    return dict(op='transform',entity=entity,patch=dict(Scale=[.01,.01,.01],Rotation=list(DISPLAY_ACTOR_ROTATION)),
        space='local',mode='absolute')


def restore_source_display_operation(entity,current,authored,baseline):
    """Restore only this driver's scale/rotation after exact ownership checks."""
    fields=('Scale','Rotation')
    if any(current.get(field)!=authored.get(field) for field in fields):
        raise ValueError('Source display transform changed externally; refusing restoration')
    if any(field not in baseline for field in fields):
        raise ValueError('Missing original imported source transform baseline')
    return dict(op='transform',entity=entity,patch={field:copy.deepcopy(baseline[field]) for field in fields},
        space='local',mode='absolute')


def composition_for(package,ident):
    composition=read(PROJECT/'Assets/Effects/Authoring/CharacterDemo/Footstep.json')
    composition['id']='integrated-'+ident
    for track in composition['tracks']:
        if track['kind']=='vfx': track['parameters']['path']=package
    impact=read(PROJECT/'Assets/Effects/Authoring/Compositions/impact.json')
    composition['tracks'].append(copy.deepcopy(next(t for t in impact['tracks'] if t['kind']=='shake')))
    if {t['kind'] for t in composition['tracks']}!={'vfx','audio','light','shake'}:
        raise ValueError('Integrated presentation requires all four tracks')
    return composition


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def atomic_json(path,value):
    content_sync.atomic(Path(path),(json.dumps(value,ensure_ascii=False,indent=2)+'\n').encode('utf-8'))


def derive_clip(original):
    """Only events are authored; channels, skeleton, mesh and timebase are identical."""
    result=copy.deepcopy(original)
    if len(result.get('clips',[]))!=1:
        raise ValueError('Expected one baked clip per stable source animation ID')
    clip=result['clips'][0]
    if clip.get('events'):
        raise ValueError('Source already has events; refusing to erase authored data')
    duration=clip['duration']/clip['ticksPerSecond']
    if not 0<duration<=120:
        raise ValueError('Unsupported clip duration')
    clip['events']=[dict(id='integrated-step',name='footstep',timeStamp=clip['duration']*.25,payload='socket')]
    comparison=copy.deepcopy(result)
    if 'events' in original['clips'][0]: comparison['clips'][0]['events']=original['clips'][0]['events']
    else: comparison['clips'][0].pop('events')
    if comparison!=original:
        raise ValueError('Derived animation changed non-event data')
    return result,duration


def controller(path,skeleton):
    if not isinstance(skeleton,str) or not skeleton.startswith('Assets/') or not skeleton.endswith('.hskel') or '..' in skeleton:
        raise ValueError('Controller requires the actual imported targetSkeletonPath')
    return dict(version=1,targetSkeletonPath=skeleton,name='IntegratedWalk',entryNodeId=1,
        nodes=[dict(id=1,type='Entry',name='Entry',clipPath='',position=[0,0]),
               dict(id=2,type='State',name='Walk',clipPath=path,position=[320,0])],
        transitions=[dict(fromNodeId=1,toNodeId=2,conditions=[],duration=0)],parameters=[])


def settled_step(session,frames=1):
    ticket=session.call('runtime.step',dict(frames=frames))
    deadline=time.monotonic()+60
    while time.monotonic()<deadline:
        state=session.call('session.status')['world']['runtimeControl']
        if state['completedSteps']>=ticket['targetCompletedSteps'] and not state['pendingSteps'] and not state['stepInProgress']:
            return
        time.sleep(.05)
    raise TimeoutError('Runtime step did not reach completed barrier')


def reload_scene(session,scene):
    before=session.call('world.summary')['sceneGeneration']
    session.call('runtime.scene.load',dict(path=scene,forceReload=True))
    for _ in range(30):
        settled_step(session)
        summary=session.call('world.summary')
        if summary['sceneGeneration']!=before:
            return summary
    raise TimeoutError('Same-scene forced reload did not advance generation')


def export(tx,entity,path):
    request=dict(sceneId=tx.state['sceneId'],baseRevision=tx.state['revision'],requestId=uuid.uuid4().hex,entity=entity,path=path)
    receipt=tx.call('export_prefab',request)
    tx.check('export.'+Path(path).stem,receipt==tx.call('export_prefab',request) and receipt['sourceRevision']==tx.state['revision'],receipt=receipt)
    return receipt


def visible_capture(checks,name):
    capture=checks.capture(name)
    capture['stats']=checks.call('vfx.stats')
    for metric in ('drawCalls','vertices'):
        current=capture['stats'].get(metric,{}).get('current')
        if type(current) not in (int,float) or not math.isfinite(current) or current<=0:
            raise RuntimeError('Capture lacks actual positive VFX '+metric)
    return capture


def warm_effect(checks,handle,position):
    checks.control(handle,'transform',position=position,forward=[0,0,-1])
    settled_step(checks.session,VISUAL_WARMUP_FRAMES)
    checks.control(handle,'pause')
    state=checks.state(handle)
    if state.get('playing') is not True: raise RuntimeError('Fresh effect did not survive visual warmup')
    return dict(frames=VISUAL_WARMUP_FRAMES,position=position,state=state)


def probe(session,directory,report,sequence,presentation,duration,socket,action="probe"):
    nonce=uuid.uuid4().hex
    folder=directory/'Saved/Reports';folder.mkdir(parents=True,exist_ok=True)
    atomic_json(folder/'VFXIntegratedCommand.json',dict(sequence=sequence,nonce=nonce,presentation=presentation,duration=duration,socketLocalId=socket,action=action))
    for _ in range(30):
        settled_step(session,VISUAL_WARMUP_FRAMES)
        path=folder/'VFXIntegratedReceipt.json'
        if path.exists():
            receipt=read(path)
            if receipt.get('sequence')!=sequence: continue
            if receipt.get('status')!='passed' or receipt.get('nonce')!=nonce:
                raise RuntimeError('Runtime integrated probe failed: '+json.dumps(receipt))
            if action=='cleanup':
                if receipt.get('cleanup',{}).get('checkedRequests',0)<2: raise RuntimeError('Cleanup did not inspect two active presentations')
                report['finalPresentationCleanup']=receipt
                return receipt
            observation=validate_animation_observation(session.call('render.animation.status'))
            capture=visible_capture(Checks(session,report),'integrated-'+str(sequence))
            report.setdefault('runtimeStages',[]).append(dict(receipt=receipt,animation=observation,capture=capture,
                world=session.call('world.summary')))
            return receipt
    raise TimeoutError('Integrated runtime probe failed to produce matching receipt')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend',choices=['DX12','Vulkan'],required=True)
    parser.add_argument('--existing-build-sha256',required=True)
    parser.add_argument('--importer',type=Path,required=True)
    parser.add_argument('--importer-sha256',required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--timeout',type=int,default=600)
    args=parser.parse_args()
    executable=ROOT/'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'
    if digest(executable).lower()!=args.existing_build_sha256.lower(): parser.error('Pinned executable mismatch')
    verify_pin(args.importer,args.importer_sha256)
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    ident=uuid.uuid4().hex
    assets=PROJECT/'Assets/IntegratedAcceptance'/ident;assets.mkdir(parents=True)
    sources=PROJECT/'SourceAssets/VFXAuthoring'/('integrated-'+ident);sources.mkdir(parents=True)
    report=dict(schemaVersion=1,evidenceType='runtime-manual',commandStatus='RUNNING',visualStatus='NOT_REVIEWED',
        backend=args.backend,checks=[],visualEvidence=[],scope='one imported character lineage, same scene, one continuous session',
        limitations=['entity-local socket, not bone socket','explicit versioned controller mapping, not animation hot reload','Game author snapshot transactions, not Editor UI'])
    original_hash=digest(SOURCE)
    protected=input_hashes(args.importer.resolve())
    report['inputHashes']=protected
    report['importerSha256']=args.importer_sha256.lower()
    report['inputHashScope']='bounded static local import/control-script closure, tool lock and referenced input resources'
    session=None
    with Storage(ROOT).workflow_scope(str(PROJECT),'vfx-integrated',[output,assets,sources]) as storage:
        try:
            source=sources/'Cactus.gltf';document=read(SOURCE)
            for collection in ('scenes','nodes','meshes','materials','skins','animations'):
                for item in document.get(collection,[]): item.get('extras',{}).pop('he_id',None)
            atomic_json(source,document);content_sync.initialize_gltf(source);document=read(source)
            document_id=document['scenes'][document.get('scene',0)]['extras']['he_id']
            walk=next(a['extras']['he_id'] for a in document['animations'] if a['name']=='Walk')
            state_path=PROJECT/'.he-content'/document_id/'state.json'
            scene='Assets/Imported/'+document_id+'/Scene.he'
            sync_args=SimpleNamespace(action='sync',project=PROJECT,source=source,importer=args.importer.resolve(),blender='',
                mode='baked-model',default_clip_id=walk,initialize_ids=False,verify='none',timeout=args.timeout,fault='')
            def synchronize(label):
                verify_pin(args.importer,args.importer_sha256)
                folder=output/label;folder.mkdir()
                content_sync.synchronize(sync_args,folder)
                return read(state_path)
            state=synchronize('import-v1')
            report['importV1Artifacts']=state['artifacts']
            actor=next(k for k,v in read(PROJECT/scene).items() if 'AnimatorComponent' in v)
            source_transform=copy.deepcopy(state['baseline'][actor]['TransformComponent'])
            report['sourceTransformV1']=source_transform
            def mapped(version,state):
                derived,duration=derive_clip(read(PROJECT/state['clips'][walk]['path']))
                path=assets/('Walk-'+version+'.hanim');atomic_json(path,derived)
                ctrl=assets/('Controller-'+version+'.hanimctrl');atomic_json(ctrl,controller(path.relative_to(PROJECT).as_posix(),derived['targetSkeletonPath']))
                report.setdefault('clipMappings',[]).append(dict(stableClipId=walk,source=state['clips'][walk]['path'],
                    sourceSha256=digest(PROJECT/state['clips'][walk]['path']),derived=path.relative_to(PROJECT).as_posix(),
                    derivedSha256=digest(path),controller=ctrl.relative_to(PROJECT).as_posix(),controllerSha256=digest(ctrl),
                    targetSkeletonPath=derived['targetSkeletonPath'],duration=duration,eventsOnly=True))
                return ctrl.relative_to(PROJECT).as_posix(),duration
            ctrl,duration=mapped('v1',state)
            recipe=read(ROOT/'Tools/VFX/recipes/hit-ember.recipe.json');recipe['id']='integrated-'+ident
            atomic_json(sources/'recipe-v1.json',recipe);report['compileV1']=authoring.build(recipe)
            package='Assets/Effects/Authoring/'+recipe['id']+'.efkpkg'
            presentation=assets/'Step.json'
            composition=composition_for(package,ident)
            atomic_json(presentation,composition)
            session=Session(output,args.backend,args.existing_build_sha256,scene=scene);session.start()
            directory=owned_run_directory(session,args.existing_build_sha256)
            tx=Transactions(session,report);tx.state=tx.call('open',dict(path=scene))
            actor_components=read(PROJECT/'Assets/Effects/Authoring/CharacterDemo/Actor.hprefab')['entities']['actor']
            report['visualFixture']=dict(instanceX=INSTANCE_X,socketLocal=SOCKET_LOCAL,
                socketScope='entity-local authored point; .01 scale and yaw pi place it .8m high / 1.8m toward negative Z, not a bone socket',warmupFrames=VISUAL_WARMUP_FRAMES)
            report['authorScale']=[.01,.01,.01]
            report['authorRotation']=list(DISPLAY_ACTOR_ROTATION)
            report['displayKeyLight']=dict(Rotation=list(DISPLAY_KEY_ROTATION),Intensity=4.0,Color=[1,1,1],bCastShadows=True)
            report['authorDisplayScope']='scene display overlay only; source-owned scale/rotation restored from v1 baseline before import and explicitly reapplied after import; engine-owned components and instance overrides must survive'
            operations=[actor_display_operation(actor),
                dict(op='patch',entity=actor,component='AnimatorComponent',patch=dict(controllerPath=ctrl,bPlay=False)),
                dict(op='add',entity=actor,component='PythonScriptComponent',patch=dict(Path='Scripts/VFXIntegratedAcceptance.py')),
                dict(op='add',entity=actor,component='Rigidbody3DComponent',patch=actor_components['Rigidbody3DComponent']),
                dict(op='add',entity=actor,component='CapsuleCollider3DComponent',patch=actor_components['CapsuleCollider3DComponent']),
                dict(op='create',temporaryId='$socket',name='IntegratedSocket',parent=actor,components={
                    'TransformComponent':dict(Translation=list(SOCKET_LOCAL),Rotation=[0,0,0],Scale=[1,1,1])})]
            environment=read(PROJECT/'Assets/Scenes/VFXAuthoringPreview.he')
            operations.extend(environment_operations(environment))
            report['displayExposure']=dict(component='CameraComponent',bOverrideExposure=True,
                ExposureEV100=DISPLAY_EXPOSURE_EV100,ExposureCompensation=0.0,
                sourceScene='Assets/Scenes/VFXAuthoringPreview.he',scope='explicit camera display override; no EnvironmentLightComponent author adapter')
            for operation in operations:
                if operation['op']=='create':
                    operation['components']=editable_create_components(operation.get('components',{}))
            tx.apply(operations,'author-source');socket=tx.state['temporaryIds']['$socket']
            prefab=(assets/'Actor.hprefab').relative_to(PROJECT).as_posix()
            export(tx,actor,prefab)
            tx.apply([dict(op='prefab',temporaryId='$a',path=prefab),dict(op='prefab',temporaryId='$b',path=prefab)],'instantiate')
            roots=[tx.state['temporaryIds'][key] for key in ('$a','$b')]
            overrides=[]
            for index,root in enumerate(roots):
                overrides += [dict(op='rename',entity=root,name='IntegratedActor'+('A','B')[index]),
                    dict(op='transform',entity=root,patch=dict(Translation=[INSTANCE_X[index],0,0]),space='local',mode='absolute'),
                    dict(op='prefab_override',entity=root,pointer='/TagComponent/Tag',mode='set'),
                    dict(op='prefab_override',entity=root,pointer='/TransformComponent/Translation',mode='set')]
            tx.apply(overrides,'instance-overrides');tx.write('save')
            report['scene']=scene;report['sourceActor']=actor;report['instanceRoots']=roots
            actual_display=read(PROJECT/scene)[actor]['TransformComponent']
            tx.check('author_display_transform_applied',display_transform_matches(actual_display,
                report['authorScale'],report['authorRotation']),actual=actual_display,
                rotationComparison='glm Euler XYZ quaternion rotation matrix, element tolerance 1e-6',scaleTolerance=1e-6)
            (directory/'Saved/Reports/VFXIntegratedCommand.json').unlink(missing_ok=True)
            reload_scene(session,scene)
            first=probe(session,directory,report,1,presentation.relative_to(PROJECT).as_posix(),duration,socket)
            before_restore=read(PROJECT/scene)
            tx.state=tx.call('open',dict(path=scene))
            current_source=tx.query()[actor]['TransformComponent']
            restore=restore_source_display_operation(actor,current_source,actual_display,source_transform)
            tx.apply([restore],'restore-source-display-before-reimport');tx.write('save')
            saved_before=read(PROJECT/scene)
            tx.check('restore_only_owned_display_fields',all(saved_before[r]==before_restore[r] for r in roots) and
                all(saved_before[actor]['TransformComponent'][field]==source_transform[field] for field in ('Scale','Rotation')) and
                {k:v for k,v in saved_before[actor]['TransformComponent'].items() if k not in ('Scale','Rotation')}==
                {k:v for k,v in before_restore[actor]['TransformComponent'].items() if k not in ('Scale','Rotation')} and
                {k:v for k,v in saved_before[actor].items() if k!='TransformComponent'}==
                {k:v for k,v in before_restore[actor].items() if k!='TransformComponent'},operation=restore)
            atomic_json(source,move_mouth_bind(stretch_clip(document,walk,1.25)))
            new_state=synchronize('reimport-v2')
            report['importV2Artifacts']=new_state['artifacts']
            merged=read(PROJECT/scene)
            tx.check('reimport_preserves_author_fields_and_instances',all(merged[r]==saved_before[r] for r in roots) and
                merged[actor]['TransformComponent']==new_state['baseline'][actor]['TransformComponent'] and
                all(merged[actor][component]==saved_before[actor][component] for component in
                    ('PythonScriptComponent','Rigidbody3DComponent','CapsuleCollider3DComponent')) and
                merged[actor]['AnimatorComponent']['controllerPath']==ctrl and
                merged[actor]['MeshComponent']!=saved_before[actor]['MeshComponent'])
            ctrl,duration=mapped('v2',new_state)
            tx.state=tx.call('open',dict(path=scene))
            tx.apply([actor_display_operation(actor),dict(op='patch',entity=actor,component='AnimatorComponent',patch=dict(controllerPath=ctrl))],'reapply-display-and-map-controller-v2')
            replacement=(assets/'Actor-v2.hprefab').relative_to(PROJECT).as_posix();export(tx,actor,replacement)
            # Update only our own template resource. Scene updates still use real prefab_sync transactions.
            atomic_json(PROJECT/prefab,read(PROJECT/replacement))
            tx.apply([dict(op='prefab_sync',entity=root) for root in roots],'sync-prefab-v2');tx.write('save')
            synced=read(PROJECT/scene)
            tx.check('display_explicitly_reapplied_after_import',all(display_transform_matches(
                synced[entity]['TransformComponent'],report['authorScale'],report['authorRotation']) for entity in [actor,*roots]))
            tx.check('sync_preserves_distinct_overrides',all(synced[r]['TagComponent']==saved_before[r]['TagComponent'] and
                synced[r]['TransformComponent']['Translation']==saved_before[r]['TransformComponent']['Translation'] and
                synced[r]['AnimatorComponent']['controllerPath']==ctrl for r in roots))
            (directory/'Saved/Reports/VFXIntegratedCommand.json').unlink(missing_ok=True)
            reload_scene(session,scene)
            for handle in first['handles']:
                tx.check('scene_reload_cleans_old_handle_'+str(handle),session.call('vfx.control',dict(handle=handle,action='status')).get('playing') is False)
            second=probe(session,directory,report,2,presentation.relative_to(PROJECT).as_posix(),duration,socket)
            checks=Checks(session,report)
            for handle in second['handles']: checks.control(handle,'pause')
            old_radius=checks.state(second['handles'][0])['authoredRadiusMeters']
            recipe2=copy.deepcopy(recipe);recipe2['compile']['radius_m']*=1.5;recipe2['compile']['color']=[40,180,255,255]
            atomic_json(sources/'recipe-v2.json',recipe2);report['compileV2']=authoring.build(recipe2)
            report['reloadV2']=session.call('vfx.reload',dict(path=package))
            fresh=checks.play(package)
            report['freshV2Warmup']=warm_effect(checks,fresh,[0,.8,-2])
            tx.check('reload_keeps_old_snapshot_new_uses_v2',checks.state(second['handles'][0])['authoredRadiusMeters']==old_radius and
                abs(checks.state(fresh)['authoredRadiusMeters']-recipe2['compile']['radius_m'])<1e-5)
            report['visualEvidence'].append(visible_capture(checks,'recipe-v2'))
            package_path=PROJECT/package;good=package_path.read_bytes();good_hash=digest(package_path)
            def invalid_compile(executable,source_path,binary_path):
                # Real compiler process gets malformed task-owned XML; no synthetic raised failure.
                Path(source_path).write_text('<EffekseerProject><broken>',encoding='utf-8')
                process=subprocess.run([str(executable),'-cui','-in',str(source_path),'-o',str(binary_path)],
                    cwd=executable.parent,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
                (output/'compiler-failure.stdout.log').write_bytes(process.stdout)
                (output/'compiler-failure.stderr.log').write_bytes(process.stderr)
                report['compilerFailureProcess']=dict(exitCode=process.returncode,outputExists=Path(binary_path).exists())
                if process.returncode==0 and Path(binary_path).exists():
                    raise RuntimeError('Malformed source was accepted; cannot claim compiler failure')
                raise authoring.RecipeError('Actual malformed XML compiler rejected input')
            try:
                authoring.build(recipe2,force=True,compiler=invalid_compile)
            except Exception as error:
                report['compilerFailure']=dict(error=str(error),packageSha256=digest(package_path))
            else: raise RuntimeError('Malformed XML unexpectedly compiled')
            tx.check('compiler_failure_keeps_published_package', 'compilerFailureProcess' in report and
                (report['compilerFailureProcess']['exitCode']!=0 or not report['compilerFailureProcess']['outputExists']) and
                digest(package_path)==good_hash and checks.state(fresh)['playing'])
            try:
                package_path.write_bytes(mutation(package_path,'outer_version'))
                checks.rejected('resource_reload_rejected','vfx.reload',path=package)
                retained=checks.play(package)
                report['retainedCacheWarmup']=warm_effect(checks,retained,[0,.8,-3])
                tx.check('resource_failure_retains_old_cache',checks.state(fresh)['playing'] and
                    abs(checks.state(retained)['authoredRadiusMeters']-recipe2['compile']['radius_m'])<1e-5)
                report['visualEvidence'].append(visible_capture(checks,'resource-failure-old-cache'))
            finally: package_path.write_bytes(good)
            session.call('vfx.reload',dict(path=package))
            probe(session,directory,report,3,presentation.relative_to(PROJECT).as_posix(),duration,socket)
            probe(session,directory,report,4,presentation.relative_to(PROJECT).as_posix(),duration,socket,action='cleanup')
            tx.check('all_recorded_checks_pass',all(c['status']=='PASS' for c in report['checks']))
            report['commandStatus']='PASS'
        except Exception as error:
            report['commandStatus']='FAIL';report['error']=str(error)
            raise
        finally:
            if session is not None:
                try: session.stop();report['normalStop']=True
                except Exception as error: report['stopError']=str(error);report['commandStatus']='FAIL'
            report['protectedInputsUnchanged']=all(digest(Path(path))==value for path,value in protected.items())
            report['originalSourceUnchanged']=digest(SOURCE)==original_hash
            report['binaryUnchanged']=digest(executable).lower()==args.existing_build_sha256.lower()
            if not report['originalSourceUnchanged'] or not report['binaryUnchanged'] or not report['protectedInputsUnchanged']: report['commandStatus']='FAIL'
            errors=[];animation_errors=[]
            for name in ('game.stdout.log','game.stderr.log'):
                path=output/name
                if path.exists():
                    with path.open(encoding='utf-8',errors='replace') as stream:
                        for line in stream:
                            if re.search(r'AnimationSystem:.*(?:failed|mismatch|missing|invalid)',line,re.I) and len(animation_errors)<32:
                                animation_errors.append(line.strip()[:1000])
                            if re.search(r'VUID-|VK_ERROR_DEVICE_LOST|DXGI_ERROR_DEVICE_REMOVED|GPU validation error',line,re.I):
                                errors.append(line.strip()[:1000])
                                if len(errors)>=32: break
            report['nativeValidationErrors']=errors
            report['nativeAnimationErrors']=animation_errors
            if errors or animation_errors: report['commandStatus']='FAIL'
            report['commands']=session.commands if session else []
            storage['status']='unknown' if report.get('stopError') else 'success' if report['commandStatus']=='PASS' else 'failed'
            write_json(output/'report.json',report)
        if report['commandStatus']!='PASS': raise RuntimeError('Integrated acceptance failed')

if __name__=='__main__': main()
