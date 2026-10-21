"""Pinned SDK source-material / external-curve positive and missing-dependency probe."""
import argparse
import io
import json
import os
from pathlib import Path
import uuid
import zipfile
import numpy as np
from PIL import Image
from preview import ROOT, Session, Storage, digest, write_json
from parameter_acceptance import Checks


def without_dependency(path, suffix):
    output=io.BytesIO()
    with zipfile.ZipFile(path) as source, zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED) as target:
        removed=[n for n in source.namelist() if n.endswith(suffix)]
        assert len(removed)==1,(path,removed)
        for name in source.namelist():
            if name not in removed: target.writestr(name,source.read(name))
    return output.getvalue()


def pixels(path):
    return np.asarray(Image.open(path).convert('RGB'),dtype=np.int16)


def compiled_package(source, cache):
    output=io.BytesIO()
    with zipfile.ZipFile(source) as original, zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED) as target:
        materials=[name for name in original.namelist() if name.endswith('.efkmat')]
        if len(materials)!=1: raise ValueError('Exactly one external source material required')
        material=materials[0]
        for name in original.namelist():
            if name not in (material,material+'d'):target.writestr(name,original.read(name))
        target.writestr(material+'d',cache)
    return output.getvalue()


def compiled_probe(checks, report, source, scratch, owned, cache, backend, source_image):
    fixture=scratch/'CompiledOnly.efkpkg';owned.append(fixture)
    data=compiled_package(source,cache)
    path=fixture.relative_to(ROOT/'Project').as_posix()
    coverage=report['compiledCoverage'];coverage['status']='RUNNING'
    if backend=='DX12':
        fixture.write_bytes(data)
        handle=checks.play(path)
        try:
            checks.call('runtime.step',frames=2)
            capture=checks.capture('compiled-only-step2');report['visualEvidence'].append(capture)
            comparison=difference(pixels(source_image),pixels(capture['path']))
            checks.record('compiled_only.source_pixel_equivalence',comparison['changedPixels']==0,
                          comparison=comparison,scope='Delta threshold16 versus original source frame2; visual review remains required')
            checks.record('compiled_only.live',checks.state(handle).get('playing') is True)
            stats=checks.call('vfx.stats');draw=stats.get('drawCalls',{}).get('current')
            checks.record('compiled_only.draw',isinstance(draw,(int,float)) and draw>0,stats=stats)
        finally:checks.stop(handle)
    else:
        # A failed same-key replacement must preserve both active source and cached source.
        fixture.write_bytes(source.read_bytes());old=checks.play(path)
        try:
            checks.call('runtime.step',frames=2);checks.control(old,'pause');before=checks.state(old)
            fixture.write_bytes(data)
            checks.rejected('compiled_wrong_backend.reload','vfx.reload',path=path)
            checks.record('compiled_wrong_backend.old_source_unchanged',checks.state(old)==before,before=before,after=checks.state(old))
            replay=checks.play(path)
            checks.record('compiled_wrong_backend.cached_source_survives',checks.state(replay).get('playing') is True)
            checks.stop(replay)
            fresh=scratch/'CompiledOnlyFresh.efkpkg';owned.append(fresh);fresh.write_bytes(data)
            checks.rejected('compiled_wrong_backend.fresh_key','vfx.reload',path=fresh.relative_to(ROOT/'Project').as_posix())
        finally:checks.stop(old)
    checks.call('runtime.step',frames=30)
    stopped=checks.capture('compiled-after-stop');report['visualEvidence'].append(stopped)
    baseline=checks.session.output/'OriginalMaterial-stopped.png'
    residual=difference(pixels(baseline),pixels(stopped['path']))
    checks.record('compiled_cleanup.empty_background',residual['changedPixels']==0,pixels=residual)
    coverage['status']='FAIL' if any(c['status']=='FAIL' for c in report['checks'] if c['name'].startswith('compiled_')) else 'PASS'


def color_contract(checks, name, handle, custom):
    before=checks.state(handle)
    checks.record(name+'.color_support',before.get('supportsGlobalColor') is (not custom),sdkState=before)
    if not custom:
        return
    checks.record(name+'.color_support_reason',
                  before.get('supportsGlobalColorReason')=='custom_material_color_response_unverified',sdkState=before)
    for action,params in [('color',dict(value=[.25,.5,.75,.5])),
                          ('parameter',dict(name='intensity',value=0))]:
        checks.rejected(name+'.reject_'+action,'vfx.control',handle=handle,action=action,**params)
        after=checks.state(handle)
        checks.record(name+'.unchanged_after_'+action,after==before,before=before,after=after,
                      scope='All public status fields unchanged; SDK color input has no public readback')


def difference(background,image):
    diff=np.abs(image-background);mask=diff.max(axis=2)>16
    yy,xx=np.where(mask)
    return dict(changedPixels=int(mask.sum()),meanAbsoluteDifference=float(diff.mean()),
                bbox=[int(xx.min()),int(yy.min()),int(xx.max()+1),int(yy.max()+1)] if len(xx) else None,
                centroid=[float(xx.mean()),float(yy.mean())] if len(xx) else None)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend',choices=['DX12','Vulkan'],required=True)
    parser.add_argument('--existing-build-sha256',required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--compiled-cache',type=Path,help='Official DX12-only OriginalCyan.efkmatd')
    parser.add_argument('--compiled-cache-sha256')
    args=parser.parse_args()
    exe=ROOT/'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe';pinned=digest(exe)
    if pinned.lower()!=args.existing_build_sha256.lower():parser.error('Pinned candidate mismatch')
    source_dir=ROOT/'Project/Assets/Effects/MaterialCurveValidation'
    sources=[source_dir/(name+'.efkpkg') for name in ('OriginalMaterial','OriginalCurve')]
    inputs={str(p):digest(p) for p in sources}
    cache=None
    if bool(args.compiled_cache)!=bool(args.compiled_cache_sha256):parser.error('Cache path and SHA256 must be supplied together')
    if args.compiled_cache:
        args.compiled_cache=args.compiled_cache.resolve()
        if digest(args.compiled_cache).lower()!=args.compiled_cache_sha256.lower():parser.error('Compiled cache SHA256 mismatch')
        cache=args.compiled_cache.read_bytes();inputs[str(args.compiled_cache)]=digest(args.compiled_cache)
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    scratch=source_dir/'Acceptance'/uuid.uuid4().hex;scratch.mkdir(parents=True,exist_ok=False)
    report=dict(status='RUNNING',visualStatus='REQUIRES_REVIEW',evidenceType='runtime-manual',
        backend=args.backend,executableSha256=pinned,inputs=inputs,checks=[],visualEvidence=[],
        thresholds=dict(pixelDelta=16,minChangedPixels=50),
        compiledCoverage=dict(status='NOT_RUN' if cache is None else 'PENDING',backend=args.backend,
            scope='DX12 compiled-only positive or Vulkan DX12-only cache rejection; no Vulkan positive cache'),
        excluded=['Vulkan compiled-only positive','corrupt curve parsing',
                  'automatic visual correctness','GPU particle simulation'])
    session=Session(output,args.backend,pinned);checks=Checks(session,report)
    environment={k:os.environ.get(k) for k in ('HE_VFX_GPU_PARTICLES','HE_VFX_DISABLE_STATS')}
    os.environ['HE_VFX_GPU_PARTICLES']='0';os.environ['HE_VFX_DISABLE_STATS']='0'
    owned=[]
    with Storage(ROOT).workflow_scope(str(ROOT/'Project'),'vfx-material-curve',[output]) as run:
        try:
            try:
                session.start();checks.call('runtime.pause');checks.call('runtime.step',frames=30)
                for source,suffix in zip(sources,('.efkmat','.efkcurve')):
                    name=source.stem
                    fixture=scratch/(name+'.efkpkg');owned.append(fixture);fixture.write_bytes(source.read_bytes())
                    path=fixture.relative_to(ROOT/'Project').as_posix()
                    blank=checks.capture(name+'-empty');report['visualEvidence'].append(blank)
                    background=pixels(blank['path'])
                    receipt=checks.call('vfx.reload',path=path)
                    checks.record(name+'.real_sdk_load',receipt.get('accepted') is True,receipt=receipt)
                    handle=checks.play(path)
                    stages=[]
                    for step,frames in [(1,1),(2,1),(30,28),(60,30)]:
                        checks.call('runtime.step',frames=frames)
                        state=checks.state(handle)
                        capture=checks.capture(name+'-step'+str(step));report['visualEvidence'].append(capture)
                        metric=difference(background,pixels(capture['path']));stages.append(metric)
                        if suffix == '.efkcurve' and step == 1:
                            # Authored FadeIn.Frame=1 starts the standard sprite
                            # at zero alpha. Keep and assert this initial capture;
                            # require visible output from the next fixed step.
                            checks.record(name+'.initial_fade_in',state.get('playing') is True and metric['changedPixels']==0,
                                          sdkState=state,pixels=metric,authoredFadeInFrames=1)
                        else:
                            checks.record(name+'.visible_step'+str(step),state.get('playing') is True and metric['changedPixels']>=50,
                                          sdkState=state,pixels=metric,scope='Visible output signal only; visual correctness needs review')
                        write_json(output/(name+'-step'+str(step)+'-stats.json'),checks.call('vfx.stats'))
                    if suffix=='.efkcurve':
                        checks.record(name+'.curve_motion_signal',all(s['centroid'] for s in stages[1:]) and
                                      abs(stages[-1]['centroid'][0]-stages[1]['centroid'][0])>5,
                                      stages=stages,scope='Projected motion signal; path shape needs visual review')
                    checks.control(handle,'pause')
                    color_contract(checks,name,handle,suffix=='.efkmat')
                    fixture.write_bytes(without_dependency(source,suffix))
                    checks.rejected(name+'.missing_dependency_reload','vfx.reload',path=path)
                    state=checks.state(handle)
                    checks.record(name+'.old_instance_survives',state.get('playing') is True and state.get('paused') is True,state=state)
                    # Fresh key defeats Runtime Effect cache when checking dependency rejection.
                    missing=scratch/(name+'-missing.efkpkg');owned.append(missing);missing.write_bytes(fixture.read_bytes())
                    checks.rejected(name+'.missing_dependency_new_load','vfx.reload',path=missing.relative_to(ROOT/'Project').as_posix())
                    checks.stop(handle);checks.call('runtime.step',frames=30)
                    stopped=checks.capture(name+'-stopped');report['visualEvidence'].append(stopped)
                    report.setdefault('stopPixelMetrics',{})[name]=difference(background,pixels(stopped['path']))
                    if suffix=='.efkmat' and cache is not None:
                        compiled_probe(checks,report,source,scratch,owned,cache,args.backend,output/(name+'-step2.png'))
            finally:
                try:
                    for handle in list(checks.handles):checks.stop(handle)
                    checks.call('runtime.step',frames=30)
                    write_json(output/'final-stats.json',checks.call('vfx.stats'))
                finally:
                    try:session.stop()
                    except Exception as error:report['cleanupError']=str(error);raise
            report['status']='FAIL' if any(c['status']=='FAIL' for c in report['checks']) else 'PASS'
            run['status']='success' if report['status']=='PASS' else 'failed'
        except Exception as error:
            report['status']='FAIL';report['error']=str(error)
            run['status']='unknown' if report.get('cleanupError') else 'failed'
        finally:
            for key,value in environment.items():
                if value is None:os.environ.pop(key,None)
                else:os.environ[key]=value
            if not report.get('cleanupError'):
                for path in owned:path.unlink(missing_ok=True)
                scratch.rmdir()
            report['inputsUnchanged']=all(digest(Path(p))==h for p,h in inputs.items())
            report['binaryUnchanged']=digest(exe)==pinned
            if not report['inputsUnchanged'] or not report['binaryUnchanged']:
                report['status']='FAIL';run['status']='failed'
            write_json(output/'report.json',report)
    print(json.dumps(dict(status=report['status'],report=str(output/'report.json'))))
    return 0 if report['status']=='PASS' else 1


if __name__=='__main__':raise SystemExit(main())
