"""Separate initial-start quality sessions; never change quality in an active host frame."""
import argparse
import json
import os
from pathlib import Path
import shutil

from preview import ROOT, Session, Storage, digest, write_json

CAPACITY={'Low':0,'Medium':20224,'High':50176}
SCENE='Assets/Scenes/VFXQualityAcceptance.he'


def assess(runtime,tier):
    checks=[]
    def add(name,condition,**detail): checks.append(dict(name=name,status='PASS' if condition else 'FAIL',**detail))
    add('startup_selected',runtime.get('status')!='FAIL' and runtime.get('tier')==tier
        and runtime.get('selected',{}).get('quality_tier')==list(CAPACITY).index(tier))
    for frame in ['8','20']:
        sample=runtime.get('frames',{}).get(frame,{})
        stats=sample.get('stats',{})
        capacity=stats.get('particle_buffer_capacity',{})
        add('capacity_'+frame,capacity.get('status')==1 and capacity.get('current')==CAPACITY[tier],observed=capacity)
        add('enabled_'+frame,stats.get('gpu_particles_enabled') is (tier!='Low'))
        add('playing_'+frame,len(sample.get('playing',[]))==(1 if tier=='Low' else 2)
            and all(sample['playing']))
        add('allocator_'+frame,all(stats.get(key)==0 for key in
            ['gpu_emitter_slot_failures','gpu_particle_block_failures','gpu_trail_block_failures']))
    end=runtime.get('frames',{}).get('180',{})
    add('natural_end',len(end.get('playing',[]))==(1 if tier=='Low' else 2) and not any(end.get('playing',[])))
    return checks


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend',choices=['DX12','Vulkan'],required=True)
    parser.add_argument('--existing-build-sha256',required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--tier',choices=list(CAPACITY),action='append')
    args=parser.parse_args()
    executable=ROOT/'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'
    pinned=digest(executable)
    if pinned.lower()!=args.existing_build_sha256.lower(): parser.error('Candidate hash mismatch')
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=False)
    paths=[SCENE,'Scripts/VFXQualityAcceptance.py']+['Assets/Effects/Authoring/'+name+'.efkpkg'
        for name in ['hit-ember','explosion-ember','explosion-ember-low']]
    inputs={path:digest(ROOT/'Project'/path) for path in paths}
    report=dict(status='RUNNING',visualStatus='NOT_REVIEWED',executableSha256=pinned,backend=args.backend,
        inputHashes=inputs,tiers=[],scope='Initial Game scene Start before host frame; no active-frame quality changes')
    previous={key:os.environ.get(key) for key in ['HE_VFX_GPU_PARTICLES','HE_VFX_QUALITY_TIER','HE_VFX_QUALITY_REPORT']}
    with Storage(ROOT).workflow_scope(str(ROOT/'Project'),'vfx-quality',[output]) as run:
        try:
            for tier in dict.fromkeys(args.tier or CAPACITY):
                directory=output/tier; directory.mkdir()
                runtime_path=directory/'runtime.json'
                os.environ.update(HE_VFX_GPU_PARTICLES='1',HE_VFX_QUALITY_TIER=tier,HE_VFX_QUALITY_REPORT=str(runtime_path))
                session=Session(directory,args.backend,pinned,scene=SCENE)
                result=dict(tier=tier,captures=[]); report['tiers'].append(result)
                try:
                    session.start()
                    result['session']=session.call('session.status')
                    previous_frame=0
                    for frame in [8,20,180]:
                        remaining=frame-previous_frame
                        while remaining:
                            count=min(120,remaining); session.call('runtime.step',dict(frames=count)); remaining-=count
                        image=session.call('capture.frame',dict(filename='quality-'+tier+'-'+str(frame)+'.png'))
                        destination=directory/(str(frame)+'.png'); shutil.copyfile(image['path'],destination)
                        result['captures'].append(dict(frame=frame,path=str(destination),sha256=digest(destination),
                            receipt=image,stats=session.call('vfx.stats')))
                        previous_frame=frame
                    runtime=json.loads(runtime_path.read_text())
                    result['checks']=assess(runtime,tier)
                    if Path(runtime['executable']).resolve()!=executable.resolve(): raise RuntimeError('Embedded executable mismatch')
                    for handle in runtime.get('handles',[]): session.call('vfx.control',dict(handle=handle,action='stop'))
                    session.call('runtime.step',dict(frames=32))
                    result['retiredStats']=session.call('vfx.stats')
                    allocation=result['retiredStats'].get('gpuAllocation',{})
                    result['checks'].append(dict(name='allocator_retired',status='PASS' if all(allocation.get(key)==0 for key in
                        ['allocatedEmitters','reservedParticles','reservedTrails','emitterSlotFailures','particleBlockFailures','trailBlockFailures']) else 'FAIL'))
                    result['status']='PASS' if all(c['status']=='PASS' for c in result['checks']) else 'FAIL'
                finally:
                    try: session.stop()
                    except Exception as error: report['cleanupError']=str(error); raise
                    write_json(directory/'report.json',result)
            report['status']='PASS' if all(t.get('status')=='PASS' for t in report['tiers']) else 'FAIL'
            run['status']='success' if report['status']=='PASS' else 'failed'
        except Exception as error:
            report['status']='FAIL'; report['error']=str(error)
            run['status']='unknown' if report.get('cleanupError') else 'failed'
        finally:
            for key,value in previous.items():
                if value is None: os.environ.pop(key,None)
                else: os.environ[key]=value
            report['inputsUnchanged']=all(digest(ROOT/'Project'/path)==value for path,value in inputs.items())
            report['binaryUnchanged']=digest(executable)==pinned
            if not report['inputsUnchanged'] or not report['binaryUnchanged']:
                report['status']='FAIL'
                if run['status']!='unknown': run['status']='failed'
            write_json(output/'report.json',report)
    print(json.dumps(dict(status=report['status'],visualStatus='NOT_REVIEWED',report=str(output/'report.json'))))
    return 0 if report['status']=='PASS' else 1


if __name__=='__main__': raise SystemExit(main())
