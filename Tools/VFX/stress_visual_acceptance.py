"""Hash-pinned official stress visual evidence; image approval is always manual."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import xml.etree.ElementTree as ET

from preview import ROOT, Session, Storage, digest, write_json
from lifecycle_acceptance import stress_identity, live_cycle_valid

SCENE = 'Assets/Scenes/VFXStressVisual.he'
SOURCE = ROOT/'Project/SourceAssets/VFXAuthoring/Stress'
EXE = ROOT/'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'


def stages(lifetime=200, owner_lifetime=1200):
    if lifetime != 200 or owner_lifetime != 1200:
        raise ValueError('This visual protocol requires the reviewed 200/1200 frame fixture')
    return [('plateau',201,False), ('paused-a',201,True), ('paused-b',201,True),
            ('resumed',231,False), ('mid-plateau',600,False),
            ('owner-before',1199,False), ('owner-after',1201,False),
            ('tail',1300,False), ('life-after',1401,False), ('natural-retired',1431,False)]


def step(session, count):
    if not isinstance(count,int) or count < 0:
        raise ValueError('Step count must be a nonnegative integer')
    while count:
        batch=min(count,120)
        session.call('runtime.step',dict(frames=batch))
        count-=batch


def owner_motion(xml_path, scale, position):
    node=ET.parse(xml_path).getroot().find('./Root/Children/Node')
    if node is None: raise ValueError('Missing reviewed stress owner node')
    for section,expected in [('LocationValues',.02),('RotationValues',10.)]:
        values=node.find(section)
        if values is None or values.findtext('Type')!='1':
            raise ValueError('Stress owner motion is no longer reviewed PVA')
        if any(float(values.findtext('PVA/Velocity/Y/'+field,'nan'))!=expected for field in ('Center','Min','Max')):
            raise ValueError('Stress owner PVA velocity differs from reviewed source')
    return dict(sourceXml=str(xml_path.relative_to(ROOT)),translationMode='PVA (Type=1)',
                velocityYPerFrame=.02,rotationYDegreesPerFrame=10.,
                estimatedOwnerY={str(age):position[1]+scale*.02*age for age in (0,201,600,1199,1200)},
                framing=framing_estimate(scale,position),
                interpretation='Authored owner motion scaled by runtime placement; root status.position stays fixed. Estimated trajectory is not a particle bounding-box or visibility guarantee.')


def framing_estimate(scale, position):
    # Shader applies the emitter transform to initial velocity, then adds world
    # gravity directly. Scaling down the emitter does not scale down gravity.
    lifetime=200; gravity=.0006; speed=.1*scale
    horizontal=speed*lifetime
    fall=gravity*lifetime*(lifetime+1)/2
    rise=speed*speed/(2*gravity)+speed
    padding=max(.5,.5*scale)
    lower=[position[0]-horizontal-padding,position[1]-horizontal-fall-padding,
           position[2]-horizontal-padding]
    upper=[position[0]+horizontal+padding,position[1]+.02*1200*scale+rise+padding,
           position[2]+horizontal+padding]
    camera=[0,-5.2,-37]
    nearest=lower[2]-camera[2]; farthest=upper[2]-camera[2]
    half_height=nearest*math.tan(math.pi/6)
    half_width=half_height*16/9
    required_y=max(abs(lower[1]-camera[1]),abs(upper[1]-camera[1]))
    required_x=max(abs(lower[0]),abs(upper[0]))
    if nearest<=.1 or farthest>=400 or required_y>=half_height or required_x>=half_width:
        raise ValueError('Estimated stress envelope exceeds fixed VFXStressVisual camera; choose a fitting scale/position or author and validate a different scene')
    return dict(units='world units; not physical meters',initialSpeedBound=.1,
                scaledInitialSpeedBound=speed,particleLifetimeFrames=lifetime,
                worldGravityPerFrame=[0,-gravity,0],gravityDisplacementBound=fall,
                gravityScope='World-space shader gravity is independent of runtime scale; damping reduces speed by .0001-.0003 per frame, not exponential drag. Bounds ignore damping.',
                estimatedAabb=dict(lower=lower,upper=upper,padding=padding),cameraPosition=camera,
                cameraRotationRadians=[math.pi,0,math.pi],verticalFovDegrees=60,aspectRatio=16/9,
                nearestDepth=nearest,verticalHalfView=half_height,requiredVerticalHalfExtent=required_y,
                verticalMarginFraction=(half_height-required_y)/half_height,
                estimateScope='Conservative analytic estimate with padding, not a GPU-readback strict bounding box; actual screenshots require manual clipping review')


def validate_camera(scene_path):
    scene=json.loads(scene_path.read_text(encoding='utf-8-sig'))
    cameras=[entity for entity in scene.values() if entity.get('CameraComponent',{}).get('Primary') is True]
    if len(cameras)!=1: raise ValueError('Stress visual scene must have one primary camera')
    camera=cameras[0]['CameraComponent']['Camera']; transform=cameras[0]['TransformComponent']
    pairs=[(transform['Translation'],[0,-5.2,-37]),(transform['Rotation'],[math.pi,0,math.pi]),
           (transform['Scale'],[1,1,1]),([camera['mPerspectiveFOV'],camera['mPerspectiveNear'],camera['mPerspectiveFar']],
                                    [math.pi/3,.1,400])]
    if camera['mProjectionType']!=0 or transform.get('ParentUuid','') or any(
            len(actual)!=len(expected) or any(abs(a-b)>1e-5 for a,b in zip(actual,expected)) for actual,expected in pairs):
        raise ValueError('Stress visual camera differs from the reviewed analytic framing')


def retired(stats):
    return (stats['activeEffects']['current']==0 and stats['cpuInstances']['current']==0 and
            all(stats.get('gpuAllocation',{}).get(key)==0 for key in ('allocatedEmitters','reservedParticles','reservedTrails')))


def image_difference(first, second):
    from PIL import Image, ImageChops, ImageStat
    with Image.open(first) as a, Image.open(second) as b:
        if a.size!=b.size: raise ValueError('Capture dimensions changed')
        difference=ImageChops.difference(a.convert('RGB'),b.convert('RGB'))
        return dict(meanAbsoluteRgb=sum(ImageStat.Stat(difference).mean)/3,
                    maximumRgb=max(hi for _,hi in difference.getextrema()),
                    interpretation='Pixel difference only; not automatic particle motion or visual approval')


def capture(session, output, label, age, host_steps, handle=None):
    receipt=session.call('capture.frame',dict(filename=label+'.png'))
    destination=output/(label+'.png')
    shutil.copyfile(receipt['path'],destination)
    return dict(stage=label,effectAgeFrames=age,requestedHostSteps=host_steps,
                path=str(destination),sha256=digest(destination),receipt=receipt,
                stats=session.call('vfx.stats'),
                effectStatus=session.call('vfx.control',dict(handle=handle,action='status')) if handle is not None else None)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend',choices=['Vulkan','DX12'],required=True)
    parser.add_argument('--nominal-particles',type=int,choices=[20000,50000],required=True)
    parser.add_argument('--existing-build-sha256',required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--scale',type=float,default=.35)
    parser.add_argument('--position',type=float,nargs=3,default=[0,-.4,0])
    parser.add_argument('--gpu-probe',action='store_true',help='Explicit developer probe; otherwise remove inherited GPU override')
    args=parser.parse_args()
    if not math.isfinite(args.scale) or not 0<args.scale<=1000 or any(not math.isfinite(v) for v in args.position):
        parser.error('Finite position and scale in (0,1000] required')
    pinned=digest(EXE)
    if pinned.lower()!=args.existing_build_sha256.lower(): parser.error('Executable hash mismatch')
    validate_camera(ROOT/'Project'/SCENE)
    name='GpuSparks'+str(args.nominal_particles//1000)+'k'
    effect='Assets/Effects/Stress/'+name+'.efkpkg'
    identity=stress_identity(ROOT/'Project'/effect,args.nominal_particles)
    metadata=json.loads((SOURCE/name/'PROVENANCE.json').read_text())
    parameters=metadata['parameters']
    schedule=stages(parameters['lifetime_frames'],parameters['owner_lifetime_frames'])
    manifest=json.loads((SOURCE/'manifest.json').read_text())
    entry=next(item for item in manifest['outputs'] if item['name']==name)
    if entry['package_sha256']!=identity['packageSha256'] or entry['validation']['binaryVersion']!=1810:
        raise ValueError('Official manifest package identity mismatch')
    motion=owner_motion(SOURCE/name/'roundtrip.xml',args.scale,args.position)
    inputs=[ROOT/'Project'/effect,ROOT/'Project'/SCENE,SOURCE/'manifest.json',
            SOURCE/name/'PROVENANCE.json',SOURCE/name/(name+'.efkproj'),SOURCE/name/(name+'.efkefc'),
            SOURCE/name/'roundtrip.xml',Path(__file__).resolve(),
            ROOT/'Tools/VFX/generate_effekseer_stress_assets.py',ROOT/'Tools/VFX/lifecycle_acceptance.py',
            ROOT/'Tools/VFX/preview.py',ROOT/'Tools/Agent/storage.py']
    hashes={str(path.relative_to(ROOT)):digest(path) for path in inputs}
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=False)
    session=Session(output,args.backend,pinned,scene=SCENE)
    report=dict(commandStatus='RUNNING',visualStatus='NOT_REVIEWED',naturalTailStatus='NOT_OBSERVED',
                evidenceType='runtime-manual',backend=args.backend,executableSha256=pinned,
                nominalIdentity=identity,inputHashes=hashes,scene=SCENE,scale=args.scale,position=args.position,
                ownerMotion=motion,
                fixedDeltaSeconds=1/60,seed=parameters['fixed_random_seed'],captures=[],
                gpuMode='explicit-dev-probe' if args.gpu_probe else 'no-GPU-environment-override',
                scope='Actual GPU commands/timers and captured appearance; no exact live particle readback, native memory or benchmark claim')
    previous=os.environ.get('HE_VFX_GPU_PARTICLES')
    handle=None; host_steps=0; age=0
    with Storage(ROOT).workflow_scope(str(ROOT/'Project'),'vfx-stress-visual',[output]) as run:
        try:
            if args.gpu_probe: os.environ['HE_VFX_GPU_PARTICLES']='1'
            else: os.environ.pop('HE_VFX_GPU_PARTICLES',None)
            session.start()
            report['description']=session.call('vfx.describe')
            report['quality']=session.call('vfx.quality')
            if report['quality']['tier']!='High' or report['quality']['enabled'] is not True:
                raise RuntimeError('Stress visual evidence requires actual GPU-enabled High quality')
            empty=capture(session,output,'empty',0,0); report['captures'].append(empty)
            if not retired(empty['stats']): raise RuntimeError('Nonempty starting effect baseline')
            play=session.call('vfx.play',dict(path=effect,seed=report['seed'],scale=args.scale,position=args.position,speed=1))
            handle=play['handle']
            if handle<0: raise RuntimeError('Stress GPU fixture failed to play')
            report['play']=play
            paused=False
            for label,target,is_paused in schedule:
                if is_paused!=paused:
                    control=session.call('vfx.control',dict(handle=handle,action='pause' if is_paused else 'resume'))
                    if control.get('accepted') is not True: raise RuntimeError('Pause/resume rejected')
                    paused=is_paused
                count=30 if paused else target-age
                step(session,count); host_steps+=count
                if not paused: age=target
                sample=capture(session,output,label,age,host_steps,handle); report['captures'].append(sample)
                state=sample['effectStatus']; stats=sample['stats']
                if target<=231 and (state.get('playing') is not True or state.get('paused') is not paused):
                    raise RuntimeError('Expected live stress pause/resume state was not observed')
                if label in ('plateau','resumed','mid-plateau'):
                    before=empty['stats'] if label=='plateau' else report['captures'][-2]['stats']
                    allocation=stats.get('gpuAllocation',{})
                    if not live_cycle_valid(before,stats) or allocation.get('allocatedEmitters',0)<=0 or allocation.get('reservedParticles',0)<args.nominal_particles:
                        raise RuntimeError('Missing actual GPU dispatch/draw/fresh positive timing or GPU allocation evidence')
                if any(stats.get('gpuAllocation',{}).get(key)!=0 for key in ('emitterSlotFailures','particleBlockFailures','trailBlockFailures')):
                    raise RuntimeError('GPU allocator failure observed')
                write_json(output/'report.json',report)
            final=report['captures'][-1]
            report['naturalTailStatus']='RETIRED' if retired(final['stats']) and final['effectStatus'].get('playing') is False else 'NOT_RETIRED_BY_1431'
            session.call('vfx.control',dict(handle=handle,action='stop'))
            step(session,30); host_steps+=30
            if not retired(session.call('vfx.stats')): raise RuntimeError('Tail cleanup failed before explicit-stop replay')
            # Stop a genuinely live, filled emitter, not only an already-ended
            # handle from the independent natural-tail observation above.
            replay=session.call('vfx.play',dict(path=effect,seed=report['seed'],scale=args.scale,position=args.position,speed=1))
            handle=replay['handle']
            if handle<0: raise RuntimeError('Explicit-stop replay failed')
            before_replay=session.call('vfx.stats')
            step(session,201); host_steps+=201
            replayed=capture(session,output,'stop-replay-plateau',201,host_steps,handle)
            report['captures'].append(replayed)
            if replayed['effectStatus'].get('playing') is not True or not live_cycle_valid(before_replay,replayed['stats']):
                raise RuntimeError('Explicit stop requires a live replay with actual GPU work')
            session.call('vfx.control',dict(handle=handle,action='stop'))
            step(session,30); host_steps+=30
            stopped=capture(session,output,'explicit-stop-retired',201,host_steps,handle); report['captures'].append(stopped)
            if not retired(stopped['stats']): raise RuntimeError('Explicit stop did not retire all effect allocator ownership')
            by_stage={item['stage']:item for item in report['captures']}
            report['imageComparisons']={label:image_difference(by_stage[a]['path'],by_stage[b]['path']) for label,a,b in
                [('pause','paused-a','paused-b'),('resume','paused-b','resumed'),
                 ('plateauAgainstEmpty','empty','plateau'),('tailAgainstEmpty','empty','natural-retired'),
                 ('stopAgainstEmpty','empty','explicit-stop-retired')]}
            report['commandStatus']='PASS' if report['naturalTailStatus']=='RETIRED' else 'FAIL'
            if report['commandStatus']=='FAIL': report['error']='Natural owner/lifetime retirement was not observed; explicit stop is separate evidence'
        except Exception as error:
            report['commandStatus']='FAIL'; report['error']=str(error)
        finally:
            try:
                if handle is not None: session.call('vfx.control',dict(handle=handle,action='stop'))
            except Exception as error: report['effectCleanupError']=str(error)
            try: session.stop()
            except Exception as error:
                report['cleanupError']=str(error); report['commandStatus']='FAIL'
            if previous is None: os.environ.pop('HE_VFX_GPU_PARTICLES',None)
            else: os.environ['HE_VFX_GPU_PARTICLES']=previous
            report['inputsUnchanged']=all(path.is_file() and digest(path)==hashes[str(path.relative_to(ROOT))] for path in inputs)
            report['binaryUnchanged']=EXE.is_file() and digest(EXE)==pinned
            logs=[output/'game.stdout.log',output/'game.stderr.log']
            report['nativeLogsComplete']=all(path.is_file() for path in logs)
            report['nativeErrorLines']=[line[:2000] for path in logs if path.is_file()
                for line in path.read_text(encoding='utf-8-sig',errors='replace').splitlines()
                if re.search(r'VUID-|Validation Error|DEVICE_LOST|device removed',line,re.I)]
            if not report['inputsUnchanged'] or not report['binaryUnchanged'] or not report['nativeLogsComplete'] or report['nativeErrorLines']:
                report['commandStatus']='FAIL'
            run['status']='unknown' if report.get('cleanupError') else 'success' if report['commandStatus']=='PASS' else 'failed'
            write_json(output/'report.json',report)
    print(json.dumps({key:report[key] for key in ('commandStatus','visualStatus','naturalTailStatus')}))
    return 0 if report['commandStatus']=='PASS' else 1


if __name__=='__main__': raise SystemExit(main())
