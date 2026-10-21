"""Recipe-driven captures; command success never implies visual acceptance."""
import argparse
import json
import math
import os
from pathlib import Path
import shutil

from preview import ROOT, Session, Storage, digest, write_json
from authoring import RECIPES, validate

SCENES={'neutral':'Assets/Scenes/VFXAuthoringTemplateNeutral.he',
        'game':'Assets/Scenes/VFXAuthoringTemplateGame.he'}
TRAIL_OFFSETS={'neutral':[0,0,0], 'game':[10,.02,10]}


def play_position(scene_name, recipe):
    if scene_name=='neutral':
        return [0,0,0]
    # Ring sprites have zero local translation and fixed 90-degree X rotation.
    # Keep them above the actual meadow surface, rather than floating at hit height.
    height=.015 if recipe['template'] in ('telegraph','aura') else .5
    if recipe['template']=='environment':
        height=recipe['compile']['radius_m']+.015
    return [10,.02+height,10]


def stages(recipe):
    compile=recipe['compile']; speed=recipe['playback']['speed']
    if speed<=0: raise ValueError('Finite matrix captures require positive playback.speed')
    attack,sustain,decay=(compile[key] for key in ['attack_s','sustain_s','decay_s'])
    def frame(seconds): return max(1,math.ceil(seconds*60/speed))
    moments=[('start',1),('peak',frame(attack+sustain*.5)),
             ('decay',frame(attack+sustain+decay*.5)),('end',frame(attack+sustain+decay)+4)]
    grouped={}
    for name,index in moments: grouped.setdefault(index,[]).append(name)
    return sorted(grouped.items())


def trail_position(frame,end,radius):
    fraction=min(1,max(0,frame/end))
    return [radius*(fraction*2-1),.5,0], [1,0,0]


def selected_recipes(names):
    values=[]
    for path in sorted(RECIPES.glob('*.recipe.json')):
        value=validate(json.loads(path.read_text(encoding='utf-8')))
        if not names or value['id'] in names: values.append((path,value))
    if names and {value['id'] for _,value in values} != set(names):
        raise ValueError('Unknown recipe selector')
    return values


def capture(session,directory,label):
    # Captures share one session mailbox; effect prefixes prevent collisions
    # without overwriting captures from earlier recipes in this batch.
    receipt=session.call('capture.frame',dict(filename=directory.name+'-'+label+'.png'))
    destination=directory/(label+'.png')
    shutil.copyfile(receipt['path'],destination)
    return dict(path=str(destination),sha256=digest(destination),receipt=receipt,
        stats=session.call('vfx.stats'),readbackScope='capture.frame image only; no particle-state or fragment-overdraw readback')


def step(session,frames):
    while frames:
        chunk=min(120,frames); session.call('runtime.step',dict(frames=chunk)); frames-=chunk


def require(result, name, condition, **evidence):
    result.setdefault('checks',[]).append(dict(name=name,status='PASS' if condition else 'FAIL',**evidence))
    if not condition:
        raise RuntimeError(name)


def effect_capture(session,directory,recipe,scene_name='neutral'):
    directory.mkdir()
    path='Assets/Effects/Authoring/'+recipe['id']+'.efkpkg'
    result=dict(id=recipe['id'],package=path,packageSha256=digest(ROOT/'Project'/path),recipe=recipe,
        commandStatus='RUNNING',visualStatus='NOT_REVIEWED',captures=[],motion=[],
        playPosition=play_position(scene_name,recipe),trailOffset=TRAIL_OFFSETS[scene_name])
    handle=None
    try:
        params=recipe['playback']
        play=session.call('vfx.play',dict(path=path,seed=recipe['compile']['seed'],
            scale=params['scale'],speed=params['speed'],position=result['playPosition']))
        handle=play['handle']; result['play']=play
        if play.get('playing') is not True: raise RuntimeError('Effect is not playing')
        state=session.call('vfx.control',dict(handle=handle,action='status'))
        require(result,'public_color_support',state.get('supportsGlobalColor') is True,state=state)
        require(result,'play_position_and_speed',state.get('position') is not None and
                len(state['position'])==3 and all(math.isclose(a,b,abs_tol=1e-5) for a,b in
                zip(state['position'],result['playPosition'])) and state.get('speed')==params['speed'],state=state)
        response=session.call('vfx.control',dict(handle=handle,action='color',value=[1,1,1,1]))
        require(result,'neutral_color_accepted',response.get('accepted') is True,receipt=response)
        response=session.call('vfx.control',dict(handle=handle,action='parameter',name='intensity',value=params['intensity']))
        state=session.call('vfx.control',dict(handle=handle,action='status'))
        require(result,'playback_intensity',response.get('accepted') is True and
                state.get('intensity')==params['intensity'],receipt=response,state=state)
        current=0; moments=stages(recipe); end=moments[-1][0]
        for target,labels in moments:
            if recipe['template']=='trail':
                # At most ~12 motion segments, bounded by capture moments. The
                # emitter moves; no movement is simulated by editing screenshots.
                stride=max(1,math.ceil(end/12))
                while current<target:
                    count=min(stride,target-current)
                    position,forward=trail_position(current,end,recipe['compile']['radius_m'])
                    position=[value+offset for value,offset in zip(position,TRAIL_OFFSETS[scene_name])]
                    response=session.call('vfx.control',dict(handle=handle,action='transform',position=position,forward=forward))
                    result['motion'].append(dict(frame=current,position=position,forward=forward,receipt=response))
                    # End-stage advancement may follow a naturally expired handle.
                    if response.get('accepted') is not True and response.get('playing') is not False:
                        raise RuntimeError('Trail transform rejected on a live handle')
                    step(session,count); current+=count
            else:
                step(session,target-current); current=target
            image=capture(session,directory,'-'.join(labels))
            image.update(frame=target,stages=labels)
            result['captures'].append(image)
            state=session.call('vfx.control',dict(handle=handle,action='status'))
            image['effectStatus']=state
            require(result,'stage_lifecycle_'+str(target),state.get('playing') is ('end' not in labels),state=state)
            if 'peak' in labels:
                count=image['stats'].get('drawCalls',{}).get('current')
                require(result,'peak_draw_submission',isinstance(count,(int,float)) and count>0,
                        drawCalls=image['stats'].get('drawCalls'),scope='Renderer submission metric; not visible pixel or particle count')
        result['naturalEnd']=session.call('vfx.control',dict(handle=handle,action='status'))
        result['commandStatus']='PASS' if result['naturalEnd'].get('playing') is False else 'FAIL'
        if result['commandStatus']=='FAIL': result['error']='Still playing after recipe nominal end + 4 frames'
    except Exception as error:
        result['commandStatus']='FAIL'; result['error']=str(error)
    finally:
        try:
            if handle is not None:
                result['stop']=session.call('vfx.control',dict(handle=handle,action='stop'))
            step(session,32)
            result['retired']=capture(session,directory,'after-stop-retired')
            if handle is not None:
                result['stoppedStatus']=session.call('vfx.control',dict(handle=handle,action='status'))
                if result['stoppedStatus'].get('playing') is not False:
                    result['commandStatus']='FAIL'; result['cleanupError']='Effect still playing after stop'
        except Exception as error:
            result['commandStatus']='FAIL'; result['cleanupError']=str(error)
        write_json(directory/'report.json',result)
    return result


def recipe_groups(recipes):
    groups={}
    for _,recipe in recipes:
        key=(recipe['compile']['simulation'],recipe['compile']['quality'])
        groups.setdefault(key,[]).append(recipe)
    return sorted(groups.items())


def configure_quality(session, simulation, quality):
    stats=session.call('vfx.stats')
    if stats['activeEffects']['current'] != 0 or stats['cpuInstances']['current'] != 0:
        raise RuntimeError('Quality selection requires a quiescent effect baseline')
    expected=dict(tier=quality,requested=simulation=='gpu',enabled=simulation=='gpu' and quality!='Low')
    result=session.call('vfx.quality',dict(tier=quality))
    if result.get('accepted') is not True or any(result.get(key)!=value for key,value in expected.items()):
        raise RuntimeError('Runtime quality selection rejected or policy does not match recipe group')
    observed=session.call('vfx.quality')
    if any(observed.get(key)!=value for key,value in expected.items()):
        raise RuntimeError('Runtime quality query does not match selected recipe group')
    return dict(selection=result,observed=observed,quiescentStats=stats)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend',choices=['DX12','Vulkan'],required=True)
    parser.add_argument('--existing-build-sha256',required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--recipe',action='append',help='Exact recipe id; repeat to select several; default all24')
    parser.add_argument('--scene',choices=list(SCENES),action='append',help='Repeat; default neutral and game')
    args=parser.parse_args()
    executable=ROOT/'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'
    pinned=digest(executable)
    if pinned.lower()!=args.existing_build_sha256.lower(): parser.error('Candidate SHA256 mismatch')
    recipes=selected_recipes(args.recipe)
    scenes=list(dict.fromkeys(args.scene or SCENES))
    for _,recipe in recipes: stages(recipe)
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=False)
    hashes={str(path.relative_to(ROOT)):digest(path) for path,_ in recipes}
    hashes.update({'Project/Assets/Effects/Authoring/'+value['id']+'.efkpkg':
        digest(ROOT/'Project/Assets/Effects/Authoring'/(value['id']+'.efkpkg')) for _,value in recipes})
    hashes.update({'Project/'+SCENES[name]:digest(ROOT/'Project'/SCENES[name]) for name in scenes})
    for name in ('template_matrix.py', 'authoring.py', 'preview.py', 'generate_template_preview_scenes.py'):
        path=ROOT/'Tools/VFX'/name
        hashes[str(path.relative_to(ROOT))]=digest(path)
    hashes['Tools/Agent/storage.py']=digest(ROOT/'Tools/Agent/storage.py')
    report=dict(commandStatus='RUNNING',visualStatus='NOT_REVIEWED',backend=args.backend,
        configuration='RelWithDebInfo Dev Agent',executableSha256=pinned,inputHashes=hashes,groups=[],
        evidenceType='runtime-manual',fixedDeltaSeconds=1/60,
        renderStatsRecording=False,
        statisticsScope='Continuous frame CSV disabled; VFX snapshots and available GPU aggregate timings retained; visual-only matrix is not a performance benchmark',
        excluded=['shipping performance','particle buffer/state readback','fragment overdraw count','visual approval'])
    previous=os.environ.get('HE_VFX_GPU_PARTICLES')
    # One immutable outer record covers every child group and effect. keep5
    # cannot recycle earlier captures while this matrix is still in progress.
    with Storage(ROOT).workflow_scope(str(ROOT/'Project'),'vfx-template-matrix',[output]) as run:
        try:
            for scene_name in scenes:
                for (simulation,quality),selected in recipe_groups(recipes):
                    group_dir=output/(scene_name+'-'+simulation+'-'+quality.lower()); group_dir.mkdir()
                    group=dict(scene=SCENES[scene_name],simulation=simulation,quality=quality,effects=[])
                    report['groups'].append(group)
                    session=Session(group_dir,args.backend,pinned,scene=SCENES[scene_name],render_stats=False)
                    os.environ['HE_VFX_GPU_PARTICLES']='1' if simulation=='gpu' else '0'
                    try:
                        session.start()
                        group['session']=session.call('session.status')
                        group['qualityPolicy']=configure_quality(session,simulation,quality)
                        group['description']=session.call('vfx.describe')
                        group['empty']=capture(session,group_dir,'empty-before-effects')
                        if group['empty']['stats'].get('gpuEnabled') != (simulation=='gpu' and quality!='Low'):
                            raise RuntimeError('Runtime GPU enabled mode does not match group')
                        for recipe in selected:
                            result=effect_capture(session,group_dir/recipe['id'],recipe,scene_name)
                            group['effects'].append(dict(id=recipe['id'],commandStatus=result['commandStatus'],
                                report=str(group_dir/recipe['id']/'report.json')))
                            write_json(output/'report.json',report)
                            if result.get('cleanupError'): raise RuntimeError(result['cleanupError'])
                    finally:
                        try: session.stop()
                        except Exception as error:
                            report['cleanupError']=str(error); raise
            report['commandStatus']='PASS' if all(effect['commandStatus']=='PASS' for group in report['groups'] for effect in group['effects']) else 'FAIL'
            run['status']='success' if report['commandStatus']=='PASS' else 'failed'
        except Exception as error:
            report['commandStatus']='FAIL'; report['error']=str(error)
            run['status']='unknown' if report.get('cleanupError') else 'failed'
        finally:
            if previous is None: os.environ.pop('HE_VFX_GPU_PARTICLES',None)
            else: os.environ['HE_VFX_GPU_PARTICLES']=previous
            try:
                report['inputsUnchanged']=all(digest(ROOT/path)==value for path,value in hashes.items())
                report['binaryUnchanged']=digest(executable)==pinned
            except OSError as error:
                report['inputsUnchanged']=report['binaryUnchanged']=False
                report['hashCheckError']=str(error)
            if not report['inputsUnchanged'] or not report['binaryUnchanged']:
                report['commandStatus']='FAIL'
                if run['status']!='unknown': run['status']='failed'
            write_json(output/'report.json',report)
    print(json.dumps(dict(commandStatus=report['commandStatus'],visualStatus='NOT_REVIEWED',report=str(output/'report.json'))))
    return 0 if report['commandStatus']=='PASS' else 1


if __name__=='__main__':
    raise SystemExit(main())
