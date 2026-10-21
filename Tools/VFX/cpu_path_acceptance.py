"""Controlled CPU path image collection; visual approval always remains pending."""
import argparse
import json
import os
from pathlib import Path
import shutil
import xml.etree.ElementTree as ET
import zipfile

import numpy as np
from PIL import Image
from preview import ROOT, Session, Storage, digest, write_json

SCENE='Assets/Scenes/VFXCpuPathValidation.he'
FIXTURES=('CpuSpriteControl','CpuSoftParticle','CpuDistortion','CpuModel')


def image(path):
    with Image.open(path) as value: return np.asarray(value.convert('RGB'),dtype=np.int16)


def differences(a,b):
    delta=np.abs(a-b);mask=delta.max(axis=2)>12
    y,x=np.nonzero(mask)
    return dict(rgbMae=float(delta.mean()),changedPixels=int(mask.sum()),threshold=12,
                bbox=[int(x.min()),int(y.min()),int(x.max()+1),int(y.max()+1)] if len(x) else None)


def source_control_equivalent():
    folder=ROOT/'Project/SourceAssets/VFXAuthoring/AuthoringPathValidation'
    values=[]
    for name in FIXTURES[:2]:
        node=ET.parse(folder/name/(name+'.efkproj')).getroot().find('Root/Children/Node')
        for child in list(node):
            if child.tag in ('Name','AdvancedRendererCommonValuesValues'): node.remove(child)
        values.append(ET.tostring(node))
    return values[0]==values[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend',choices=['DX12','Vulkan'],required=True)
    parser.add_argument('--existing-build-sha256',required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    executable=ROOT/'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'
    pinned=digest(executable)
    if pinned.lower()!=args.existing_build_sha256.lower(): parser.error('Candidate SHA256 mismatch')
    if not source_control_equivalent(): parser.error('Soft/control differ beyond name and soft parameters')
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    paths={name:'Assets/Effects/AuthoringPathValidation/'+name+'.efkpkg' for name in FIXTURES}
    inputs={p:digest(ROOT/'Project'/p) for p in [SCENE]+list(paths.values())}
    report=dict(commandStatus='RUNNING',visualStatus='REQUIRES_REVIEW',evidenceType='runtime-manual',
                backend=args.backend,executableSha256=pinned,inputs=inputs,checks=[],captures=[],
                controlEquivalentExceptSoft=True,scope='CPU path control experiment; image differences are not visual PASS')
    for name,path in paths.items():
        with zipfile.ZipFile(ROOT/'Project'/path) as package:
            report.setdefault('compiledParameters',{})[name]=json.loads(package.read('PROVENANCE.json'))
    session=Session(output,args.backend,pinned,scene=SCENE);handles=set();baseline=None
    call=lambda method,**params:session.call(method,params)
    def check(name,condition,**data):
        report['checks'].append(dict(name=name,status='PASS' if condition else 'FAIL',**data))
        write_json(output/'report.json',report)
    def capture(name,handle=None):
        receipt=call('capture.frame',filename=name+'.png');target=output/(name+'.png')
        shutil.copyfile(receipt['path'],target)
        stats=call('vfx.stats');state=call('vfx.control',handle=handle,action='status') if handle is not None else None
        write_json(output/(name+'-stats.json'),dict(stats=stats,state=state))
        report['captures'].append(dict(name=name,path=str(target),sha256=digest(target),receipt=receipt,state=state))
        return image(target),stats
    def graph(name):
        page=call('render.graph.snapshot',section='passes',limit=100);pages=[page]
        while page.get('nextOffset') is not None:
            if len(pages)>=10: raise RuntimeError('Unbounded graph snapshot')
            page=call('render.graph.snapshot',section='passes',limit=100,
                      snapshotId=page['snapshotId'],offset=page['nextOffset']);pages.append(page)
        write_json(output/(name+'-graph.json'),pages)
        return {p.get('name') for page in pages for p in page.get('items',[])
                if p.get('culled') is False and p.get('executionIndex') is not None and p.get('recordedQueue') is not None}
    saved={k:os.environ.get(k) for k in ('HE_VFX_GPU_PARTICLES','HE_VFX_DISABLE_STATS')}
    os.environ['HE_VFX_GPU_PARTICLES']='0';os.environ['HE_VFX_DISABLE_STATS']='0'
    with Storage(ROOT).workflow_scope(str(ROOT/'Project'),'vfx-cpu-paths',[output]) as run:
        try:
            try:
                session.start();call('runtime.pause');call('runtime.step',frames=30)
                report['session']=call('session.status')
                _,baseline=capture('initial-empty')
                check('gpu_disabled',baseline.get('gpuEnabled') is False)
                check('empty_scene',baseline.get('activeEffects',{}).get('current')==0)
                frames={}
                for name in FIXTURES:
                    empty,before=capture(name+'-empty')
                    result=call('vfx.play',path=paths[name],position=[0,0,0],seed=1806)
                    handle=result['handle'];handles.add(handle)
                    check(name+'.play',result.get('playing') is True,receipt=result)
                    previous=0
                    for step in (1,30,90):
                        call('runtime.step',frames=step-previous);previous=step
                        pixels,stats=capture(name+'-'+str(step),handle)
                        state=report['captures'][-1]['state']
                        check(name+'.state'+str(step),state.get('playing') is True and state.get('speed')==1
                              and state.get('paused') is False and state.get('position')==[0,0,0],state=state)
                        report.setdefault('imageDifferences',{})[name+'-'+str(step)]=differences(pixels,empty)
                        frames[(name,step)]=pixels
                        if step==30:
                            names=graph(name)
                            needed={'VFXTransparent'}
                            if name=='CpuSoftParticle':needed.add('VFXDepthSnapshot')
                            if name=='CpuDistortion':needed.add('VFXBackgroundCopy')
                            check(name+'.graph_inputs',needed<=names,required=sorted(needed),executed=sorted(names))
                            check(name+'.actual_draw',stats.get('totalRecordedDrawCount',0)>before.get('totalRecordedDrawCount',0))
                    result=call('vfx.control',handle=handle,action='stop')
                    if result.get('accepted') is True: handles.discard(handle)
                    check(name+'.stop',result.get('accepted') is True)
                    call('runtime.step',frames=30);stopped,_=capture(name+'-stopped',handle)
                    check(name+'.stopped_state',report['captures'][-1]['state'].get('playing') is False)
                    report['imageDifferences'][name+'-stopped']=differences(stopped,empty)
                for step in (1,30,90):
                    report.setdefault('softControlDifferences',{})[str(step)]=differences(
                        frames[('CpuSoftParticle',step)],frames[('CpuSpriteControl',step)])
                report['reviewRequired']=['Check depth-contact-localized fading, not a uniform alpha decrease',
                    'Check warped background edges rather than ordinary sprite color/alpha',
                    'Check actual textured arrow model silhouette and occlusion',
                    'Check empty/stopped scene stability and validation logs; reject temporal/background confounds']
            finally:
                try:
                    for handle in list(handles): call('vfx.control',handle=handle,action='stop')
                    if baseline is not None:
                        call('runtime.step',frames=30);report['finalStats']=call('vfx.stats')
                        check('retired_effects_and_instances',all(report['finalStats'].get(k,{}).get('current')==0
                              for k in ('activeEffects','cpuInstances')))
                finally:
                    try: session.stop()
                    except Exception as error: report['cleanupError']=str(error);raise
            report['commandStatus']='FAIL' if any(c['status']=='FAIL' for c in report['checks']) else 'PASS'
            run['status']='success' if report['commandStatus']=='PASS' else 'failed'
        except Exception as error:
            report['commandStatus']='FAIL';report['error']=str(error)
            run['status']='unknown' if report.get('cleanupError') else 'failed'
        finally:
            for key,value in saved.items():
                if value is None:os.environ.pop(key,None)
                else:os.environ[key]=value
            report['inputsUnchanged']=all(digest(ROOT/'Project'/p)==h for p,h in inputs.items())
            report['binaryUnchanged']=digest(executable)==pinned
            if not report['inputsUnchanged'] or not report['binaryUnchanged']:
                report['commandStatus']='FAIL';run['status']='failed'
            write_json(output/'report.json',report)
    print(json.dumps(dict(commandStatus=report['commandStatus'],visualStatus=report['visualStatus'],report=str(output/'report.json'))))
    return 0 if report['commandStatus']=='PASS' else 1


if __name__=='__main__':raise SystemExit(main())
