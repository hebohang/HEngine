"""Pinned GPU trail pause/speed-zero image acceptance; never builds an engine."""
import argparse
import json
import os
from pathlib import Path
import shutil

import numpy as np
from PIL import Image

from preview import ROOT, Session, Storage, digest, write_json

EFFECT = 'Assets/Effects/Stress/GpuTrailPause16.efkpkg'
FAILURES = ('emitterSlotFailures', 'particleBlockFailures', 'trailBlockFailures')
RESERVED = ('allocatedEmitters', 'reservedParticles', 'reservedTrails')
THRESHOLDS = dict(signal=12, minSignalPixels=20, stableMae=1.5,
                  stableChangedFraction=.01, stableMaskIoU=.97,
                  hiddenSignalFraction=.03, resumeMaskIoU=.25)


def pixels(path):
    with Image.open(path) as image:
        return np.asarray(image.convert('RGB'), dtype=np.int16)


def measure(a, b, background):
    signal_a = np.max(np.abs(a-background), axis=2) > THRESHOLDS['signal']
    signal_b = np.max(np.abs(b-background), axis=2) > THRESHOLDS['signal']
    support = signal_a | signal_b
    delta = np.max(np.abs(a-b), axis=2)
    union = int(support.sum())
    return dict(signalA=int(signal_a.sum()), signalB=int(signal_b.sum()),
                mae=float(np.abs(a-b)[support].mean()) if union else 0,
                changedFraction=float((delta[support] > THRESHOLDS['signal']).mean()) if union else 0,
                maskIoU=float((signal_a & signal_b).sum()/union) if union else 1)


def stable(metrics):
    return (metrics['signalA'] >= THRESHOLDS['minSignalPixels'] and
            metrics['mae'] <= THRESHOLDS['stableMae'] and
            metrics['changedFraction'] <= THRESHOLDS['stableChangedFraction'] and
            metrics['maskIoU'] >= THRESHOLDS['stableMaskIoU'])


class Probe:
    def __init__(self, session, report):
        self.session, self.report, self.handles = session, report, set()
        self.baseline = None

    def check(self, name, condition, **evidence):
        self.report['checks'].append(dict(name=name, status='PASS' if condition else 'FAIL', **evidence))
        write_json(self.session.output/'report.json', self.report)

    def call(self, method, **params):
        return self.session.call(method, params)

    def step(self, frames):
        self.call('runtime.step', frames=frames)

    def control(self, handle, action, **params):
        response = self.call('vfx.control', handle=handle, action=action, **params)
        if action != 'status' and response.get('accepted') is not True:
            raise RuntimeError('Control rejected: '+str(response))
        return response

    def stats(self, name, emitters=None):
        result = self.call('vfx.stats')
        write_json(self.session.output/(name+'-stats.json'), result)
        allocation = result.get('gpuAllocation', {})
        if any(type(allocation.get(k)) is not int or allocation[k] < 0 for k in FAILURES+RESERVED):
            raise RuntimeError('Missing exact allocator telemetry')
        if any(allocation[k] != 0 for k in FAILURES):
            self.check(name+'.allocation_failures', False, allocation=allocation)
            raise RuntimeError('GPU allocation failure; commands cannot pass')
        if emitters is not None:
            expected = dict(zip(RESERVED, (emitters, emitters*256, emitters*4096)))
            self.check(name+'.allocator', all(allocation[k] == self.baseline[k]+v for k,v in expected.items()),
                       allocation=allocation, baseline=self.baseline, expectedDelta=expected)
        return result

    def capture(self, name):
        receipt = self.call('capture.frame', filename=name+'.png')
        path = self.session.output/(name+'.png')
        shutil.copyfile(receipt['path'], path)
        self.report['captures'].append(dict(name=name, path=str(path), sha256=digest(path), receipt=receipt))
        self.stats(name)
        return pixels(path)

    def play(self, x):
        result = self.call('vfx.play', path=EFFECT, seed=1806, position=[x,0,0])
        handle = result['handle']; self.handles.add(handle)
        if result.get('playing') is not True:
            raise RuntimeError('GPU fixture did not start')
        return handle

    def stop_all(self):
        errors = []
        for handle in list(self.handles):
            try:
                self.control(handle, 'stop'); self.handles.discard(handle)
            except Exception as error:
                errors.append(str(error))
        self.step(30)
        final = self.stats('retired-'+str(len(self.report['checks'])), 0)
        if errors:
            raise RuntimeError('; '.join(errors))
        return final

    def scenario(self, mode):
        background = self.capture(mode+'-empty')
        target = self.play(-1.2)
        self.step(32)
        self.stats(mode+'-formed', 1)
        formed = self.capture(mode+'-formed')
        if mode == 'pause':
            self.control(target, 'pause')
        else:
            self.control(target, 'parameter', name='speed', value=0)
        # The first zero-time sample is explicit: do not hide a one-frame shift
        # by comparing only two later paused frames.
        self.step(1)
        zero = self.capture(mode+'-first-zero')
        mask = np.max(np.abs(formed-background), axis=2) > THRESHOLDS['signal']
        ys, xs = np.nonzero(mask)
        if len(xs) < THRESHOLDS['minSignalPixels']:
            raise RuntimeError('No visible isolated target GPU trail')
        x0,x1=max(0,int(xs.min())-12),min(mask.shape[1],int(xs.max())+13)
        y0,y1=max(0,int(ys.min())-12),min(mask.shape[0],int(ys.max())+13)
        if x1-x0 > mask.shape[1]//2:
            raise RuntimeError('Target ROI spans half the image; neutral/isolation assumption invalid')
        roi = lambda image: image[y0:y1,x0:x1]
        self.report.setdefault('rois', {})[mode] = [x0,y0,x1,y1]
        bg = roi(background)
        first = measure(roi(formed),roi(zero),bg)
        self.check(mode+'.first_zero_stable', stable(first), metrics=first)
        other = self.play(1.2)
        self.step(8)
        start = self.capture(mode+'-other-active-baseline')
        before = self.stats(mode+'-before32', 2)
        self.step(32)
        end = self.capture(mode+'-after32')
        after = self.stats(mode+'-after32', 2)
        state = self.control(target,'status'); other_state = self.control(other,'status')
        frozen_state = state.get('paused') is True if mode=='pause' else state.get('speed') == 0
        self.check(mode+'.sdk_state', frozen_state and state.get('playing') is True and
                   other_state.get('playing') is True and other_state.get('paused') is False and
                   other_state.get('speed') == 1, target=state, other=other_state)
        for label,a,b in [('mixed_baseline',zero,start),('32_zero_steps',start,end)]:
            metrics=measure(roi(a),roi(b),bg)
            self.check(mode+'.'+label,stable(metrics),metrics=metrics)
        timing=after.get('gpuSimulationMs',{})
        self.check(mode+'.native_simulation',
                   after.get('totalRecordedDispatchCount',0)>before.get('totalRecordedDispatchCount',0) and
                   after.get('lastGpuSimulationSubmission',0)>before.get('lastGpuSimulationSubmission',0) and
                   timing.get('samples',0)>0 and timing.get('current',0)>0, before=before,after=after)
        self.control(target,'parameter',name='intensity',value=0); self.step(1)
        hidden=self.capture(mode+'-hidden')
        hidden_signal=int((np.max(np.abs(roi(hidden)-bg),axis=2)>THRESHOLDS['signal']).sum())
        visible_signal=max(1,int((np.max(np.abs(roi(end)-bg),axis=2)>THRESHOLDS['signal']).sum()))
        self.check(mode+'.hidden_and_isolated', hidden_signal/visible_signal<=THRESHOLDS['hiddenSignalFraction'],
                   residualPixels=hidden_signal,visiblePixels=visible_signal,
                   scope='Also rejects active second emitter/background contamination inside target ROI')
        self.control(target,'parameter',name='intensity',value=1); self.step(1)
        restored=self.capture(mode+'-restored')
        metrics=measure(roi(end),roi(restored),bg)
        self.check(mode+'.restore_same_geometry',stable(metrics),metrics=metrics)
        if mode=='pause': self.control(target,'resume')
        else: self.control(target,'parameter',name='speed',value=1)
        self.step(1)
        resumed=self.capture(mode+'-resume1')
        metrics=measure(roi(restored),roi(resumed),bg)
        self.check(mode+'.resume_continuity',metrics['signalB']>=THRESHOLDS['minSignalPixels'] and
                   metrics['maskIoU']>=THRESHOLDS['resumeMaskIoU'] and metrics['mae']>0,
                   metrics=metrics,scope='One-frame visible motion with retained overlapping trail; no particle-state hash')
        self.stop_all()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend',choices=['DX12','Vulkan'],required=True)
    parser.add_argument('--existing-build-sha256',required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    executable=ROOT/'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'
    pinned=digest(executable)
    if pinned.lower()!=args.existing_build_sha256.lower(): parser.error('Candidate hash mismatch')
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=False)
    inputs={p:digest(ROOT/'Project'/p) for p in [EFFECT,'Assets/Scenes/VFXAuthoringPreview.he']}
    report=dict(status='RUNNING',evidenceType='runtime-manual',backend=args.backend,executableSha256=pinned,
                inputs=inputs,thresholds=THRESHOLDS,checks=[],captures=[],
                scope='Fixed-camera target ROI image acceptance; not particle-state hashes, overdraw or performance',
                excluded=['GPU particle state readback','device recreation','global overdraw','performance benchmark'])
    session=Session(output,args.backend,pinned); probe=Probe(session,report)
    old={key:os.environ.get(key) for key in ['HE_VFX_GPU_PARTICLES','HE_VFX_DISABLE_STATS']}
    os.environ['HE_VFX_GPU_PARTICLES']='1'; os.environ['HE_VFX_DISABLE_STATS']='0'
    with Storage(ROOT).workflow_scope(str(ROOT/'Project'),'vfx-trail-pause',[output]) as run:
        try:
            try:
                session.start(); probe.call('runtime.pause'); probe.step(30)
                baseline=probe.stats('baseline'); probe.baseline=baseline['gpuAllocation']
                if baseline.get('gpuEnabled') is not True or any(probe.baseline[k] for k in RESERVED):
                    raise RuntimeError('Neutral scene must have enabled GPU system and empty allocator')
                report['session']=probe.call('session.status'); report['baseline']=baseline
                probe.scenario('pause'); probe.scenario('speed0')
            finally:
                try:
                    if probe.baseline is not None: report['finalStats']=probe.stop_all()
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
            for key,value in old.items():
                if value is None: os.environ.pop(key,None)
                else: os.environ[key]=value
            report['inputsUnchanged']=all(digest(ROOT/'Project'/p)==value for p,value in inputs.items())
            report['binaryUnchanged']=digest(executable)==pinned
            if not report['inputsUnchanged'] or not report['binaryUnchanged']:
                report['status']='FAIL'; run['status']='failed'
            write_json(output/'report.json',report)
    print(json.dumps(dict(status=report['status'],report=str(output/'report.json'))))
    return 0 if report['status']=='PASS' else 1


if __name__=='__main__':
    raise SystemExit(main())
