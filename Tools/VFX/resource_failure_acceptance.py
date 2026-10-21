"""Same-key malformed/dependency reload rollback probe; pinned owned session, no builds."""
import argparse
import io
import json
import os
from pathlib import Path
import struct
import uuid
import zipfile

from preview import ROOT, Session, Storage, digest, write_json
from parameter_acceptance import Checks, numeric_match, recipe


def mutation(path, kind):
    with zipfile.ZipFile(path) as z: entries={n:z.read(n) for n in z.namelist()}
    if kind=='missing_texture':
        names=[n for n in entries if n.lower().endswith('.png')]
        assert names
        for name in names: del entries[name]
    elif kind=='missing_model':
        names=[n for n in entries if n.lower().endswith('.efkmodel')]
        assert names
        for name in names: del entries[name]
    else:
        name=next(n for n in entries if n.endswith('.efkefc'))
        data=bytearray(entries[name]);assert data[:4]==b'EFKE'
        if kind=='outer_version': struct.pack_into('<I',data,4,0x7fffffff)
        else:
            offset=8
            while offset<len(data):
                size=struct.unpack_from('<I',data,offset+4)[0]
                if data[offset:offset+4]==b'BIN_':
                    assert data[offset+8:offset+12]==b'SKFE'
                    struct.pack_into('<I',data,offset+12,0x7fffffff);break
                offset+=8+size
            else: raise ValueError('No BIN_ chunk')
        entries[name]=bytes(data)
    out=io.BytesIO()
    with zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED) as z:
        for name,data in sorted(entries.items()): z.writestr(name,data)
    return out.getvalue()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend',choices=['DX12','Vulkan'],required=True)
    parser.add_argument('--existing-build-sha256',required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--legacy-sampler-package',type=Path,
        help='Preserved invalid 1800 GpuSparks20k package; enables GPU so rejection cannot be GPU-disabled policy')
    args=parser.parse_args()
    exe=ROOT/'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe';pinned=digest(exe)
    if pinned.lower()!=args.existing_build_sha256.lower():parser.error('Pinned candidate mismatch')
    good=ROOT/'Project/Assets/Effects/Authoring/hit-ember.efkpkg'
    sprite=ROOT/'Project/Assets/Effects/AuthoringPathValidation/CpuSpriteControl.efkpkg'
    model=ROOT/'Project/Assets/Effects/AuthoringPathValidation/CpuModel.efkpkg'
    inputs={str(p):digest(p) for p in (good,sprite,model)}
    legacy=args.legacy_sampler_package.resolve() if args.legacy_sampler_package else None
    kinds=['missing_texture','missing_model','outer_version','binary_version']
    if legacy:
        # This negative case must use the preserved pre-migration package, not
        # the replacement official-compiled 1810 asset at the old runtime path.
        with zipfile.ZipFile(legacy) as z:
            raw=z.read(next(n for n in z.namelist() if n.endswith('.efkefc')))
        from generate_effekseer_stress_assets import parse_chunks
        binary=dict(parse_chunks(raw))[b'BIN_']
        if binary[:4]!=b'SKFE' or struct.unpack_from('<I',binary,4)[0]!=1800:
            parser.error('Legacy negative fixture must retain its original 1800 runtime payload')
        inputs[str(legacy)]=digest(legacy);kinds.append('legacy_sampler')
    radius=recipe(good)['compile']['radius_m']
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    scratch=ROOT/'Project/Assets/Effects/ResourceFailureAcceptance'/uuid.uuid4().hex
    scratch.mkdir(parents=True,exist_ok=False)
    fixture=scratch/'candidate.efkpkg';relative=fixture.relative_to(ROOT/'Project').as_posix()
    report=dict(status='RUNNING',evidenceType='runtime-manual',backend=args.backend,executableSha256=pinned,
        inputs=inputs,checks=[],visualEvidence=[],excluded=['material/curve/sound failure fixtures','particle-state hash','visual approval'])
    report['legacySamplerCoverage']='REQUESTED' if legacy else 'NOT_REQUESTED'
    session=Session(output,args.backend,pinned);checks=Checks(session,report)
    old={k:os.environ.get(k) for k in ('HE_VFX_GPU_PARTICLES','HE_VFX_DISABLE_STATS')}
    os.environ['HE_VFX_GPU_PARTICLES']='1' if legacy else '0';os.environ['HE_VFX_DISABLE_STATS']='0'
    with Storage(ROOT).workflow_scope(str(ROOT/'Project'),'vfx-resource-failure',[output]) as run:
        try:
            try:
                session.start();checks.call('runtime.pause');checks.call('runtime.step',frames=30)
                if legacy:
                    stats=checks.call('vfx.stats')
                    checks.record('legacy_sampler.gpu_enabled',stats.get('gpuEnabled') is True,stats=stats)
                    if stats.get('gpuEnabled') is not True:raise RuntimeError('Legacy sampler probe requires enabled GPU support')
                for kind in kinds:
                    fixture.write_bytes(good.read_bytes())
                    loaded=checks.call('vfx.reload',path=relative)
                    checks.record(kind+'.baseline_reload',loaded.get('accepted') is True,receipt=loaded)
                    handle=checks.play(relative);checks.call('runtime.step',frames=8)
                    checks.control(handle,'pause');checks.control(handle,'parameter',name='intensity',value=.5)
                    checks.call('runtime.step',frames=1)
                    before=checks.state(handle)
                    report['visualEvidence'].append(checks.capture(kind+'-before'))
                    fixture.write_bytes(legacy.read_bytes() if kind=='legacy_sampler' else
                        mutation(model if kind=='missing_model' else sprite,kind))
                    checks.rejected(kind+'.reload_rejected','vfx.reload',path=relative)
                    # Same key while corrupt bytes remain on disk: success must
                    # come from the restored old cache, not a repaired file.
                    fresh=checks.play(relative);checks.call('runtime.step',frames=1)
                    after,new=checks.state(handle),checks.state(fresh)
                    checks.record(kind+'.old_instance_and_cache_retained',after.get('playing') is True
                        and after.get('paused') is True and numeric_match(after.get('intensity'),.5)
                        and numeric_match(after.get('authoredRadiusMeters'),radius,1e-5)
                        and new.get('playing') is True and numeric_match(new.get('authoredRadiusMeters'),radius,1e-5),
                        before=before,after=after,newFromOldCache=new,
                        scope='Real same-key rollback plus SDK state/old radius metadata; image evidence is not state hashing')
                    report['visualEvidence'].append(checks.capture(kind+'-after'))
                    write_json(output/(kind+'-stats.json'),checks.call('vfx.stats'))
                    checks.stop(handle);checks.stop(fresh);checks.call('runtime.step',frames=30)
            finally:
                try:
                    for handle in list(checks.handles): checks.stop(handle)
                    checks.call('runtime.step',frames=30)
                finally:
                    try: session.stop()
                    except Exception as error: report['cleanupError']=str(error);raise
            if legacy:
                matched=[]
                with (output/'game.stdout.log').open(encoding='utf-8-sig',errors='replace') as log:
                    for line in log:
                        if 'Invalid built-in CPU sampler; package rejected:' in line and 'filter=-1' in line and 'wrap=-1' in line:
                            matched.append(line.strip()[:1000])
                            if len(matched)==4:break
                checks.record('legacy_sampler.explicit_runtime_reason',bool(matched),nativeLogEvidence=matched)
            report['status']='FAIL' if any(c['status']=='FAIL' for c in report['checks']) else 'PASS'
            run['status']='success' if report['status']=='PASS' else 'failed'
        except Exception as error:
            report['status']='FAIL';report['error']=str(error)
            run['status']='unknown' if report.get('cleanupError') else 'failed'
        finally:
            for key,value in old.items():
                if value is None:os.environ.pop(key,None)
                else:os.environ[key]=value
            if not report.get('cleanupError'):
                fixture.unlink(missing_ok=True);scratch.rmdir()
            report['inputsUnchanged']=all(digest(Path(p))==h for p,h in inputs.items())
            report['binaryUnchanged']=digest(exe)==pinned
            if not report['inputsUnchanged'] or not report['binaryUnchanged']:
                report['status']='FAIL';run['status']='failed'
            write_json(output/'report.json',report)
    print(json.dumps(dict(status=report['status'],report=str(output/'report.json'))))
    return 0 if report['status']=='PASS' else 1


if __name__=='__main__':raise SystemExit(main())
