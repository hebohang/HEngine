"""Pinned, owned runtime parameter/reload/fallback diagnostics. Never builds."""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import uuid
import zipfile

from preview import ROOT, Session, Storage, digest, write_json


def numeric_match(actual, expected, rel_tol=1e-9):
    """Missing or malformed runtime telemetry fails a check, not the session."""
    return (type(actual) in (int, float) and math.isfinite(actual)
            and math.isclose(actual, expected, rel_tol=rel_tol))


def recipe(path):
    with zipfile.ZipFile(path) as package:
        info = package.getinfo('PROVENANCE.json')
        if info.file_size > 65536:
            raise ValueError('Oversized provenance')
        return json.loads(package.read(info))['recipe']


class Checks:
    def __init__(self, session, report):
        self.session, self.report, self.handles = session, report, set()

    def record(self, name, condition, **evidence):
        self.report['checks'].append(dict(name=name,status='PASS' if condition else 'FAIL',**evidence))
        write_json(self.session.output/'report.json',self.report)

    def call(self, method, **params):
        return self.session.call(method,params)

    def play(self,path,seed=1806):
        value = self.call('vfx.play',path=path,seed=seed)
        handle = value['handle']; self.handles.add(handle)
        if value.get('playing') is not True:
            raise RuntimeError('Play did not return a live effect')
        return handle

    def state(self,handle):
        return self.call('vfx.control',handle=handle,action='status')

    def control(self,handle,action,**params):
        return self.call('vfx.control',handle=handle,action=action,**params)

    def capture(self,name):
        receipt = self.call('capture.frame',filename=name+'.png')
        target = self.session.output/(name+'.png')
        shutil.copyfile(receipt['path'],target)
        return dict(path=str(target),sha256=digest(target),receipt=receipt)

    def stop(self,handle):
        self.control(handle,'stop'); self.handles.discard(handle)

    def rejected(self,label,method,**params):
        start = len(self.session.commands)
        try:
            receipt = self.call(method,**params)
            accepted = receipt.get('accepted') is False
        except RuntimeError:
            if len(self.session.commands) <= start:
                raise
            text = Path(self.session.commands[-1]['stdout']).read_text(encoding='utf-8-sig',errors='replace')
            offset = text.find('{')
            receipt = json.loads(text[offset:]) if offset >= 0 else {}
            error = receipt.get('error',{})
            accepted = receipt.get('ok') is False and isinstance(error,dict) and (
                error.get('code') == 'invalid_params' or 'VFX load or play failed' in str(error.get('message','')))
        self.call('session.status')  # A dead/failed transport is not rejection evidence.
        self.record(label,accepted,receipt=receipt)

    def parameters(self,label,path):
        handle = self.play(path)
        self.call('runtime.step',frames=8)
        self.control(handle,'pause')
        state = self.state(handle)
        self.record(label+'.supports_color',state.get('supportsGlobalColor') is True,state=state)
        captures = [self.capture(label+'-base')]
        color = self.control(handle,'color',value=[.25,.75,1,1])
        self.record(label+'.color_accepted',color.get('accepted') is True,receipt=color)
        self.call('runtime.step',frames=1)
        captures.append(self.capture(label+'-color'))
        for intensity in [0,.5,1]:
            result = self.control(handle,'parameter',name='intensity',value=intensity)
            current = self.state(handle)
            self.record(label+'.intensity_'+str(intensity),result.get('accepted') is True and
                numeric_match(current.get('intensity'),intensity),receipt=result,state=current)
            self.call('runtime.step',frames=1)
            captures.append(self.capture(label+'-intensity-'+str(intensity)))
        radius = recipe(ROOT/'Project'/path)['compile']['radius_m']
        for name,value in [('radius_m',radius*2),('scale',1),('speed',0),('speed',1)]:
            result = self.control(handle,'parameter',name=name,value=value)
            current = self.state(handle)
            valid = result.get('accepted') is True
            if name == 'speed': valid = valid and current.get('speed') == value
            if name == 'radius_m': valid = valid and numeric_match(current.get('authoredRadiusMeters'),radius,rel_tol=1e-5)
            self.record(label+'.'+name+'_'+str(value),valid,receipt=result,state=current,
                scope='Parameter accepted; radius uses nominal metadata, actual scale/bounds not exposed')
        self.call('runtime.step',frames=1)
        captures.append(self.capture(label+'-parameters'))
        for name,value in [('intensity',-1),('intensity',1.01),('radius_m',0),('scale',0),('speed',17),('unknown',1)]:
            self.rejected(label+'.reject_'+name+'_'+str(value),'vfx.control',handle=handle,
                action='parameter',name=name,value=value)
        self.report['visualEvidence'].extend(captures)
        self.stop(handle)
        self.call('runtime.step',frames=30)
        # Seed receipts plus images are useful diagnostics, not state hashes.
        for index in range(2):
            fresh = self.play(path,seed=1806)
            self.call('runtime.step',frames=8)
            self.report['visualEvidence'].append(self.capture(label+'-seed1806-'+str(index)))
            self.stop(fresh); self.call('runtime.step',frames=30)
        self.record(label+'.same_seed_fresh_handles',True,seed=1806,
            scope='Two real fresh plays; simulation reproducibility requires state readback and remains NOT_VERIFIED')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend',choices=['DX12','Vulkan'],required=True)
    parser.add_argument('--existing-build-sha256',required=True)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    executable = ROOT/'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'
    pinned = digest(executable)
    if pinned.lower() != args.existing_build_sha256.lower():
        parser.error('Candidate SHA256 mismatch; no session started')
    cpu = 'Assets/Effects/Authoring/hit-ember.efkpkg'
    replacement = 'Assets/Effects/Authoring/hit-frost.efkpkg'
    gpu = 'Assets/Effects/Authoring/explosion-ember.efkpkg'
    fallback_id = recipe(ROOT/'Project'/gpu)['requirements']['fallback_recipe']
    fallback = 'Assets/Effects/Authoring/'+fallback_id+'.efkpkg'
    inputs = {p:digest(ROOT/'Project'/p) for p in [cpu,replacement,gpu,fallback]}
    old_radius = recipe(ROOT/'Project'/cpu)['compile']['radius_m']
    new_radius = recipe(ROOT/'Project'/replacement)['compile']['radius_m']
    if old_radius == new_radius:
        parser.error('Reload fixture needs different radius metadata')
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=False)
    scratch = ROOT/'Project/Assets/Effects/ParameterAcceptance'/uuid.uuid4().hex
    scratch.mkdir(parents=True,exist_ok=False)
    fixture = scratch/'reload.efkpkg'
    fixture.write_bytes((ROOT/'Project'/cpu).read_bytes())
    fixture_relative = fixture.relative_to(ROOT/'Project').as_posix()
    report=dict(status='RUNNING',evidenceType='runtime-manual',configuration='RelWithDebInfo Dev Agent',
        executableSha256=pinned,backend=args.backend,inputs=inputs,checks=[],visualEvidence=[],
        excluded=['performance','particle state determinism','measured radius boundary','visual approval',
            'base/applied color numerical readback','scale numerical readback'],
        scratch=str(scratch),sceneSha256=digest(ROOT/'Project/Assets/Scenes/VFXAuthoringPreview.he'))
    previous=os.environ.get('HE_VFX_GPU_PARTICLES')
    with Storage(ROOT).workflow_scope(str(ROOT/'Project'),'vfx-parameters',[output]) as run:
        try:
            for enabled in [True,False]:
                child=output/('enabled' if enabled else 'disabled'); child.mkdir()
                session=Session(child,args.backend,pinned)
                checks=Checks(session,report)
                os.environ['HE_VFX_GPU_PARTICLES']='1' if enabled else '0'
                try:
                    session.start()
                    stats=checks.call('vfx.stats')
                    checks.record(('enabled' if enabled else 'disabled')+'.gpu_mode',stats.get('gpuEnabled') is enabled,
                        stats=stats,description=checks.call('vfx.describe'),session=checks.call('session.status'))
                    if enabled:
                        checks.parameters('cpu',cpu); checks.parameters('gpu',gpu)
                        old=checks.play(fixture_relative)
                        fixture.write_bytes((ROOT/'Project'/replacement).read_bytes())
                        reload_result=checks.call('vfx.reload',path=fixture_relative)
                        new=checks.play(fixture_relative)
                        old_state,new_state=checks.state(old),checks.state(new)
                        checks.record('reload.changed_content_old_instance_retained',reload_result.get('accepted') is True
                            and old_state.get('playing') and new_state.get('playing')
                            and numeric_match(old_state.get('authoredRadiusMeters'),old_radius,rel_tol=1e-5)
                            and numeric_match(new_state.get('authoredRadiusMeters'),new_radius,rel_tol=1e-5),
                            receipt=reload_result,old=old_state,new=new_state)
                        fixture.write_bytes(b'corrupt acceptance candidate')
                        checks.rejected('reload.same_key_corrupt_rejected','vfx.reload',path=fixture_relative)
                        cached=checks.play(fixture_relative)
                        state=checks.state(cached)
                        checks.record('reload.same_key_rollback_preserves_cache',state.get('playing') and
                            numeric_match(state.get('authoredRadiusMeters'),new_radius,rel_tol=1e-5)
                            and checks.state(old).get('playing') and checks.state(new).get('playing'),state=state)
                    else:
                        checks.rejected('gpu_disabled.package_rejected','vfx.play',path=gpu,seed=1806)
                        handle=checks.play(fallback)
                        checks.call('runtime.step',frames=8)
                        checks.record('gpu_disabled.explicit_recipe_cpu_fallback',checks.state(handle).get('playing') and
                            checks.call('vfx.stats').get('gpuEnabled') is False,fallback=fallback,
                            scope='Explicit recipe fallback selected by caller; runtime does not silently convert GPU effects')
                        report['visualEvidence'].append(checks.capture('cpu-fallback'))
                finally:
                    try:
                        for handle in list(checks.handles): checks.stop(handle)
                    finally:
                        try:
                            session.stop()
                        except Exception as error:
                            report['cleanupError']=str(error)
                            raise
            report['status']='FAIL' if any(c['status']=='FAIL' for c in report['checks']) else 'PASS'
            run['status']='success' if report['status']=='PASS' else 'failed'
        except Exception as error:
            report['status']='FAIL'; report['error']=str(error)
            run['status']='unknown' if report.get('cleanupError') else 'failed'
        finally:
            if previous is None: os.environ.pop('HE_VFX_GPU_PARTICLES',None)
            else: os.environ['HE_VFX_GPU_PARTICLES']=previous
            # Only the exact files and newly created directory owned by this run.
            if not report.get('cleanupError'):
                fixture.unlink(missing_ok=True)
                scratch.rmdir()
            report['inputsUnchanged']=all(digest(ROOT/'Project'/p)==value for p,value in inputs.items())
            report['binaryUnchanged']=digest(executable)==pinned
            if not report['inputsUnchanged'] or not report['binaryUnchanged']:
                report['status']='FAIL'; run['status']='failed'
            report['visualStatus']='NOT_REVIEWED'
            write_json(output/'report.json',report)
    print(json.dumps(dict(status=report['status'],report=str(output/'report.json'))))
    return 0 if report['status']=='PASS' else 1


if __name__=='__main__':
    raise SystemExit(main())
