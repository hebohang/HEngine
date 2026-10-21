"""Paused owned-process memory sampling: diagnosis only, never budget acceptance."""
import argparse
import json
import math
import os
from pathlib import Path
import statistics
import subprocess

from preview import ROOT, Session, Storage, digest, write_json
from lifecycle_acceptance import stress_identity, live_cycle_valid

EFFECT='Assets/Effects/Stress/GpuSparks20k.efkpkg'
SCENE='Assets/Scenes/VFXAuthoringNeutral.he'
EXE=ROOT/'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'


def owner_identity(session):
    owner=json.loads((ROOT/'out/agent/sessions/active.json').read_text(encoding='utf-8-sig'))
    if owner.get('ownerTag','').lower()!=session.owner.lower():
        raise RuntimeError('Memory sampler does not own this session')
    pid,ticks=int(owner['pid']),int(owner['processStartUtcTicks'])
    if pid<=0 or ticks<=0: raise ValueError('Invalid owned process identity')
    return pid,ticks


def parse_samples(text, pid, segments):
    rows=[json.loads(line) for line in text.splitlines() if line.strip()]
    if len(rows)!=100*segments: raise ValueError('Incomplete 10 Hz memory sample sequence')
    previous=-1
    for index,row in enumerate(rows):
        if row.get('pid')!=pid or row.get('segment')!=index//100 or row.get('sample')!=index%100:
            raise ValueError('Memory sample identity or ordering mismatch')
        for key in ('elapsedMs','privateBytes','workingSetBytes'):
            value=row.get(key)
            if isinstance(value,bool) or not isinstance(value,(int,float)) or not math.isfinite(value) or value<0:
                raise ValueError('Invalid memory sample value')
        if row['elapsedMs']<=previous: raise ValueError('Memory sample time is not increasing')
        previous=row['elapsedMs']
    return rows


def sample_process(session, segments):
    pid,ticks=owner_identity(session)
    # One process and one stopwatch for all contiguous segments. It only reads
    # the owned PID and never sends Agent requests or changes process state.
    script=f'''$ErrorActionPreference='Stop'
$sampleProcess=[Diagnostics.Process]::GetProcessById({pid})
$sampleClock=[Diagnostics.Stopwatch]::StartNew()
try {{
    for($sampleIndex=0; $sampleIndex -lt {100*segments}; $sampleIndex++) {{
        $remaining=($sampleIndex*100)-$sampleClock.Elapsed.TotalMilliseconds
        if($remaining -gt 0) {{ Start-Sleep -Milliseconds ([int][Math]::Ceiling($remaining)) }}
        $sampleProcess.Refresh()
        if($sampleProcess.HasExited -or $sampleProcess.StartTime.ToUniversalTime().Ticks -ne {ticks}) {{ throw 'Owned PID identity changed' }}
        @{{pid=$sampleProcess.Id; segment=[int][Math]::Floor($sampleIndex/100); sample=$sampleIndex%100; elapsedMs=$sampleClock.Elapsed.TotalMilliseconds; privateBytes=$sampleProcess.PrivateMemorySize64; workingSetBytes=$sampleProcess.WorkingSet64}} | ConvertTo-Json -Compress
    }}
}} finally {{ $sampleProcess.Dispose() }}
'''
    script_path=session.output/'memory-sampler.ps1'
    script_path.write_text(script,encoding='utf-8')
    stdout=session.output/'memory-samples.jsonl'; stderr=session.output/'memory-sampler.stderr.log'
    with stdout.open('w',encoding='utf-8') as out,stderr.open('w',encoding='utf-8') as err:
        result=subprocess.run(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(script_path)],
            stdout=out,stderr=err,timeout=segments*10+20,creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode: raise RuntimeError('Owned memory sampler failed; see sampler stderr')
    return parse_samples(stdout.read_text(encoding='utf-8-sig'),pid,segments)


def summarize(rows):
    groups=[]
    for segment in sorted({row['segment'] for row in rows}):
        segment_rows=[row for row in rows if row['segment']==segment]
        values=[row['privateBytes'] for row in segment_rows]
        intervals=[b['elapsedMs']-a['elapsedMs'] for a,b in zip(segment_rows,segment_rows[1:])]
        groups.append(dict(segment=segment,count=len(values),minimum=min(values),maximum=max(values),
                           median=statistics.median(values),first=values[0],last=values[-1],
                           observedSpanMs=segment_rows[-1]['elapsedMs']-segment_rows[0]['elapsedMs'],
                           minimumIntervalMs=min(intervals),maximumIntervalMs=max(intervals)))
    return groups


def is_retired(stats):
    return all(stats.get(key,{}).get('current')==0 for key in ('activeEffects','cpuInstances')) and all(
        stats.get('gpuAllocation',{}).get(key)==0 for key in ('allocatedEmitters','reservedParticles','reservedTrails'))


def execute(session, report, segments, sampler=sample_process):
    handle=None
    try:
        session.start()
        baseline=session.call('vfx.stats')
        if baseline.get('gpuEnabled') is not True or not is_retired(baseline):
            raise RuntimeError('Expected empty GPU-enabled baseline')
        handle=session.call('vfx.play',dict(path=EFFECT,seed=1806))['handle']
        if handle<0: raise RuntimeError('Stress play rejected')
        for count in (120,81): session.call('runtime.step',dict(frames=count))
        live=session.call('vfx.stats')
        if not live_cycle_valid(baseline,live): raise RuntimeError('Missing real stress GPU work')
        session.call('vfx.control',dict(handle=handle,action='stop'))
        session.call('runtime.step',dict(frames=30))
        report['beforeStats']=session.call('vfx.stats')
        report['beforeSession']=session.call('session.status')
        if not is_retired(report['beforeStats']): raise RuntimeError('Effect did not retire before sampling')
        report['samples']=sampler(session,segments)
        report['afterStats']=session.call('vfx.stats')
        report['afterSession']=session.call('session.status')
        report['segments']=summarize(report['samples'])
        before,after=report['beforeStats'],report['afterStats']
        report['simulationUnchanged']=before['updateSerial']==after['updateSerial']
        report['presentationFrameDelta']=report['afterSession']['completedRenderFrames']-report['beforeSession']['completedRenderFrames']
        report['vfxRecordingDeltas']={key:after[key]-before[key] for key in
            ('recordingSerial','totalRecordedDrawCount','totalRecordedDispatchCount')}
        if not is_retired(after) or not report['simulationUnchanged'] or report['presentationFrameDelta']<=0:
            raise RuntimeError('Sampling phase was not retired, simulation-paused and presentation-active')
        report['diagnosticStatus']='COMPLETE'
    finally:
        try:
            if handle is not None: session.call('vfx.control',dict(handle=handle,action='stop'))
        except Exception as error: report['effectCleanupError']=str(error)
        finally: session.stop()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend',choices=['Vulkan','DX12'],required=True)
    parser.add_argument('--existing-build-sha256',required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--segments',type=int,choices=[1,2,3],default=3)
    parser.add_argument('--gpu-probe',action='store_true')
    args=parser.parse_args()
    pinned=digest(EXE)
    if pinned.lower()!=args.existing_build_sha256.lower(): parser.error('Executable hash mismatch')
    identity=stress_identity(ROOT/'Project'/EFFECT,20000)
    inputs=[ROOT/'Project'/EFFECT,ROOT/'Project'/SCENE,Path(__file__).resolve(),
            ROOT/'Tools/VFX/preview.py',ROOT/'Tools/VFX/lifecycle_acceptance.py',
            ROOT/'Tools/VFX/generate_effekseer_stress_assets.py',ROOT/'Tools/Agent/storage.py']
    hashes={str(path.relative_to(ROOT)):digest(path) for path in inputs}
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=False)
    session=Session(output,args.backend,pinned,scene=SCENE)
    report=dict(diagnosticStatus='RUNNING',acceptanceStatus='NOT_ASSESSED',backend=args.backend,
                evidenceType='runtime-manual',executableSha256=pinned,nominalIdentity=identity,inputHashes=hashes,
                samplingHz=10,secondsPerSegment=10,segmentCount=args.segments,
                scope='One play201-stop30 then contiguous passive owned-PID samples. No acceptance threshold, budget PASS, added warmup or changed lifecycle criterion; before/after Agent snapshots are not atomic with OS samples.')
    previous=os.environ.get('HE_VFX_GPU_PARTICLES')
    with Storage(ROOT).workflow_scope(str(ROOT/'Project'),'vfx-memory-plateau-diagnostic',[output]) as run:
        try:
            if args.gpu_probe: os.environ['HE_VFX_GPU_PARTICLES']='1'
            else: os.environ.pop('HE_VFX_GPU_PARTICLES',None)
            execute(session,report,args.segments)
        except Exception as error:
            report['diagnosticStatus']='FAILED'; report['error']=str(error)
        finally:
            if previous is None: os.environ.pop('HE_VFX_GPU_PARTICLES',None)
            else: os.environ['HE_VFX_GPU_PARTICLES']=previous
            report['inputsUnchanged']=all(path.is_file() and digest(path)==hashes[str(path.relative_to(ROOT))] for path in inputs)
            report['binaryUnchanged']=EXE.is_file() and digest(EXE)==pinned
            logs=[output/'game.stdout.log',output/'game.stderr.log']
            report['nativeLogs']={path.name:dict(path=str(path),sha256=digest(path)) for path in logs if path.is_file()}
            if not report['inputsUnchanged'] or not report['binaryUnchanged'] or len(report['nativeLogs'])!=2:
                report['diagnosticStatus']='FAILED'
            run['status']='success' if report['diagnosticStatus']=='COMPLETE' else 'unknown'
            write_json(output/'report.json',report)
    print(json.dumps({key:report[key] for key in ('diagnosticStatus','acceptanceStatus')}))
    return 0 if report['diagnosticStatus']=='COMPLETE' else 1


if __name__=='__main__': raise SystemExit(main())
