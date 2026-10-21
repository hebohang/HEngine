"""Serial, owned VFX lifecycle acceptance against a pinned current candidate.

Does not build, change packages, claim visual review, or benchmark shipping cost.
Inputs must be finite one-shot effects that end within --natural-end-frames.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import uuid
import zipfile

from preview import ROOT, Session, Storage, write_json

ALLOCATION_FAILURES = ('emitterSlotFailures', 'particleBlockFailures', 'trailBlockFailures')
ALLOCATION_RESERVED = ('allocatedEmitters', 'reservedParticles', 'reservedTrails')
BUDGET = ROOT/'Tools/VFX/acceptance-budget.json'
POOL_FIELDS = ('storageBufferCount','storageLogicalBytes','uniformBufferCount','uniformLogicalBytes','uniformSnapshotCount')


def resource_trend(values):
    if len(values)<2 or any(not isinstance(v,(int,float)) or isinstance(v,bool) or not math.isfinite(v) or v<0 for v in values):
        return dict(status='FAIL',reason='Missing or invalid nonnegative resource samples')
    growth=statistics.median(values[-5:])-statistics.median(values[:5])
    mean_x,mean_y=(len(values)-1)/2,statistics.mean(values)
    slope=sum((i-mean_x)*(v-mean_y) for i,v in enumerate(values))/sum((i-mean_x)**2 for i in range(len(values)))
    return dict(status='PASS' if no_resource_growth(values,growth,slope) else 'FAIL',
        baseline=values[0],maximum=max(values),medianWindowGrowth=growth,slopePerCycle=slope)


def resource_observations(settled):
    pools=[sample.get('stats',{}).get('bufferPools',{}) for sample in settled]
    available=bool(pools) and all(p.get('available') is True for p in pools)
    trends={key:resource_trend([p.get(key) for p in pools]) for key in POOL_FIELDS}
    result=[dict(name='cycles_held_buffer_pools',status='PASS' if available and all(t['status']=='PASS' for t in trends.values()) else 'FAIL',
        available=available,fields=trends,warmupCycles=5,retirementFrames=30,
        scope='Registered LLGI storage/uniform held buffers, deduplicated identity and GetSize logical bytes; resident pools need not be zero. Excludes native allocation padding, textures, vertex/index, command-only uploads and frame pools; not fence-retired memory')]
    memory=[sample.get('stats',{}).get('renderMemory',{}) for sample in settled]
    for segment,key in [('local','localUsageBytes'),('nonlocal','nonLocalUsageBytes')]:
        eligible=[m.get('available') is True and m.get('perProcess') is True and
                  (segment=='local' or m.get('nonLocalAvailable') is True) for m in memory]
        if not any(eligible):
            trend=dict(status='NOT_VERIFIED',reason='Per-process OS usage unavailable; not treated as zero')
        elif not all(eligible):
            trend=dict(status='FAIL',reason='OS usage availability changed during settled samples')
        else:
            trend=resource_trend([m.get(key) for m in memory])
        result.append(dict(name='cycles_os_'+segment+'_memory_trend',**trend,warmupCycles=5,retirementFrames=30,
            scope='RenderDevice OS current-process usage at equal post-stop stages; not VFX-only or exact native allocation'))
    return result


def no_resource_growth(values, growth, slope):
    return bool(values) and all(isinstance(v,(int,float)) and math.isfinite(v) and v>=0 for v in values) and growth<=0 and slope<=0 and max(values)<=values[0]


def stress_identity(path, nominal):
    from generate_effekseer_stress_assets import GenerationError, validate_compiled_stress
    with zipfile.ZipFile(path) as archive:
        metadata=json.loads(archive.read('PROVENANCE.json'))
        effects=[name for name in archive.namelist() if name.endswith('.efkefc')]
        if len(effects)!=1: raise ValueError('Exactly one compiled stress effect required')
        payload=archive.read(effects[0])
    parameters=metadata['parameters']
    if nominal not in (20000,50000) or metadata.get('estimated_peak_particles')!=nominal or parameters['lifetime_frames']*parameters['emit_per_frame']!=nominal:
        raise ValueError('Stress nominal metadata mismatch')
    try:
        runtime=validate_compiled_stress(payload,nominal)
    except GenerationError as error:
        raise ValueError('Compiled stress validation failed: '+str(error)) from error
    for authored,compiled in (('lifetime_frames','lifetimeFrames'),('emit_per_frame','emitPerFrame'),
                              ('emit_count','emitCount'),('owner_lifetime_frames','ownerLifetimeFrames'),
                              ('fixed_random_seed','seed')):
        if parameters.get(authored)!=runtime[compiled]:
            raise ValueError('Stress metadata differs from compiled '+authored)
    return dict(nominalParticles=nominal,packageSha256=digest(path),scope='Compiled capacity/rate/lifetime; not live GPU particle count readback')


def live_cycle_valid(before, after):
    for metric_name,identity in [('gpuSimulationMs','lastGpuSimulationSubmission'),('gpuRenderMs','lastGpuRenderSubmission')]:
        value=after.get(metric_name,{})
        current=value.get('current')
        if value.get('status')!=1 or not isinstance(current,(int,float)) or isinstance(current,bool) or not math.isfinite(current) or current<=0 or after.get(identity,0)<=before.get(identity,0):return False
    return all(after.get(key,0)>before.get(key,0) for key in ('totalRecordedDispatchCount','totalRecordedDrawCount'))


def combine_reports(paths):
    reports=[json.loads(Path(path).read_text()) for path in paths]
    nominal={r.get('nominalIdentity',{}).get('nominalParticles') for r in reports}
    valid=len(reports)==2 and nominal=={20000,50000}
    valid=valid and len({r.get('executableSha256') for r in reports})==1 and len({r.get('backend') for r in reports})==1
    valid=valid and all(r.get('budgetSha256')==digest(BUDGET) and r.get('budgetUnchanged') is True and
        r.get('commandStatus')=='PASS' and len(r.get('cycleSamples',[]))>=30 for r in reports)
    for report in reports:
        checks={c['name']:c['status'] for c in report.get('checks',[])}
        valid=valid and all(checks.get('cycle_'+str(i)+'.'+suffix)=='PASS'
            for i in range(30) for suffix in ('live_work','retired_zero'))
        valid=valid and checks.get('cycles_private_memory_trend')=='PASS'
        valid=valid and checks.get('cycles_held_buffer_pools')=='PASS'
        valid=valid and all(checks.get('cycles_os_'+segment+'_memory_trend') in ('PASS','NOT_VERIFIED') for segment in ('local','nonlocal'))
    return dict(commandStatus='PASS' if valid else 'FAIL',acceptanceStatus='PARTIAL' if valid else 'FAIL',
        reports=[dict(path=str(p),sha256=digest(p)) for p in paths],
        scope='Both nominal stress scales; held storage/uniform logical pools observed. OS usage only when available; exact native/fence-retired bytes, other resource classes and visual scopes remain unverified')


def digest(path):
    # The system Python can predate hashlib.file_digest (3.11).
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            value.update(block)
    return value.hexdigest()


def metric(stats, name):
    entry = stats.get(name, {})
    return entry.get('current') if isinstance(entry, dict) else None


def owned_memory(session):
    """Read only our exact PID/start-time pair; never expose the owner token."""
    owner = json.loads((ROOT/'out/agent/sessions/active.json').read_text(encoding='utf-8-sig'))
    if owner.get('ownerTag', '').lower() != session.owner.lower():
        raise RuntimeError('Memory sample owner differs from this lifecycle session')
    pid, ticks = int(owner['pid']), int(owner['processStartUtcTicks'])
    script = (
        f'$p=[Diagnostics.Process]::GetProcessById({pid}); '
        f'if($p.StartTime.ToUniversalTime().Ticks -ne {ticks}){{throw "PID identity changed"}}; '
        '$p.Refresh(); @{pid=$p.Id; privateBytes=$p.PrivateMemorySize64; '
        'workingSetBytes=$p.WorkingSet64} | ConvertTo-Json -Compress')
    result = subprocess.run(['powershell.exe', '-NoProfile', '-Command', script],
        capture_output=True, text=True, timeout=15, creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode:
        raise RuntimeError('Owned process memory sample failed')
    return json.loads(result.stdout)


class Acceptance:
    def __init__(self, session, report, args):
        self.session, self.report, self.args = session, report, args
        self.handles = set()

    def record(self, name, status, **evidence):
        self.report['checks'].append(dict(name=name, status=status, **evidence))
        write_json(self.session.output/'report.json', self.report)

    def call(self, method, **params):
        response = self.session.call(method, params)
        if method == 'vfx.stats':
            allocation = response.get('gpuAllocation', {})
            if any(not isinstance(allocation.get(key), int) or allocation[key] < 0
                   for key in ALLOCATION_FAILURES + ALLOCATION_RESERVED):
                raise RuntimeError('Candidate lacks valid CPU GPU-allocator telemetry')
            baseline = self.report.get('baseline', {}).get('gpuAllocation')
            if any(allocation[key] != (baseline[key] if baseline else 0) for key in ALLOCATION_FAILURES):
                self.record('gpu_allocation_failure', 'FAIL', allocation=allocation,
                    baseline=baseline, scope='Current GPU system lifetime; counter reset also invalidates comparison')
                raise RuntimeError('GPU allocator failure count changed; successful handles are insufficient')
        return response

    def step(self, frames):
        while frames:
            count = min(frames, 120)
            self.call('runtime.step', frames=count)
            frames -= count

    def status(self, handle):
        return self.call('vfx.control', handle=handle, action='status')

    def play(self, path, seed=None):
        result = self.call('vfx.play', path=path, seed=self.args.seed if seed is None else seed)
        handle = result['handle']
        self.handles.add(handle)
        if not result.get('playing'):
            raise RuntimeError('Play returned a non-playing handle')
        return handle

    def stop(self, handle):
        receipt = self.call('vfx.control', handle=handle, action='stop')
        self.handles.discard(handle)
        return receipt

    def capture(self, label):
        receipt = self.call('capture.frame', filename=label+'.png')
        source = Path(receipt['path'])
        target = self.session.output/(label+'.png')
        shutil.copyfile(source, target)
        return dict(path=str(target), sha256=digest(target), receipt=receipt)

    def wait_end(self, handle):
        stepped = 0
        while stepped < self.args.natural_end_frames:
            count = min(120, self.args.natural_end_frames-stepped)
            self.step(count)
            stepped += count
            status = self.status(handle)
            if not status.get('playing'):
                self.handles.discard(handle)
                return True, stepped, status
        return False, stepped, status

    def expect_rejected(self, name, method, params):
        start = len(self.session.commands)
        rejected = False
        evidence = {}
        try:
            response = self.session.call(method, params)
            if method == 'vfx.play' and response.get('playing') and 'handle' in response:
                self.handles.add(response['handle'])
            rejected = response.get('accepted') is False
            evidence['response'] = response
        except RuntimeError as error:
            # A transport/process failure is not an expected parameter rejection.
            if len(self.session.commands) <= start:
                raise
            logfile = Path(self.session.commands[-1]['stdout'])
            raw = logfile.read_text(encoding='utf-8-sig', errors='replace')
            offset = raw.find('{')
            try:
                response = json.loads(raw[offset:]) if offset >= 0 else {}
            except json.JSONDecodeError:
                response = {}
            error_code = response.get('error', {}).get('code') if isinstance(response.get('error'),dict) else ''
            rejected = response.get('ok') is False and error_code == 'invalid_params'
            evidence = dict(receipt=response, commandError=str(error))
        # Independently prove the session remains responsive after rejection.
        evidence['sessionAfter'] = self.call('session.status')
        self.record(name, 'PASS' if rejected else 'FAIL', **evidence)

    def finite_lifecycle(self, kind, path):
        handle = self.play(path)
        self.step(1)
        for speed in [0.0, 0.5, 1.0]:
            receipt = self.call('vfx.control', handle=handle, action='speed', value=speed)
            state = self.status(handle)
            self.record(f'{kind}.speed_{speed}',
                'PASS' if receipt.get('accepted') is True and state.get('speed') == speed else 'FAIL',
                state=state, scope='SDK effective speed; numerical particle integration requires state readback')
        paused = self.call('vfx.control', handle=handle, action='pause')
        before = self.status(handle)
        frozen = self.capture(kind+'-paused-start')
        self.step(self.args.natural_end_frames)
        after = self.status(handle)
        frozen_end = self.capture(kind+'-paused-end')
        pause_state = (paused.get('accepted') is True and before.get('playing') is True and
                       after.get('playing') is True)
        if before.get('paused') is None or after.get('paused') is None:
            status = 'NOT_VERIFIED'
        else:
            status = 'PASS' if pause_state and before['paused'] and after['paused'] else 'FAIL'
        self.record(kind+'.pause_lifetime', status, before=before, after=after,
            heldFrames=self.args.natural_end_frames, captures=[frozen, frozen_end],
            scope='SDK paused state and no natural expiry; pixel/simulation-state freezing requires separate review')
        resumed = self.call('vfx.control', handle=handle, action='resume')
        resume_status = self.status(handle)
        ended, frames, final = self.wait_end(handle)
        verified_resume = resume_status.get('paused') is False
        self.record(kind+'.resume_natural_end', 'PASS' if resumed.get('accepted') and verified_resume and ended else 'FAIL',
            resumed=resume_status, ended=final, framesAfterResume=frames)
        if not ended:
            self.stop(handle)
        self.step(30)

        replays = []
        for index in range(2):
            replay = self.play(path)
            self.step(self.args.sample_frames)
            replays.append(dict(handle=replay, status=self.status(replay),
                capture=self.capture(f'{kind}-replay-{index}'), stats=self.call('vfx.stats')))
            self.stop(replay)
            self.step(30)
            stopped = self.status(replay)
            self.record(f'{kind}.stop_replay_{index}', 'PASS' if not stopped.get('playing') else 'FAIL', after=stopped)
        self.record(kind+'.same_seed_replay_commands', 'PASS', seed=self.args.seed, replays=replays,
            scope='Same seed submitted on two fresh handles; not proof of deterministic GPU state')
        self.record(kind+'.seed_simulation_reproducibility', 'NOT_VERIFIED',
            captures=[entry['capture'] for entry in replays],
            reason='No simulation-state hash or reset of global renderer time. Matching screenshots alone would not prove seed application.')

    def multiple_views(self):
        # No runtime.step anywhere in this probe: both views reuse the last update.
        clock_before = self.call('session.status')
        before = self.call('vfx.stats')
        created = False
        try:
            initial = self.call('render.view.create', width=640, height=360)
            created = True
            self.call('render.view.capture')
            status = initial
            for _ in range(12):
                status = self.call('render.view.status')
                if status.get('capture', {}).get('status') in {'complete','failed'}:
                    break
            after = self.call('vfx.stats')
            clock_after = self.call('session.status')
            capture = status.get('capture', {})
            evidence = dict(view=status, before=before, after=after,
                simulationTimeBefore=clock_before.get('simulationTimeSeconds'),
                simulationTimeAfter=clock_after.get('simulationTimeSeconds'))
            evidence['simulationTiming'] = dict(beforeSubmission=before.get('lastGpuSimulationSubmission'),
                afterSubmission=after.get('lastGpuSimulationSubmission'), sample=after.get('gpuSimulationMs'),
                scope='May retire pre-view work; not used to claim a new simulation for this view')
            render_timing = after.get('gpuRenderMs', {})
            render_fresh = after.get('lastGpuRenderSubmission',0) > before.get('lastGpuRenderSubmission',0)
            evidence['renderTiming'] = dict(beforeSubmission=before.get('lastGpuRenderSubmission'),
                afterSubmission=after.get('lastGpuRenderSubmission'), sample=render_timing)
            if capture.get('status') == 'complete':
                target = self.session.output/'second-view.png'
                shutil.copyfile(capture['path'], target)
                evidence['copiedCapture'] = dict(path=str(target), sha256=digest(target))
            same_time = (clock_before.get('simulationTimeSeconds') is not None and
                         clock_before.get('simulationTimeSeconds') == clock_after.get('simulationTimeSeconds'))
            dispatch_same = before.get('totalRecordedDispatchCount') == after.get('totalRecordedDispatchCount')
            draw_progress = after.get('totalRecordedDrawCount',0) > before.get('totalRecordedDrawCount',0)
            graph = status.get('hasRecordedGraph') and capture.get('status') == 'complete'
            verdict = ('FAIL' if same_time and not dispatch_same else
                       'PASS' if same_time and dispatch_same and draw_progress and graph else 'NOT_VERIFIED')
            self.record('multiview_no_repeat_compute', verdict, **evidence,
                scope='Second-view graph/capture while simulation time is fixed; global draw progress is not per-view VFX pixel proof')
            self.record('multiview_render_timing',
                'PASS' if render_fresh and render_timing.get('samples',0)>0 and render_timing.get('current',0)>0 else 'NOT_VERIFIED',
                **evidence['renderTiming'], scope='Fresh completed rendering timer; no per-view timer attribution')
        finally:
            if created:
                self.call('render.view.destroy')
        destroyed = self.call('render.view.status')
        self.record('multiview_destroy', 'PASS' if destroyed.get('available') is False else 'FAIL', state=destroyed)

    def run(self):
        self.call('runtime.pause')
        self.report['session'] = self.call('session.status')
        self.report['capabilities'] = self.call('vfx.describe')
        self.step(30)
        baseline = self.call('vfx.stats')
        self.report['baseline'] = baseline
        if metric(baseline, 'activeEffects') != 0:
            raise RuntimeError('Lifecycle scene must begin without active effects')
        self.record('gpu_probe_enabled', 'PASS' if baseline.get('gpuEnabled') else 'FAIL', stats=baseline)
        if not baseline.get('gpuEnabled'):
            raise RuntimeError('Pinned candidate did not enable the requested GPU development probe')

        for kind, path in [('cpu', self.args.cpu_effect), ('gpu', self.args.gpu_effect)]:
            self.finite_lifecycle(kind, path)

        cpu, gpu = self.play(self.args.cpu_effect), self.play(self.args.gpu_effect)
        start = self.call('vfx.stats')
        self.step(self.args.sample_frames)
        active = self.call('vfx.stats')
        cpu_status, gpu_status = self.status(cpu), self.status(gpu)
        simulation = active.get('gpuSimulationMs', {})
        dispatch_delta = active.get('totalRecordedDispatchCount', 0)-start.get('totalRecordedDispatchCount', 0)
        self.record('cpu_gpu_concurrent', 'PASS' if cpu_status.get('playing') and gpu_status.get('playing') and dispatch_delta > 0 else 'FAIL',
            cpu=cpu_status, gpu=gpu_status, dispatchDelta=dispatch_delta, stats=active,
            capture=self.capture('cpu-gpu-concurrent'), scope='Concurrent handles and accepted native compute recording; image is unreviewed')
        fresh_timing = active.get('lastGpuSimulationSubmission',0) > start.get('lastGpuSimulationSubmission',0)
        self.record('gpu_simulation_timing', 'PASS' if fresh_timing and simulation.get('samples', 0)>0 and simulation.get('current', 0)>0 else 'FAIL',
            timing=simulation, lastGpuSimulationSubmission=active.get('lastGpuSimulationSubmission'),
            scope='Completed GPU timer sample, distinct from native recording counts; not a particle-state readback')
        self.multiple_views()

        invalid = [
            ('negative_scale', 'vfx.play', dict(path=self.args.cpu_effect, scale=-1)),
            ('oversized_speed', 'vfx.play', dict(path=self.args.cpu_effect, speed=17)),
            ('seed_overflow', 'vfx.play', dict(path=self.args.cpu_effect, seed=2147483648)),
            ('seed_underflow', 'vfx.play', dict(path=self.args.cpu_effect, seed=-2147483649)),
            ('invalid_position', 'vfx.play', dict(path=self.args.cpu_effect, position=[0, 1])),
            ('negative_speed', 'vfx.control', dict(handle=cpu, action='speed', value=-1)),
            ('dynamic_index', 'vfx.control', dict(handle=cpu, action='dynamicInput', index=4, value=1)),
            ('invalid_color', 'vfx.control', dict(handle=cpu, action='color', value=[2,0,0,1])),
            ('unknown_action', 'vfx.control', dict(handle=cpu, action='not_an_action'))]
        for name, method, params in invalid:
            self.expect_rejected('rejection.'+name, method, params)
        self.record('rejection_existing_handles_survive',
            'PASS' if self.status(cpu).get('playing') and self.status(gpu).get('playing') else 'FAIL')
        for kind, path, handle in [('cpu',self.args.cpu_effect,cpu),('gpu',self.args.gpu_effect,gpu)]:
            result = self.call('vfx.reload', path=path)
            old = self.status(handle)
            new = self.play(path)
            self.record(kind+'.reload_success', 'PASS' if result.get('accepted') and old.get('playing') and new != handle else 'FAIL',
                receipt=result, oldHandle=old, newHandle=new, scope='Unchanged package rebuilt in cache; not content-change visual verification')
            self.stop(new)
        missing = 'Assets/Effects/__lifecycle_missing_'+uuid.uuid4().hex+'.efkpkg'
        self.expect_rejected('reload_missing_path', 'vfx.reload', dict(path=missing))
        self.record('reload_missing_preserves_other_effects',
            'PASS' if self.status(cpu).get('playing') and self.status(gpu).get('playing') else 'FAIL',
            scope='Missing unrelated path leaves active effects intact; not same-key corrupt-reload rollback coverage')
        self.stop(cpu); self.stop(gpu); self.step(30)

        samples = []
        for cycle in range(self.args.cycles):
            before=self.call('vfx.stats')
            cpu, gpu = self.play(self.args.cpu_effect), self.play(self.args.gpu_effect)
            # Stress reaches LifeTime*EmitPerFrame plateau only after 200 frames.
            self.step(max(201,self.args.sample_frames))
            live=self.call('vfx.stats')
            self.record('cycle_'+str(cycle)+'.live_work',
                'PASS' if self.status(gpu).get('playing') is True and live_cycle_valid(before,live) else 'FAIL',before=before,live=live)
            self.stop(cpu); self.stop(gpu)
            self.step(30)
            stats = self.call('vfx.stats')
            retired_clear=all(stats['gpuAllocation'][key]==0 for key in ALLOCATION_RESERVED)
            retired_clear=retired_clear and all(metric(stats,key)==0 for key in ('activeEffects','cpuInstances'))
            self.record('cycle_'+str(cycle)+'.retired_zero','PASS' if retired_clear else 'FAIL',stats=stats)
            sample = dict(cycle=cycle, before=before, live=live, stats=stats, memory=owned_memory(self.session))
            samples.append(sample)
            self.report['cycleSamples'] = samples
            write_json(self.session.output/'report.json', self.report)
        settled = samples[5:]
        for observation in resource_observations(settled):
            self.record(**observation)
        baseline_allocation = baseline['gpuAllocation']
        allocation_clear = all(all(sample['stats']['gpuAllocation'][key] == baseline_allocation[key] == 0
                                   for key in ALLOCATION_RESERVED) for sample in samples)
        self.record('cycles_allocator_returns_to_baseline', 'PASS' if allocation_clear else 'FAIL',
            baseline=baseline_allocation, cycles=len(samples), retirementFrames=30,
            scope='Exact committed CPU allocator slot ownership; not fence-retired GPU memory')
        self.record('gpu_allocator_no_failures', 'PASS', baseline=baseline_allocation,
            final=samples[-1]['stats']['gpuAllocation'],
            scope='Every sampled vfx.stats required unchanged failure counters; current system lifetime only')
        clear = all(metric(s['stats'],'activeEffects') == 0 and metric(s['stats'],'cpuInstances') == 0 and
                    metric(s['stats'],'gpuParticles') == 0 for s in settled)
        self.record('cycles_effect_reclamation', 'PASS' if clear else 'FAIL', cycles=len(samples), warmupCycles=5,
            scope='CPU-reported counts after stop; GPU particle count is an estimate')
        cached = all(metric(s['stats'],'cachedEffects') <= metric(samples[5]['stats'],'cachedEffects') and
                     metric(s['stats'],'cachedPackages') <= metric(samples[5]['stats'],'cachedPackages') for s in settled)
        self.record('cycles_cache_stability', 'PASS' if cached else 'FAIL')
        values = [s['memory']['privateBytes'] for s in settled]
        growth = statistics.median(values[-5:])-statistics.median(values[:5])
        mean_x, mean_y = (len(values)-1)/2, statistics.mean(values)
        slope = sum((i-mean_x)*(v-mean_y) for i,v in enumerate(values))/sum((i-mean_x)**2 for i in range(len(values)))
        memory_ok = no_resource_growth(values,growth,slope)
        self.record('cycles_private_memory_trend', 'PASS' if memory_ok else 'FAIL', medianWindowGrowthBytes=growth,
            slopeBytesPerCycle=slope, scope='Owned process private bytes proxy; not GPU allocation accounting')
        self.record('gpu_buffer_reclamation', 'NOT_VERIFIED',
            reason='CPU allocator slots and held storage/uniform logical pools observed separately; exact native/fence-retired bytes, textures, vertex/index, command-only uploads and frame pools remain outside coverage')


def main():
    if '--combine' in sys.argv:
        parser=argparse.ArgumentParser(description='Combine existing 20k and 50k reports without launching an engine')
        parser.add_argument('--combine',nargs=2,type=Path,required=True)
        parser.add_argument('--output',type=Path,required=True)
        args=parser.parse_args();result=combine_reports(args.combine)
        write_json(args.output,result)
        print(json.dumps(result))
        return 0 if result['commandStatus']=='PASS' else 1
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', choices=['DX12','Vulkan'], required=True)
    parser.add_argument('--cpu-effect', required=True)
    parser.add_argument('--gpu-effect', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--existing-build-sha256', required=True)
    parser.add_argument('--scene', default='Assets/Scenes/VFXAuthoringPreview.he')
    parser.add_argument('--seed', type=int, default=1806)
    parser.add_argument('--cycles', type=int, default=30)
    parser.add_argument('--sample-frames', type=int, default=15)
    parser.add_argument('--natural-end-frames', type=int, default=3600)
    parser.add_argument('--nominal-particles', type=int, choices=[20000,50000],required=True)
    args = parser.parse_args()
    if not -2147483648 <= args.seed <= 2147483647:
        parser.error('seed must fit the public int32 contract')
    budget=json.loads(BUDGET.read_text());budget_hash=digest(BUDGET)
    if args.cycles<budget['stress']['repeatPlayStopCycles']:
        parser.error('Cycle count below locked stress budget')
    if budget['stress']['postWarmupResourceGrowthAllowed'] is not False:
        parser.error('Expected locked no-growth stress budget')
    nominal_identity=stress_identity(ROOT/'Project'/args.gpu_effect,args.nominal_particles)
    if not 30 <= args.cycles <= 100 or not 1 <= args.sample_frames <= 120 or not 120 <= args.natural_end_frames <= 36000:
        parser.error('cycles 30..100, sample frames 1..120, natural end frames 120..36000 required')
    executable = ROOT/'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'
    actual_hash = digest(executable)
    if actual_hash.lower() != args.existing_build_sha256.lower():
        parser.error('Pinned executable SHA256 does not match current candidate; no session started')
    for path in [args.cpu_effect,args.gpu_effect,args.scene]:
        resolved=(ROOT/'Project'/path).resolve()
        try:
            resolved.relative_to((ROOT/'Project').resolve())
        except ValueError:
            parser.error('Inputs must stay inside Project')
        if not resolved.is_file():
            parser.error('Inputs must be existing project-relative files')
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=False)
    session=Session(output,args.backend,actual_hash,scene=args.scene)
    report=dict(schemaVersion=1,evidenceType='runtime-manual',commandStatus='RUNNING',checks=[],
        backend=args.backend,configuration='RelWithDebInfo Dev Agent (not shipping)',executableSha256=actual_hash,
        sourceHead=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        sourceStatus=subprocess.check_output(['git','status','--porcelain','-uno'],cwd=ROOT,text=True).splitlines(),
        sourceIdentityScope='Pinned candidate only; source may contain later integration changes',
        inputs={path:digest(ROOT/'Project'/path) for path in [args.cpu_effect,args.gpu_effect,args.scene]},
        command=[sys.executable,*sys.argv],budgetSha256=budget_hash,nominalIdentity=nominal_identity,
        requiredScaleReports=budget['stress']['nominalParticles'],
        budgets=dict(cycles=args.cycles,warmupCycles=5,postWarmupResourceGrowthAllowed=False),
        excludedScopes=['scene switch','multiview per-view VFX pixel proof','window resize','device recreation',
            'same-key corrupt resource rollback','changed-content reload','visual approval','shipping performance'])
    previous=os.environ.get('HE_VFX_GPU_PARTICLES'); os.environ['HE_VFX_GPU_PARTICLES']='1'
    acceptance=Acceptance(session,report,args)
    with Storage(ROOT).workflow_scope(str(ROOT/'Project'),'vfx-lifecycle',[output]) as run:
        try:
            session.start(); acceptance.run()
            report['commandStatus']='FAIL' if any(c['status']=='FAIL' for c in report['checks']) else 'PASS'
            report['acceptanceStatus']='FAIL' if report['commandStatus']=='FAIL' else 'PARTIAL'
            run['status']='success' if report['commandStatus']=='PASS' else 'failed'
        except Exception as error:
            report['commandStatus']=report['acceptanceStatus']='FAIL'; report['error']=str(error); run['status']='failed'
        finally:
            try:
                for handle in list(acceptance.handles): acceptance.stop(handle)
            except Exception as error:
                report['effectCleanupError']=str(error)
            try:
                session.stop()
            except Exception as error:
                report['cleanupError']=str(error); report['commandStatus']='FAIL'; run['status']='unknown'
            if previous is None: os.environ.pop('HE_VFX_GPU_PARTICLES',None)
            else: os.environ['HE_VFX_GPU_PARTICLES']=previous
            try:
                report['packageHashesUnchanged']=all(digest(ROOT/'Project'/path)==value for path,value in report['inputs'].items())
            except OSError as error:
                report['packageHashesUnchanged']=False; report['hashCheckError']=str(error)
            if not report['packageHashesUnchanged']:
                report['commandStatus']='FAIL'
                if run['status'] != 'unknown': run['status']='failed'
            report['budgetUnchanged']=digest(BUDGET)==budget_hash
            if not report['budgetUnchanged']:
                report['commandStatus']='FAIL'
                if run['status']!='unknown':run['status']='failed'
            if report['commandStatus']=='FAIL': report['acceptanceStatus']='FAIL'
            write_json(output/'report.json',report)
    print(json.dumps(dict(commandStatus=report['commandStatus'],acceptanceStatus=report.get('acceptanceStatus','FAIL'),report=str(output/'report.json'))))
    return 0 if report['commandStatus']=='PASS' else 1


if __name__=='__main__':
    raise SystemExit(main())
