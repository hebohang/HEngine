"""Bounded actual Editor animation preview UI states, not mouse usability evidence."""
from __future__ import annotations
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import time
import uuid

from editor_prefab_acceptance import Mailbox, collect_runtime_logs, prepare
from preview import ROOT, Storage, digest, write_json


def graph_fit_result(state):
    if state.get('nodeCount',0)<=0 or state.get('lastUiFrame',-1)<state.get('applicationFrame',0)-3:
        raise AssertionError('Controller canvas state is missing or stale')
    values=[state[key] for key in ('visibleMin','visibleMax','boundsMin','boundsMax')]
    if any(len(pair)!=2 or any(not math.isfinite(v) for v in pair) for pair in values):
        raise AssertionError('Controller canvas bounds must be finite pairs')
    low,high,node_low,node_high=values
    fits=all(low[i]<=node_low[i]<=node_high[i]<=high[i] for i in range(2))
    if state.get('visibilityToleranceCanvasUnits')!=0 or state.get('nodesFullyVisible') is not fits:
        raise AssertionError('Controller visibility receipt disagrees with exact bounds')
    return dict(status='PASS' if fits else 'FAIL',scope='All node rectangles within rendered canvas; zero tolerance, visual legibility still requires screenshot review',state=state)


def require_controller_fits(report):
    failed = [key for key in ('controllerInitialFit','controllerExplicitFocusFit')
              if report.get(key, {}).get('status') != 'PASS']
    if failed:
        raise AssertionError('Controller canvas fit failed: '+', '.join(failed))


def dependency_inputs(seeds, root=ROOT):
    """Follow only explicit asset references; no Project directory scan."""
    project=(root/'Project').resolve(); content=(root/'Engine/Content').resolve()
    pending=[Path(p).resolve() for p in seeds]; seen=set()
    structured={'.hanim','.hanimctrl','.hmesh','.hmat','.hskel','.he','.json'}
    while pending:
        path=pending.pop()
        if path in seen: continue
        if len(seen)>=256: raise ValueError('Preview dependency graph exceeds 256 files')
        if not path.is_file(): raise ValueError('Missing preview dependency: '+str(path))
        seen.add(path)
        if path.suffix.lower() not in structured: continue
        if path.stat().st_size>16*1024*1024: raise ValueError('Preview JSON dependency exceeds 16 MiB')
        def visit(value,key=''):
            if isinstance(value,dict):
                for child_key,child in value.items(): visit(child,child_key)
            elif isinstance(value,list):
                for child in value: visit(child,key)
            elif isinstance(value,str):
                name=value.replace('\\','/')
                candidate=None
                if name.startswith(('Assets/','Shaders/')):
                    options=[project/name,content/name]
                    candidate=next((p for p in options if p.is_file()),options[0])
                elif key=='uri' and name and not name.startswith('data:'):
                    candidate=path.parent/name
                if candidate is not None:
                    candidate=candidate.resolve()
                    if not any(candidate.is_relative_to(base) for base in (project,content)):
                        raise ValueError('Preview dependency escapes content roots')
                    pending.append(candidate)
        visit(json.loads(path.read_text(encoding='utf-8-sig')))
    return sorted(seen)


def require_ready(state):
    if not state.get('ready') or state.get('loadError'):
        raise AssertionError('Actual animation preview/mesh is not ready')
    if state['applicationFrame']-state['lastUiFrame'] > 2:
        raise AssertionError('Animation preview UI stopped rendering')
    for key in ('progress','seconds','durationSeconds','fixedDeltaSeconds'):
        if not isinstance(state.get(key),(int,float)) or not math.isfinite(state[key]):
            raise AssertionError('Invalid preview numeric state: '+key)
    if state['durationSeconds'] <= 0:
        raise AssertionError('Animation duration is empty')


def verify_advance(before, after, frames, speed):
    require_ready(before); require_ready(after)
    if after['playing'] or after['framesRemaining'] != 0:
        raise AssertionError('Bounded preview play did not pause at its frame boundary')
    duration=before['durationSeconds']
    expected=(before['seconds']+frames*before['fixedDeltaSeconds']*speed)%duration
    if abs(after['seconds']-expected)>1e-4:
        raise AssertionError(f'Preview frame/speed progression differs: {after["seconds"]} vs {expected}')
    if after['poseUpdates']-before['poseUpdates'] < frames:
        raise AssertionError('Missing actual preview pose updates')


def exercise(client, output, asset, report):
    def state(): return client.call('editor.preview',dict(path=asset,action='state'))
    def wait_ready():
        deadline=time.monotonic()+25
        while time.monotonic()<deadline:
            value=state()
            if value.get('loadError'): raise RuntimeError(value['loadError'])
            if value.get('ready'):
                require_ready(value); return value
            time.sleep(.2)
        raise TimeoutError('Animation preview mesh/pose did not become ready')
    def capture(name):
        started=client.call('editor.capture',dict(name=name,settleFrames=3))
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            result=client.call('editor.capture.result',dict(ticket=started['ticket']))
            if result['state']=='failed': raise RuntimeError('Editor capture failed: '+result.get('error',''))
            if result['state']=='complete':
                if result['target']!='presented-backbuffer' or result['submission']<=0 or result['renderFrame']<result['dueFrame']:
                    raise AssertionError('Capture lacks completed presented-frame identity')
                path=Path(result['path']).resolve()
                if not path.is_relative_to((output/'mailbox/captures').resolve()) or not path.is_file():
                    raise AssertionError('Capture did not publish its owned PNG')
                result['sha256']=digest(path)
                report['captures'].append(result)
                return result
            time.sleep(.2)
        raise TimeoutError('Editor capture completion barrier timed out')
    client.call('editor.preview',dict(path=asset,action='open'))
    wait_ready()
    for progress,name in ((.15,'pose-015'),(.65,'pose-065')):
        client.call('editor.preview',dict(path=asset,action='scrub',progress=progress))
        capture(name)
        current=state(); require_ready(current)
        if current['playing'] or abs(current['progress']-progress)>1e-5:
            raise AssertionError('Scrubbed UI pose was not retained')
        report['states'][name]=current
    report['checks'].append('actual animation preview renders and retains two requested scrub positions')
    for speed,label in ((.5,'speed-050'),(2.,'speed-200')):
        client.call('editor.preview',dict(path=asset,action='scrub',progress=.15))
        before=state(); require_ready(before)
        client.call('editor.preview',dict(path=asset,action='play',speed=speed,frames=12))
        deadline=time.monotonic()+20
        while True:
            after=state()
            if after['framesRemaining']==0: break
            if time.monotonic()>deadline: raise TimeoutError('Bounded preview frames stalled')
            time.sleep(.2)
        verify_advance(before,after,12,speed)
        capture(label)
        report['states'][label]=dict(before=before,after=after)
    report['checks'].append('12 actual preview frames advance animation at .5x and 2x fixed-delta rates then pause')
    paused=client.call('editor.preview',dict(path=asset,action='pause'))
    capture('paused-a'); capture('paused-b')
    held=state(); require_ready(held)
    if held['playing'] or abs(held['seconds']-paused['seconds'])>1e-6 or held['poseUpdates']<=paused['poseUpdates']:
        raise AssertionError('Pause failed to hold pose across subsequent rendered UI frames')
    report['checks'].append('pause preserves timeline over completed presented UI captures')
    # Read-only negative controls must not mutate the preview or write outside its capture jail.
    for method,params in [('editor.preview',dict(path='../escape.hanim',action='open')),
                          ('editor.preview',dict(path=asset,action='play',speed=1,frames=601)),
                          ('editor.capture',dict(name='../escape',settleFrames=3))]:
        try: client.call(method,params)
        except RuntimeError as error:
            if 'Editor transport rejected' not in str(error): raise
        else: raise AssertionError('Invalid Editor automation request was accepted')
    final=state()
    if final['playing'] or abs(final['seconds']-held['seconds'])>1e-6:
        raise AssertionError('Rejected command mutated animation state')
    report['checks'].append('path traversal, capture traversal and excessive play frames rejected without state mutation')
    controller='Assets/Effects/Authoring/CharacterDemo/Character.hanimctrl'
    client.call('editor.preview',dict(path=controller,action='open'))
    capture('controller-ui')
    initial_graph=client.call('editor.preview',dict(path=controller,action='state'))
    report['controllerInitialFit']=graph_fit_result(initial_graph)
    client.call('editor.preview',dict(path=controller,action='focus'))
    capture('controller-focused')
    focused_graph=client.call('editor.preview',dict(path=controller,action='state'))
    report['controllerExplicitFocusFit']=graph_fit_result(focused_graph)
    if focused_graph.get('focusExecutions',0)<=initial_graph.get('focusExecutions',0):
        raise AssertionError('Explicit controller focus did not execute')
    require_controller_fits(report)
    report['checks'].append('real controller panel opened and presented; graph appearance awaits visual review')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend',choices=['Vulkan','DX12'],required=True)
    parser.add_argument('--expected-editor-sha256',required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    output=args.output.resolve(); allowed=(ROOT/'out/agent/rendering-evidence/vfx-authoring').resolve()
    if not output.is_relative_to(allowed) or output==allowed: parser.error('output must be a new vfx-authoring evidence child')
    executable=ROOT/'out/agent/editor-relwithdebinfo/bin/HEngineEditor.exe'
    if digest(executable).lower()!=args.expected_editor_sha256.lower(): parser.error('Wrong reviewed Editor binary')
    asset='Assets/Effects/Authoring/CharacterDemo/Punch.hanim'
    inputs=dependency_inputs([ROOT/'Project'/asset,
            ROOT/'Project/Assets/Effects/Authoring/CharacterDemo/Character.hanimctrl',
            ROOT/'Project/Assets/Effects/Authoring/CharacterDemo/CharacterAcceptance.he'])
    inputs += [Path(__file__).resolve(),ROOT/'Tools/VFX/editor_prefab_acceptance.py']
    inputs_before={str(p.relative_to(ROOT)):digest(p) for p in inputs}
    output.mkdir(parents=True,exist_ok=False)
    token=uuid.uuid4().hex; mailbox=output/'mailbox'
    for name in ('requests','responses'): (mailbox/name).mkdir(parents=True)
    scratch=ROOT/'Project/Assets/Effects/Authoring/EditorAcceptance'/token
    scratch.mkdir(parents=True); prefix=prepare(scratch)
    report=dict(status='RUNNING',host='Editor',backend=args.backend,evidenceType='runtime-manual',
        visualStatus='NOT_REVIEWED',scope='Actual Editor preview and controller UI rendered states; not mouse usability, game runtime, animation event or controller-transition proof',
        checks=[],captures=[],states={},inputsBefore=inputs_before,expectedEditorSha256=args.expected_editor_sha256,
        inputHashScope='bounded explicit asset dependency closure of animation, controller and source scene, including meshes, buffers, skeletons, materials and textures; built-in defaults remain identified by pinned binary')
    command=['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(ROOT/'Tools/Agent/hectl.ps1'),
        'editor','check','-Configuration','RelWithDebInfo','-Parallel','2','-Frames','18000','-TimeoutSeconds','240',
        '-FixedDeltaSeconds','0.0166666666666667','-Resolution','1920x1080','-StartupScene',prefix+'EditorAcceptance.he',
        '-GraphicsApi',args.backend,'-StrictGraphicsApi','-RenderValidation']
    report['command']=command
    process=None; client=None
    with Storage(ROOT).workflow_scope(str(ROOT/'Project'),'vfx-editor-animation',[output]) as storage:
        environment=os.environ.copy()
        environment.update(HE_EDITOR_SCENE_MAILBOX=str(mailbox),HE_EDITOR_SCENE_TOKEN=token,HE_EDITOR_SCENE_ROOT=str(scratch),
                           HE_EDITOR_CONTENT_DIRECTORY='Assets/Effects/Authoring/CharacterDemo')
        with (output/'editor-check.stdout.log').open('w',encoding='utf-8') as stdout, (output/'editor-check.stderr.log').open('w',encoding='utf-8') as stderr:
            try:
                process=subprocess.Popen(command,cwd=ROOT,env=environment,stdout=stdout,stderr=stderr,creationflags=subprocess.CREATE_NO_WINDOW)
                deadline=time.monotonic()+900
                while not (mailbox/'ready.json').is_file():
                    if process.poll() is not None: raise RuntimeError('Editor exited before mailbox ready')
                    if time.monotonic()>deadline: raise TimeoutError('Editor startup deadline')
                    time.sleep(.1)
                if digest(executable).lower()!=args.expected_editor_sha256.lower(): raise RuntimeError('Editor rebuilt to a different candidate')
                client=Mailbox(mailbox,token,process,output)
                exercise(client,output,asset,report)
                report['status']='PASS'
            except Exception as error:
                report['status']='FAIL'; report['error']=str(error)[:1800]
            finally:
                if process and process.poll() is None:
                    if client:
                        try: report['stopAcknowledged']=client.call('session.stop',timeout=5).get('stopping') is True
                        except Exception as error: report['stopError']=str(error)[:1000]
                    try: process.wait(timeout=255)
                    except subprocess.TimeoutExpired:
                        report['cleanupError']='Owned check/Storage has not completed; retained for recovery'
                        report['cleanupProcessId']=process.pid
                report['exitCode']=process.returncode if process else None
                report['normalStop']=bool(report.get('stopAcknowledged') and report['exitCode']==0)
                if not report['normalStop']: report['status']='FAIL'
                for label,operation in [('runtimeEvidence',lambda:collect_runtime_logs(output,report)),
                                        ('identity',lambda:None)]:
                    try:
                        stdout.flush(); stderr.flush()
                        if label=='identity':
                            report['inputsAfter']={str(p.relative_to(ROOT)):digest(p) for p in inputs}
                            report['inputsUnchanged']=report['inputsAfter']==inputs_before
                            report['editorSha256After']=digest(executable)
                            report['binaryUnchanged']=report['editorSha256After'].lower()==args.expected_editor_sha256.lower()
                            if not report['inputsUnchanged'] or not report['binaryUnchanged']: raise RuntimeError('Acceptance inputs or binary changed')
                        else: operation()
                    except Exception as error:
                        report['status']='FAIL'; report[label+'Error']=str(error)[:1800]
                shutil.copytree(scratch,output/'scratch-evidence')
                if process and process.returncode is not None:
                    for pattern in ('*.json','*.claimed'):
                        for pending in (mailbox/'requests').glob(pattern):
                            value=json.loads(pending.read_text()); value.pop('token',None); write_json(pending,value)
                storage['status']='unknown' if report.get('cleanupError') else 'success' if report['status']=='PASS' else 'failed'
                write_json(output/'report.json',report)
    print(json.dumps(dict(status=report['status'],checks=len(report['checks']),captures=len(report['captures']),report=str(output/'report.json'))))
    return 0 if report['status']=='PASS' else 1

if __name__=='__main__': raise SystemExit(main())
