"""Real runtime scene switches and native window resize; no offscreen substitute."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct

from preview import ROOT, Session, Storage, digest, write_json

SCENES=['Assets/Scenes/VFXAuthoringNeutral.he','Assets/Scenes/VFXAuthoringPreview.he']
EFFECTS=['Assets/Effects/Authoring/environment-ember-low.efkpkg',
         'Assets/Effects/Authoring/environment-ember.efkpkg']
FAILURES=['emitterSlotFailures','particleBlockFailures','trailBlockFailures']
RESERVED=['allocatedEmitters','reservedParticles','reservedTrails']


def png_size(path):
    with Path(path).open('rb') as stream: header=stream.read(24)
    if len(header)!=24 or header[:8]!=b'\x89PNG\r\n\x1a\n' or header[12:16]!=b'IHDR':
        raise ValueError('Capture is not a PNG with IHDR')
    return list(struct.unpack('>II',header[16:24]))


class Acceptance:
    def __init__(self,session,report):
        self.session,self.report,self.handles=session,report,set()

    def call(self,method,**params): return self.session.call(method,params)

    def check(self,name,condition,**evidence):
        self.report['checks'].append(dict(name=name,status='PASS' if condition else 'FAIL',**evidence))
        write_json(self.session.output/'report.json',self.report)
        if not condition: raise RuntimeError('Failed: '+name)

    def step(self,frames):
        while frames:
            count=min(120,frames); self.call('runtime.step',frames=count); frames-=count

    def stats(self):
        stats=self.call('vfx.stats'); allocation=stats.get('gpuAllocation',{})
        if any(type(allocation.get(key)) is not int or allocation[key]<0 for key in FAILURES+RESERVED):
            raise RuntimeError('Required allocator telemetry unavailable')
        self.check('allocator_no_failures',all(allocation[key]==0 for key in FAILURES),allocation=allocation)
        return stats

    def state(self,handle): return self.call('vfx.control',handle=handle,action='status')

    def play_pair(self,label):
        for path in EFFECTS:
            result=self.call('vfx.play',path=path,seed=1806)
            handle=result['handle']; self.handles.add(handle)
            self.check(label+'.playing',result.get('playing') is True,path=path,receipt=result)
        before=self.stats(); self.step(8); after=self.stats()
        self.check(label+'.fresh_commands',after.get('totalRecordedDrawCount',0)>before.get('totalRecordedDrawCount',0)
            and after.get('totalRecordedDispatchCount',0)>before.get('totalRecordedDispatchCount',0),before=before,after=after,
            scope='Accepted native draw/dispatch recording; not particle-state or completed GPU-byte readback')
        self.check(label+'.coexisting',len(self.handles)==2 and all(self.state(h).get('playing') for h in self.handles))

    def capture(self,label,expected):
        receipt=self.call('capture.frame',filename=label+'.png')
        target=self.session.output/(label+'.png'); shutil.copyfile(receipt['path'],target)
        size=png_size(target)
        self.check(label+'.png_dimensions',size==expected,actual=size,expected=expected,
            path=str(target),sha256=digest(target),receipt=receipt,stats=self.stats())

    def stop_pair(self,label):
        old=list(self.handles)
        for handle in old: self.call('vfx.control',handle=handle,action='stop')
        self.handles.clear(); self.step(32)
        stats=self.stats()
        self.check(label+'.retired',all(not self.state(h).get('playing') for h in old)
            and all(stats['gpuAllocation'][key]==self.report['baseline']['gpuAllocation'][key] for key in RESERVED),stats=stats,
            scope='SDK allocator ownership restored after 32 frames; not proof of fence-retired GPU bytes')

    def resize(self,width,height,label):
        before=self.stats()
        receipt=self.call('window.resize',width=width,height=height)
        self.check(label+'.native_client',receipt.get('ok') is True and
            [receipt.get('clientWidth'),receipt.get('clientHeight')]==[width,height],receipt=receipt)
        for _ in range(30):
            self.step(1); status=self.call('session.status')
            if [status.get('renderWidth'),status.get('renderHeight')]==[width,height]: break
        self.check(label+'.render_extent',[status.get('renderWidth'),status.get('renderHeight')]==[width,height],runtimeState=status)
        self.step(4); after=self.stats()
        self.check(label+'.fresh_commands',after.get('totalRecordedDrawCount',0)>before.get('totalRecordedDrawCount',0)
            and after.get('totalRecordedDispatchCount',0)>before.get('totalRecordedDispatchCount',0),before=before,after=after)
        self.check(label+'.handles_live',all(self.state(h).get('playing') for h in self.handles))
        self.capture(label,[width,height])

    def switch(self,path,label):
        before=self.call('world.summary')
        queued=self.call('runtime.scene.load',path=path,forceReload=True)
        self.check(label+'.queued',queued.get('ok') is True,receipt=queued)
        for _ in range(60):
            self.step(1); status=self.call('session.status'); world=self.call('world.summary')
            if not status.get('runtimeSceneRequestPending') and world.get('sceneGeneration')!=before.get('sceneGeneration'): break
        observed=str(world.get('scene','')).replace('\\','/').lower()
        expected=path.lower()
        self.check(label+'.committed',world.get('available') is True and not status.get('runtimeSceneRequestPending')
            and world.get('sceneGeneration',0)>0
            and world.get('sceneGeneration',0)!=before.get('sceneGeneration',0)
            and observed in [expected,Path(expected).name,Path(expected).stem],before=before,after=world,runtimeState=status)
        self.report.setdefault('oldHandleObservations',[]).append(dict(stage=label,
            handles=[self.state(h) for h in self.handles],policy='Observed only; no assumed scene-auto-stop contract'))
        self.stop_pair(label)
        self.play_pair(label+'.new_scene')
        self.capture(label,[status['renderWidth'],status['renderHeight']])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend',choices=['DX12','Vulkan'],required=True)
    parser.add_argument('--existing-build-sha256',required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    executable=ROOT/'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'
    pinned=digest(executable)
    if pinned.lower()!=args.existing_build_sha256.lower(): parser.error('Pinned candidate SHA256 mismatch')
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=False)
    inputs={path:digest(ROOT/'Project'/path) for path in SCENES+EFFECTS}
    report=dict(status='RUNNING',visualStatus='NOT_REVIEWED',backend=args.backend,
        executableSha256=pinned,configuration='RelWithDebInfo Dev Agent',inputHashes=inputs,checks=[],
        scope='Actual runtime.scene.load and Window.SetClientSize; no author preview or offscreen view resize',
        excluded=['device loss/recreation','particle-state readback','fence-retired GPU bytes','visual approval','performance'])
    session=Session(output,args.backend,pinned,scene=SCENES[0]); acceptance=Acceptance(session,report)
    previous=os.environ.get('HE_VFX_GPU_PARTICLES'); os.environ['HE_VFX_GPU_PARTICLES']='1'
    with Storage(ROOT).workflow_scope(str(ROOT/'Project'),'vfx-scene-resize',[output]) as run:
        try:
            session.start()
            report['discovery']=session.run(['agent','discover'],'discover')
            report['session']=acceptance.call('session.status')
            report['description']=acceptance.call('vfx.describe')
            acceptance.step(30); report['baseline']=acceptance.stats()
            acceptance.check('gpu_enabled',report['baseline'].get('gpuEnabled') is True)
            acceptance.check('empty_allocator',all(report['baseline']['gpuAllocation'][key]==0 for key in RESERVED))
            acceptance.capture('empty',[1280,720])
            acceptance.play_pair('initial')
            acceptance.resize(960,540,'resize-small')
            acceptance.resize(1280,720,'resize-restore')
            acceptance.switch(SCENES[1],'switch-game')
            acceptance.resize(1024,768,'resize-aspect')
            acceptance.switch(SCENES[0],'switch-neutral')
            acceptance.resize(1280,720,'final-restore')
            acceptance.stop_pair('final')
            acceptance.capture('final-retired',[1280,720])
            report['status']='PASS'; run['status']='success'
        except Exception as error:
            report['status']='FAIL'; report['error']=str(error); run['status']='failed'
        finally:
            try:
                for handle in list(acceptance.handles): acceptance.call('vfx.control',handle=handle,action='stop')
            except Exception as error: report['effectCleanupError']=str(error)
            try: session.stop()
            except Exception as error:
                report['status']='FAIL'; report['cleanupError']=str(error); run['status']='unknown'
            if previous is None: os.environ.pop('HE_VFX_GPU_PARTICLES',None)
            else: os.environ['HE_VFX_GPU_PARTICLES']=previous
            try:
                report['inputsUnchanged']=all(digest(ROOT/'Project'/p)==value for p,value in inputs.items())
                report['binaryUnchanged']=digest(executable)==pinned
            except OSError:
                report['inputsUnchanged']=report['binaryUnchanged']=False
            if not report['inputsUnchanged'] or not report['binaryUnchanged']:
                report['status']='FAIL'
                if run['status']!='unknown': run['status']='failed'
            write_json(output/'report.json',report)
    print(json.dumps(dict(status=report['status'],visualStatus='NOT_REVIEWED',report=str(output/'report.json'))))
    return 0 if report['status']=='PASS' else 1


if __name__=='__main__': raise SystemExit(main())
