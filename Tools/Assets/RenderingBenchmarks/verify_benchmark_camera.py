#!/usr/bin/env python3
"""Headless fixed-tick script contract using a minimal engine binding stand-in."""
import importlib.util
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import types

ROOT=Path(__file__).resolve().parents[3]
PROJECT=ROOT/'Examples/RenderingTests'
OUTPUT=ROOT/'out/agent/benchmarks/camera-script-contract'
engine=types.ModuleType('HEngine')
engine.vec3=lambda *values:tuple(values)
sys.modules['HEngine']=engine
spec=importlib.util.spec_from_file_location('RenderingBenchmarkCamera',PROJECT/'Scripts/RenderingBenchmarkCamera.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)


class Entity:
    def __init__(self,trajectory):
        self.transform=types.SimpleNamespace(Translation=None,Rotation=None)
        self.tag=types.SimpleNamespace(Tag='BenchmarkCamera:'+trajectory)
    def GetComponent(self,name):return self.transform if name=='Transform' else self.tag


def run(trajectory,suffix):
    directory=OUTPUT/suffix;directory.mkdir(parents=True,exist_ok=True)
    os.environ['HE_RENDER_BENCHMARK_OUTPUT']=str(directory)
    entity=Entity(trajectory);camera=module.RenderingBenchmarkCamera();camera.Start(entity)
    poses=[]
    for tick in range(1200):
        camera.Update(types.SimpleNamespace(GetSeconds=lambda:1/60))
        poses.append((entity.transform.Translation,entity.transform.Rotation))
    camera.Stop()
    assert poses[:600]==poses[600:]
    assert len(set(poses[:600]))>500
    evidence=json.loads((directory/'camera.runtime.json').read_text())
    assert evidence['ticks']==1200 and evidence['status']=='PASS_FIXED_TICK_INPUT'
    return evidence['poseSequenceSha256']


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--refresh-manifests',action='store_true',help='Regenerate trajectory byte hashes after the deterministic contract passes; scene/script hashes must already match.')
    args=parser.parse_args()
    prior=os.environ.get('HE_RENDER_BENCHMARK_OUTPUT')
    results=[]
    try:
        # Historical codec variants are not part of the public sample project.
        for name in ['IntelSponza2048BCGPULOD','BistroExterior2048BCGPUMaskFixedEnvironment']:
            trajectory='Assets/RenderingBenchmarks/Trajectories/'+name+'/trajectory.json'
            first=run(trajectory,name+'-first');second=run(trajectory,name+'-second')
            assert first==second
            directory=OUTPUT/(name+'-invalid-delta');directory.mkdir(parents=True,exist_ok=True)
            os.environ['HE_RENDER_BENCHMARK_OUTPUT']=str(directory)
            camera=module.RenderingBenchmarkCamera();camera.Start(Entity(trajectory))
            try:camera.Update(types.SimpleNamespace(GetSeconds=lambda:1/30))
            except ValueError:pass
            else:raise AssertionError('Variable/wrong timestep was silently accepted')
            assert camera.evidence['status']=='FAIL'
            results.append({'trajectory':trajectory,'poseSequenceSha256':first,'periodFrames':600,'simulatedTicks':1200,'status':'PASS'})
        if args.refresh_manifests:
            # A new byte baseline does not assert equivalence with historical GPU
            # measurements. Keep scene/script/environment identities unchanged.
            for result in results:
                manifest_path=(PROJECT/result['trajectory']).with_name('benchmark_input.json')
                manifest=json.loads(manifest_path.read_text(encoding='utf-8-sig'))
                for kind in ('scene','script'):
                    assert hashlib.sha256((PROJECT/manifest[kind]).read_bytes()).hexdigest()==manifest[kind+'Sha256'], kind+' identity changed'
                if 'environment' in manifest:
                    environment=manifest['environment']
                    assert hashlib.sha256((PROJECT/environment['path']).read_bytes()).hexdigest()==environment['sha256']
                manifest['trajectorySha256']=hashlib.sha256((PROJECT/result['trajectory']).read_bytes()).hexdigest()
                manifest_path.write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n',encoding='utf-8')
        report={'evidenceType':'unit-test','command':'python Tools/Assets/RenderingBenchmarks/verify_benchmark_camera.py',
                'binding':'Minimal Python stand-in; actual HGame scripting/transform integration remains runtime validation',
                'checks':'Two byte-identical runs per retained scene; exact closed cycles; actual movement; invalid timestep fails','results':results}
        (OUTPUT/'validation.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report,indent=2))
    finally:
        if prior is None:os.environ.pop('HE_RENDER_BENCHMARK_OUTPUT',None)
        else:os.environ['HE_RENDER_BENCHMARK_OUTPUT']=prior


if __name__=='__main__':main()
