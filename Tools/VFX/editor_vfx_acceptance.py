"""Bounded owned Editor VFX diagnostic; no strict-memory or visual PASS implied."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
ENV = {'HE_VFX_GPU_PARTICLES':'1', 'HE_VFX_STRESS_MODE':'0',
       'HE_VFX_VALIDATE_INIT_RECOVERY':'0', 'HE_VFX_DISABLE_STATS':'0', 'HE_VFX_STATS_LOG':'0'}

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2)+'\n', encoding='utf-8')

def require(value, message):
    if not value:
        raise AssertionError(message)

class Protocol:
    """Real transport is injected; CPU tests validate only client protocol logic."""
    def __init__(self, exchange, sleep=time.sleep):
        self.exchange, self.sleep = exchange, sleep
        self.requests = self.captures = self.total_frames = 0

    def call(self, method, params=None, reject=False, cleanup=False):
        require(self.requests < (256 if cleanup else 240), 'Client request reserve exhausted')
        self.requests += 1
        response = self.exchange(method, params or {})
        require(response.get('protocolVersion') == 1, 'Wrong protocol version')
        if reject:
            require(response.get('ok') is False and bool(response.get('error')), 'Negative request was not rejected')
            return response['error']
        require(response.get('ok') is True, 'Mailbox rejection: '+str(response.get('error'))[:300])
        return response['result']

    def vfx(self, action, **params):
        return self.call('editor.vfx', dict(action=action, **params))

    def end(self, cleanup=False):
        state=self.call('editor.vfx',dict(action='state'),cleanup=cleanup)
        if not state['active']:
            require(not state['cleanupFailed'], 'Inactive diagnostic cleanup failed')
            return state
        ack=self.call('editor.vfx',dict(action='end'),cleanup=cleanup)
        ticket=ack['ticket']
        require(ack['active'] and ack['retiring'] and not ack['retired'], 'Invalid retirement ACK')
        for _ in range(8):
            self.sleep(1.0)
            state=self.call('editor.vfx',dict(action='state'),cleanup=cleanup)
            require(not state['cleanupFailed'], 'Normal end cleanup failed')
            if not state['active'] and not state['retiring'] and state['retired'] and state['completedTicket']==ticket:
                return state
        raise TimeoutError('Normal end retirement deadline')

    def scene_identity(self, expected):
        result=self.call('scene.describe')
        for _ in range(8):
            if result.get('completion')!='queued': break
            self.sleep(.2)
            result=self.call('scene.result',dict(ticket=result['ticket']))
        require(result.get('ok') is True and result.get('path')==expected and bool(result.get('sceneId')),
                'Actual Editor scene differs from owned scratch scene')
        return result

    def stats(self):
        return self.vfx('stats')

    def step(self, frames):
        require(type(frames) is int and 1 <= frames <= 3600, 'Client step bounds')
        require(self.total_frames+frames <= 36000, 'Client frame budget')
        before = self.stats()['updateSerial']
        ack = self.vfx('step', frames=frames)
        ticket = ack['ticket']
        require(ticket > 0 and ack['active'], 'Invalid step ACK')
        deadline = time.monotonic()+90
        while True:
            self.sleep(1.0)
            state = self.vfx('state')
            require(state['active'] and not state['cleanupFailed'], 'Step lost diagnostic')
            if state['completedTicket'] == ticket and state['remainingFrames'] == 0:
                break
            require(time.monotonic() < deadline, 'Step ticket deadline')
        self.total_frames += frames
        require(state['totalFrames'] == self.total_frames, 'Wrong completed frame count')
        after = self.stats()
        require(after['updateSerial']-before == frames, 'UpdateSerial is not exact step count')
        return dict(ticket=ticket, frames=frames, state=state, stats=after)

    def paused(self):
        before = self.stats()['updateSerial']
        self.sleep(.3)
        state = self.vfx('state')
        after = self.stats()['updateSerial']
        require(state['active'] and state['remainingFrames'] == 0 and before == after, 'Paused presentation updated VFX')
        return dict(before=before, after=after)

    def negative(self, params):
        before = self.vfx('state')
        serial = self.stats()['updateSerial']
        error = self.call('editor.vfx', params, reject=True)
        require(self.vfx('state') == before and self.stats()['updateSerial'] == serial, 'Rejected request mutated diagnostic')
        return dict(params=params, error=error)

    def capture(self, name, mailbox):
        require(self.captures < 32, 'Client capture budget')
        self.captures += 1
        ack = self.call('editor.capture', dict(name=name, settleFrames=2))
        ticket = ack['ticket']
        for _ in range(12):
            self.sleep(.2)
            receipt = self.call('editor.capture.result', dict(ticket=ticket))
            require(receipt['ticket'] == ticket, 'Capture ticket mismatch')
            if receipt['state'] == 'complete':
                path = Path(receipt['path']).resolve()
                require(path.parent == (mailbox/'captures').resolve(), 'Capture outside owned mailbox')
                require(receipt['target'] == 'presented-backbuffer' and receipt.get('submission',0)>0, 'Missing presented submission')
                require(path.is_file(), 'Missing original PNG')
                return dict(receipt=receipt, sha256=sha(path), path=str(path))
            require(receipt['state'] in ('queued','inFlight'), 'Capture failed')
        raise TimeoutError('Capture completion deadline')

def retired(stats):
    for key in ('activeEffects','cpuInstances'):
        require(stats[key]['status'] == 1 and stats[key]['current'] == 0, key+' not retired')
    for key in ('allocatedEmitters','reservedParticles','reservedTrails','emitterSlotFailures','particleBlockFailures','trailBlockFailures'):
        require(stats['gpuAllocation'][key] == 0, key+' not zero')

def exercise(client, mailbox, report, cpu, gpu):
    # Before-begin rejection tested without assuming realtime UpdateSerial is frozen.
    state = client.vfx('state')
    client.call('editor.vfx', dict(action='step',frames=1), reject=True)
    require(client.vfx('state') == state, 'Pre-begin rejection mutated state')
    require(client.vfx('begin')['active'], 'Begin failed')
    origin_serial=client.stats()['updateSerial']
    report['diagnosticStartUpdateSerial']=origin_serial
    description = client.vfx('describe')
    report['description'] = description
    require(description['gpu']['enabled'] is True and description['gpu']['validated'] is False,
            'Expected enabled diagnostic probe and unvalidated production capability')
    negatives = [dict(action='step',frames=True), dict(action='status',handle=2147483647),
                 dict(action='play',path='../escape.efkpkg',seed=1806)]
    report['negativeTests'] = [client.negative(p) for p in negatives]
    report['captures'].append(client.capture('empty', mailbox))
    report['pausedInitial'] = client.paused()
    for label, path, frames in [('cpu',cpu,[1,30,90,180]), ('gpu20k',gpu,[1,60,201,600])]:
        case = dict(kind=label, package=path, stages=[])
        report['cases'].append(case)
        case['before']=client.stats()
        play = client.vfx('play',path=path,seed=1806)
        require(play.get('playing') is True, 'Play not alive')
        handle = play['handle']; case['play'] = play; current = 0
        for frame in frames:
            stage = client.step(frame-current); current = frame
            stage['frame'] = frame
            stage['capture'] = client.capture(label+'-'+str(frame), mailbox)
            case['stages'].append(stage)
        if label == 'gpu20k':
            require(any(x['stats']['totalRecordedDispatchCount'] > case['stages'][0]['stats']['totalRecordedDispatchCount'] for x in case['stages'][1:]), 'GPU dispatch evidence absent')
        require(any(x['stats']['totalRecordedDrawCount']>case['before']['totalRecordedDrawCount'] for x in case['stages']), 'VFX draw recording absent')
        case['paused'] = client.paused()
        natural_frame = 240 if label == 'cpu' else 1600
        case['naturalStep'] = client.step(natural_frame-current)
        case['naturalStatus'] = client.vfx('status',handle=handle)
        require(case['naturalStatus']['playing'] is False, 'Natural end not reached within fixed diagnostic window')
        case['naturalCapture'] = client.capture(label+'-natural', mailbox)
        client.vfx('stop',handle=handle)  # Release ownership even if naturally ended.
        replay = client.vfx('play',path=path,seed=1806)
        require(replay['playing'] is True, 'Replay failed')
        case['replay'] = replay
        case['replayStep'] = client.step(30)
        case['stop'] = client.vfx('stop',handle=replay['handle'])
        require(case['stop']['playing'] is False, 'Stop failed')
        case['retired'] = client.step(30)['stats']; retired(case['retired'])
        case['retiredCapture'] = client.capture(label+'-retired', mailbox)
    report['finalPaused'] = client.paused()
    require(report['finalPaused']['after']-origin_serial==client.total_frames,
            'Unrequested updates occurred during captures/polling')
    end = client.end()
    require(end.get('retired') is True, 'End did not complete retirement')
    report['end'] = end
    report['endStats']=client.stats()
    retired(report['endStats'])
    report['captures'].append(client.capture('end-empty',mailbox))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend',choices=['DX12','Vulkan'],required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--expected-editor-sha256',required=True)
    parser.add_argument('--cpu',default='Assets/Effects/Authoring/hit-arcane.efkpkg')
    parser.add_argument('--gpu',default='Assets/Effects/Stress/GpuSparks20k.efkpkg')
    args = parser.parse_args()
    sys.path.insert(0,str(ROOT/'Tools/VFX'))
    from editor_prefab_acceptance import atomic, collect_runtime_logs
    from preview import Storage
    executable = ROOT/'out/agent/editor-relwithdebinfo/bin/HEngineEditor.exe'
    require(sha(executable).lower() == args.expected_editor_sha256.lower(), 'Pinned Editor mismatch')
    output = args.output.resolve()
    allowed=(ROOT/'out/agent/rendering-evidence/vfx-authoring').resolve()
    require(output!=allowed and output.is_relative_to(allowed) and not output.exists(),
            'Use a new rendering-evidence/vfx-authoring child directory')
    output.mkdir(parents=True)
    token = uuid.uuid4().hex; mailbox = output/'mailbox'
    for part in ('requests','responses'): (mailbox/part).mkdir(parents=True)
    scratch = ROOT/'Project/Assets/Effects/Authoring/EditorAcceptance'/token
    scratch.mkdir(parents=True)
    # Reuse the canonical preview camera/exposure; no script or effect autoplays.
    # A bare {} scene inherits dark default exposure and cannot prove VFX visibility.
    preview_source = ROOT/'Project/Assets/Scenes/VFXAuthoringPreview.he'
    preview_document = json.loads(preview_source.read_text(encoding='utf-8'))
    scene_document = {key:value for key,value in preview_document.items()
                      if 'CameraComponent' in value or 'EnvironmentLightComponent' in value}
    require(len(scene_document)==2 and all('PythonScriptComponent' not in value for value in scene_document.values()),
            'Canonical preview must provide exactly a camera and exposure entity')
    scene = scratch/'Empty.he'; save(scene,scene_document)
    files = [Path(__file__),executable,scene,preview_source,ROOT/'Tools/VFX/editor_prefab_acceptance.py',
             ROOT/'Tools/VFX/preview.py',ROOT/'Tools/Agent/storage.py',ROOT/'Tools/Agent/hectl.ps1',
             ROOT/'Tools/VFX/acceptance-budget.json']
    for value in (args.cpu,args.gpu):
        p = (ROOT/'Project'/value).resolve()
        require(p.is_relative_to(ROOT/'Project/Assets') and p.suffix=='.efkpkg', 'Package outside Assets')
        files.append(p)
    inputs = {str(p):sha(p) for p in files}
    report = dict(commandStatus='RUNNING',evidenceType='runtime-manual',host='Editor',backend=args.backend,
                  inputHashes=inputs,captures=[],cases=[],visualStatus='NOT_REVIEWED',strictMemoryStatus='NOT_VERIFIED',
                  scope='Functional/presented capture diagnostic, not 30-cycle strict lifecycle gate. OS/private not judged PASS.',
                  notCovered=['36000 total-frame exhaustion','16-handle exhaustion','pending duplicate step race','request replay',
                              'extended invalid-key/action/seed matrix',
                              'GPU disabled/default policy','native abort/crash recovery','end cancellation of pending ticket',
                              'GPU fence/native-byte reclamation','strict OS/private budgets','visual approval'])
    command=['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(ROOT/'Tools/Agent/hectl.ps1'),
             'editor','check','-Configuration','RelWithDebInfo','-Parallel','2','-Frames','36000','-TimeoutSeconds','600',
             '-StartupScene',scene.relative_to(ROOT/'Project').as_posix(),'-GraphicsApi',args.backend,'-StrictGraphicsApi','-RenderValidation','-RenderStats']
    report['command'] = command
    saved_env = {k:os.environ.get(k) for k in ENV}; process = client = None
    with Storage(ROOT).workflow_scope(str(ROOT/'Project'),'editor-vfx-diagnostic',[output]) as run:
        environment=os.environ.copy(); environment.update(ENV)
        environment.update(HE_EDITOR_SCENE_MAILBOX=str(mailbox),HE_EDITOR_SCENE_TOKEN=token,HE_EDITOR_SCENE_ROOT=str(scratch))
        with (output/'editor-check.stdout.log').open('w',encoding='utf-8') as stdout, (output/'editor-check.stderr.log').open('w',encoding='utf-8') as stderr:
            try:
                process=subprocess.Popen(command,cwd=ROOT,env=environment,stdout=stdout,stderr=stderr,creationflags=subprocess.CREATE_NO_WINDOW)
                deadline=time.monotonic()+900
                while not (mailbox/'ready.json').exists():
                    require(process.poll() is None, 'Editor ended before ready')
                    require(time.monotonic()<deadline, 'Editor ready deadline')
                    time.sleep(.1)
                require(sha(executable).lower()==args.expected_editor_sha256.lower(),'Check rebuilt different Editor')
                ready=json.loads((mailbox/'ready.json').read_text(encoding='utf-8-sig'))
                require(ready.get('maxRequests')==256,'Unexpected mailbox request budget')
                report['ready']={k:ready.get(k) for k in ('protocolVersion','startupScene','maxRequests','maxCaptures')}
                def exchange(method, params):
                    identifier=uuid.uuid4().hex
                    atomic(mailbox/'requests'/(identifier+'.json'),dict(protocolVersion=1,requestId=identifier,token=token,method=method,params=params))
                    destination=mailbox/'responses'/(identifier+'.json'); deadline=time.monotonic()+20
                    while not destination.exists():
                        require(process.poll() is None,'Editor exited while awaiting '+method)
                        require(time.monotonic()<deadline,'Mailbox deadline '+method)
                        time.sleep(.05)
                    response=json.loads(destination.read_text(encoding='utf-8-sig'))
                    save(output/(str(client.requests).zfill(3)+'-'+method+'.json'),dict(method=method,params=params,response=response))
                    require(response.get('requestId')==identifier,'Response request identity mismatch')
                    return response
                client=Protocol(exchange)
                report['sceneIdentity']=client.scene_identity(scene.relative_to(ROOT/'Project').as_posix())
                exercise(client,mailbox,report,args.cpu,args.gpu)
                report['commandStatus']='PASS'
            except Exception as error:
                report.update(commandStatus='FAIL',error=str(error)[:1500])
            finally:
                if process and process.poll() is None and client:
                    try:
                        report['finallyEnd']=client.end(cleanup=True)
                    except Exception as error: report['endCleanupNote']=str(error)[:500]
                    try:
                        stop=client.call('session.stop',cleanup=True)
                        report['stopAcknowledged']=stop.get('stopping') is True
                    except Exception as error: report['stopError']=str(error)[:500]
                if process:
                    try: process.wait(timeout=660)
                    except subprocess.TimeoutExpired:
                        report.update(commandStatus='FAIL',cleanupError='Owned check still running; retained for recovery',cleanupProcessId=process.pid)
                    report['exitCode']=process.returncode
                report['sessionCleanup']='PASS' if process and process.returncode==0 and report.get('stopAcknowledged') else 'FAIL'
                if report['sessionCleanup']!='PASS': report['commandStatus']='FAIL'
                report['environmentRestored']=all(os.environ.get(k)==v for k,v in saved_env.items())
                report['environmentScope']='Child environment overrides only; parent values never modified'
                try:
                    report['inputsAfter']={p:sha(p) for p in inputs}
                    require(report['inputsAfter']==inputs,'Frozen input hash changed')
                    capture_records=list(report['captures'])
                    for case in report['cases']:
                        capture_records.extend(s['capture'] for s in case['stages'] if 'capture' in s)
                        capture_records.extend(case[k] for k in ('naturalCapture','retiredCapture') if k in case)
                    require(all(sha(c['path'])==c['sha256'] for c in capture_records),'Original capture hash changed')
                    report['captureHashesUnchanged']=True
                except Exception as error: report.update(commandStatus='FAIL',inputClosureError=str(error)[:1000])
                try:
                    stdout.flush(); stderr.flush()
                    if process and process.returncode is not None: collect_runtime_logs(output,report)
                    else: raise RuntimeError('Native stream closure unknown while owned check runs')
                    for stream in report['runtimeLogs'].values():
                        text=Path(stream['path']).read_text(encoding='utf-8-sig',errors='replace')
                        require(not re.search(r'\[(error|fatal)\]|diagnostic cleanup failed|unhandled exception',text,re.I),'Native error/fatal found')
                except Exception as error: report.update(commandStatus='FAIL',nativeClosureError=str(error)[:1000])
                if not report['environmentRestored']: report['commandStatus']='FAIL'
                report['requests']=client.requests if client else 0
                report['captureCount']=client.captures if client else 0
                report['totalStepFrames']=client.total_frames if client else 0
                if process and process.returncode is not None:
                    for pending in list((mailbox/'requests').glob('*.json'))+list((mailbox/'requests').glob('*.claimed')):
                        value=json.loads(pending.read_text(encoding='utf-8-sig')); value.pop('token',None); atomic(pending,value)
                save(output/'report.json',report)
                run['status']='unknown' if report.get('cleanupError') else 'success' if report['commandStatus']=='PASS' else 'failed'
    print(json.dumps(dict(status=report['commandStatus'],report=str(output/'report.json'))))
    return 0 if report['commandStatus']=='PASS' else 1

if __name__=='__main__':
    raise SystemExit(main())
