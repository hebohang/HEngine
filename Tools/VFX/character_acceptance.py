"""Run real character/presentation receipts in owned, pinned Game sessions.

Never builds. Editor transactions and visual approval are separate evidence.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
from pathlib import Path
import shutil
import time
import uuid

from preview import ROOT, Session, Storage, digest, write_json

CASES = {
    "character": ("Assets/Effects/Authoring/CharacterDemo/CharacterAcceptance.he", "VFXCharacterAcceptance.json"),
    "presentation": ("Assets/Scenes/PresentationAcceptance.he", "PresentationDemo.json"),
}
REQUIRED_CHARACTER = {
    "instances_and_children_have_distinct_runtime_ids", "nested_name_overrides", "independent_animation_speed_overrides",
    "subtree_scripts_started", "controller_parameters_loaded", "independent_idle_run_state_selection",
    "continuous_blend_parameter_is_instance_local", "pause_keeps_both_progress_values",
    "step_one_instance_does_not_advance_other", "event_batch_has_no_duplicates", "event_drain_is_single_consumer",
    "multi_cycle_step_delivers_footsteps", "animation_event_started_presentation", "real_vfx_instance_started",
    "vfx_sdk_position_follows_moved_rotated_entity_socket", "presentation_light_follows_same_socket",
    "attack_transition_is_instance_local", "attack_contact_fires_once", "attack_vfx_uses_hand_local_id",
    "cancel_stops_attack_presentation", "presentation_pause_holds_timeline", "animation_cancel_clears_pending_events",
    "cancelled_vfx_handles_are_not_playing", "other_instance_resumes_after_cancel", "all_owned_presentation_tracks_cleaned",
} | {prefix+str(index) for index in range(2) for prefix in
     ("character_collision_component_", "feet_parent_rebound_", "lookup_from_member_is_instance_scoped_")}
REQUIRED_CHARACTER |= {
    'step_fixture_is_actual_single_walk_clip','continuous_speed_scan_and_authored_displacement','exact_cross_batch_events_large','exact_cross_batch_events_small',
    'cross_batch_event_ids_unique_large','cross_batch_event_ids_unique_small','extended_event_drain_once',
    'large_small_step_event_and_progress_equivalence','paused_animation_has_no_events_or_progress',
    'loop_boundary_progress_and_event_interval','cancel_discards_unconsumed_event_batch','cancel_does_not_reemit_on_later_frames',
    'continuous_entity_socket_vfx_light_follow','presentation_reload_preserves_active_snapshot_and_handles',
    'presentation_reload_new_request_uses_loaded_snapshot','destroyed_target_cancels_all_active_tracks',
    'destroyed_target_stops_actual_vfx','destroyed_target_destroys_actual_light','destroyed_target_stops_actual_audio',
    'scene_reload_has_live_owned_presentation'}
EXPECTED_CHECKPOINTS = {'idle','attack-pose','step-equivalence','loop-before','loop-after','socket-follow',
                        'target-destroyed','scene-reload-active'} | {'locomotion-'+str(i) for i in range(5)} | {'large-yaw-'+str(i) for i in range(3)}
REQUIRED_PRESENTATION = {"vfx_started","audio_started","light_created","sprite_changed","camera_shaken",
    "vfx_position_matches","light_position_matches","vfx_stopped","audio_stopped","light_destroyed",
    "sprite_restored","camera_restored","active_tracks_empty"}


def validate_receipt(case, value):
    if not isinstance(value,dict):
        raise ValueError("Runtime report must be an object")
    if value.get("error"):
        raise ValueError("Runtime report has an error: " + str(value.get("error", "invalid report"))[:1200])
    if case=="character":
        if value.get("status")!="passed":
            raise ValueError("Character runtime status: " + str(value.get("status")))
        checks=value.get("checks",[])
        if not isinstance(checks,list) or not all(isinstance(item,str) for item in checks):
            raise ValueError("Character checks must be explicit successful check names")
        missing=REQUIRED_CHARACTER-set(checks)
        if missing:
            raise ValueError("Missing character checks: " + ", ".join(sorted(missing)))
        instances=value.get("instances",[])
        if len(instances)!=2 or len(set(instances))!=2:
            raise ValueError("Character receipt lacks two distinct instances")
        names={item.get("name") for item in value.get("events",[])}
        if not {"attack","footstep"}.issubset(names) or not value.get("positions"):
            raise ValueError("Character event/position evidence is missing")
        for position in value["positions"]:
            before,after,expected=(position[key] for key in ("before","after","expected"))
            if not all(len(item)==3 for item in (before,after,expected)):
                raise ValueError("Position evidence must contain three coordinates")
            if not all(isinstance(number,(int,float)) and math.isfinite(number) for item in (before,after,expected) for number in item):
                raise ValueError("Position evidence contains nonfinite coordinates")
            if any(abs(a-b)>1e-4 for a,b in zip(after,expected)) or all(abs(a-b)<=1e-4 for a,b in zip(before,after)):
                raise ValueError("SDK position did not follow the moved socket")
        return dict(scope="character runtime assertions", passedChecks=len(checks))
    if value.get("status")!="completed":
        raise ValueError("Presentation timeline status: " + str(value.get("status")))
    composition=json.loads((ROOT/"Project/Assets/Effects/Authoring/Compositions/impact.json").read_text(encoding="utf-8"))
    ending=max(track["at"]+track["duration"] for track in composition["tracks"])
    if not isinstance(value.get("time"),(int,float)) or not math.isfinite(value["time"]) or value["time"]+1e-5<ending:
        raise ValueError("Presentation report ended before the last track")
    if value.get("schemaVersion",0)>=2:
        checks=value.get("checks",{})
        if not isinstance(checks,dict) or not REQUIRED_PRESENTATION.issubset(checks) or any(check is not True for check in checks.values()):
            raise ValueError("Presentation per-track readback or cleanup checks failed/missing")
        readback=value.get("readback",{})
        if not readback.get("tracks") or not readback.get("initial") or not readback.get("final"):
            raise ValueError("Presentation per-track readback evidence missing")
        return dict(scope="actual per-track state observations and cleanup",passedChecks=len(checks),
                    excluded=["visual approval","audible quality"])
    return dict(scope="timeline completion only", passedChecks=3,
                excluded=["per-track readback","visual approval","audible quality"])


def validate_checkpoint(value, previous_sequence):
    if not isinstance(value,dict) or value.get('schema')!=1:
        raise ValueError('Invalid character checkpoint schema')
    if not isinstance(value.get('nonce'),str) or not re.fullmatch('[0-9a-f]{32}',value['nonce']):
        raise ValueError('Invalid character checkpoint nonce')
    if type(value.get('sequence')) is not int or not (value['sequence']==previous_sequence+1 and value['sequence']<=64):
        raise ValueError('Invalid or repeated character checkpoint sequence')
    if not isinstance(value.get('label'),str) or not re.fullmatch('[a-z0-9-]{1,48}',value['label']):
        raise ValueError('Invalid character checkpoint label')
    return value


def validate_animation_observation(value):
    if value.get('available') is not True or value.get('source')!='lastSuccessfullySubmittedMainRenderWorld' or value.get('objectsTruncated') is not False:
        raise ValueError('Missing complete submitted animation observation')
    objects=value.get('objects',[])
    if len(objects)<2: raise ValueError('Two rendered character palettes are required')
    for item in objects:
        if item.get('currentPaletteCount',0)<=0 or item.get('hashCoversFullPalettes') is not True or item.get('nonFiniteComparisons')!=0:
            raise ValueError('Invalid rendered bone palette')
    return value


def read_bounded_json(path):
    if not path.exists(): return None
    if path.stat().st_size>1024*1024: raise ValueError('Character evidence exceeds 1 MiB')
    try: return json.loads(path.read_text(encoding='utf-8'))
    except json.JSONDecodeError: return None


def capture_checkpoint(session, directory, output, result, value):
    validate_checkpoint(value,result.get('checkpointSequence',0))
    if result.get('checkpointNonce',value['nonce'])!=value['nonce']:
        raise ValueError('Character checkpoint generation changed unexpectedly')
    status=session.call('session.status')['world']['runtimeControl']
    if status['pendingSteps'] or status['stepInProgress']:
        raise ValueError('Checkpoint capture requires a settled runtime step barrier')
    observation=validate_animation_observation(session.call('render.animation.status'))
    capture=session.call('capture.frame',dict(filename='character-'+value['label']+'.png'))
    destination=output/('character-'+value['label']+'.png')
    shutil.copyfile(capture['path'],destination)
    result.setdefault('checkpoints',[]).append(dict(checkpoint=value,animation=observation,
        capture=dict(receipt=capture,path=str(destination),sha256=digest(destination)),visualApproval='NOT_REVIEWED'))
    result['checkpointSequence']=value['sequence'];result['checkpointNonce']=value['nonce']
    # Publish only an exact generation/sequence acknowledgement in this owned Saved tree.
    acknowledgement=directory/'Saved/Reports/VFXCharacterCheckpointAck.json'
    temporary=acknowledgement.with_suffix('.tmp')
    temporary.write_text(json.dumps(dict(nonce=value['nonce'],sequence=value['sequence'])),encoding='utf-8')
    os.replace(temporary,acknowledgement)
    write_json(output/'report.json',result)


def validate_forced_reload(session, directory, scene, receipt):
    before=session.call('world.summary')
    live=[session.call('vfx.control',dict(handle=h,action='status')) for h in receipt['reloadHandles']]
    if not live or any(item.get('playing') is not True for item in live):
        raise AssertionError('Scene reload needs a still-live owned presentation')
    if any(item.get('paused') is not True for item in live):
        raise AssertionError('Scene reload fixture must remain paused across checkpoint handshakes')
    marker=directory/'Saved/Reports/VFXCharacterReloadRequested.json'
    marker.write_text(json.dumps(dict(scope='Skip rerunning completed acceptance after forced scene replacement')),encoding='utf-8')
    queued=session.call('runtime.scene.load',dict(path=scene,forceReload=True))
    if queued.get('ok') is not True: raise AssertionError('Forced scene reload was not queued')
    after=before
    for _ in range(60):
        session.call('runtime.step',dict(frames=1))
        status=session.call('session.status');after=session.call('world.summary')
        if not status.get('runtimeSceneRequestPending') and after.get('sceneGeneration')!=before.get('sceneGeneration'): break
    if after.get('sceneGeneration',0)<=0 or after.get('sceneGeneration')==before.get('sceneGeneration'):
        raise AssertionError('Forced scene reload did not commit a new generation')
    stopped=[session.call('vfx.control',dict(handle=handle,action='status')) for handle in receipt['reloadHandles']]
    if not stopped or any(item.get('playing') is not False for item in stopped):
        raise AssertionError('Scene reload retained a presentation VFX handle')
    # Persisted scene UUIDs can legally recur. Only the runtime-created light must disappear.
    light=session.call('world.get',dict(uuid=str(receipt['reloadTargets'][-1])))
    if light.get('found') is not False: raise AssertionError('Scene reload retained the temporary presentation light')
    return dict(status='PASS',before=before,after=after,liveHandles=live,handles=stopped,temporaryLight=light,
                scope='Forced scene generation replacement and actual old paused VFX/light cleanup; not animation hot reload')


def owned_run_directory(session, pinned):
    owner=json.loads((ROOT/"out/agent/sessions/active.json").read_text(encoding="utf-8-sig"))
    if owner.get("ownerTag","").lower()!=session.owner.lower() or owner.get("artifactSha256","").lower()!=pinned.lower():
        raise RuntimeError("Active session ownership or executable hash differs")
    # Never serialize the owner record: it contains the private mailbox token.
    directory=Path(owner["runDirectory"]).resolve()
    if not directory.is_relative_to((ROOT/"out/agent/sessions").resolve()):
        raise RuntimeError("Owned session directory is outside the expected session root")
    return directory


def input_hashes():
    paths={Path(__file__).resolve()}
    for folder in ("Project/Assets/Effects/Authoring/CharacterDemo", "Project/Assets/Effects/Authoring/CharacterRuntimeProbe", "Project/Assets/Effects/Authoring/Compositions",
                   "Project/Assets/Models/Quaternius/UltimateMonsters/BlueDemon"):
        paths.update(path for path in (ROOT/folder).iterdir() if path.is_file())
    for name in ("Project/Scripts/VFXCharacterAcceptance.py", "Project/Scripts/PresentationDemo.py",
                 "Project/Scripts/PresentationPlayer.py", "Project/Scripts/PresentationSchema.py",
                 "Project/Assets/Scenes/PresentationAcceptance.he", "Project/Assets/Effects/Authoring/hit-ember.efkpkg",
                 "Project/Assets/Audio/SFX/Gameplay/monster_hit_01.wav"):
        paths.add(ROOT/name)
    return {path.relative_to(ROOT).as_posix():digest(path) for path in sorted(paths)}


def run_case(case, output, backend, pinned, timeout, max_frames):
    output.mkdir()
    scene,filename=CASES[case]
    session=Session(output,backend,pinned,scene)
    result=dict(case=case,status="RUNNING",scene=scene,sceneSha256=digest(ROOT/"Project"/scene),
                evidenceType="runtime-manual",editorTransactions="NOT_RUN",visualApproval="NOT_REVIEWED")
    started_ns=time.time_ns()
    deadline=time.monotonic()+timeout
    runtime_directory=None
    try:
        session.start()
        runtime_directory=owned_run_directory(session,pinned)
        result["sessionDirectory"]=str(runtime_directory)
        result["session"]=session.call("session.status")
        report_path=runtime_directory/"Saved/Reports"/filename
        frames=0
        while True:
            if case=='character':
                checkpoint=read_bounded_json(runtime_directory/'Saved/Reports/VFXCharacterCheckpoint.json')
                if checkpoint and checkpoint.get('sequence',0)>result.get('checkpointSequence',0):
                    capture_checkpoint(session,runtime_directory,output,result,checkpoint)
            if report_path.exists():
                metadata=report_path.stat()
                if metadata.st_size>1024*1024:
                    raise RuntimeError("Runtime report exceeds 1 MiB")
                if metadata.st_mtime_ns<started_ns:
                    raise RuntimeError("Refusing stale runtime report")
                try:
                    payload=report_path.read_bytes()
                    value=json.loads(payload)
                except (json.JSONDecodeError,UnicodeDecodeError):
                    value=None  # A writer may be between open and flush.
                if value is not None:
                    (output/filename).write_bytes(payload)
                    result["receiptSha256"]=digest(output/filename)
                    result["receiptMtimeNs"]=metadata.st_mtime_ns
                    result["validation"]=validate_receipt(case,value)
                    if case=='character':
                        if value.get('schemaVersion')!=2 or len(result.get('checkpoints',[]))!=len(value.get('checkpoints',[])) or {r['checkpoint']['label'] for r in result.get('checkpoints',[])}!=EXPECTED_CHECKPOINTS:
                            raise ValueError('Extended character checkpoint evidence missing')
                        result['forcedSceneReload']=validate_forced_reload(session,runtime_directory,scene,value)
                        result['coverageLimitations']=['Entity-local sockets only; no bone sockets or animation hot reload',
                            'Palette hashes/progress are not numerical cross-instance matrix equivalence or quaternion orthogonality; use AnimationPoseContract and visual review']
                    break
            if frames>=max_frames or time.monotonic()>=deadline:
                raise TimeoutError("No valid fresh runtime report within frame/wall-time budget")
            count=min(30,max_frames-frames)
            ticket=session.call("runtime.step",dict(frames=count))
            target=ticket["targetCompletedSteps"]
            while True:
                state=session.call("session.status")["world"]["runtimeControl"]
                if state["completedSteps"]>=target and not state["pendingSteps"] and not state["stepInProgress"]:
                    break
                if time.monotonic()>=deadline:
                    raise TimeoutError("Runtime step ticket did not complete")
                time.sleep(.1)
            frames+=count
        result["steppedFrames"]=frames
        result["vfxStats"]=session.call("vfx.stats")
        capture=session.call("capture.frame",dict(filename=case+"-complete.png"))
        shutil.copyfile(capture["path"],output/(case+"-complete.png"))
        result["capture"]=dict(path=str(output/(case+"-complete.png")),sha256=digest(output/(case+"-complete.png")))
        result["status"]="PASS"
    except Exception as error:
        result["status"]="FAIL"
        result["error"]=str(error)[:1600]
    finally:
        try:
            if runtime_directory:
                # Bounded text tails survive session-history cleanup; no owner token copied.
                for name in ("game.stdout.log","game.stderr.log"):
                    path=runtime_directory/name
                    if path.exists():
                        with path.open("rb") as source:
                            source.seek(max(0,path.stat().st_size-32768))
                            (output/(name+".tail.txt")).write_bytes(source.read(32768))
        except Exception as error:
            result["logCopyError"]=str(error)[:1200]
            result["status"]="FAIL"
        try:
            session.stop()
        except Exception as error:
            result["cleanupError"]=str(error)[:1200]
            result["status"]="FAIL"
        write_json(output/"report.json",result)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend",choices=["DX12","Vulkan"],required=True)
    parser.add_argument("--existing-build-sha256",required=True)
    parser.add_argument("--case",choices=["both",*CASES],default="both")
    parser.add_argument("--output",type=Path)
    parser.add_argument("--timeout",type=int,default=600)
    parser.add_argument("--max-frames",type=int,default=1500)
    args=parser.parse_args()
    if not 10<=args.timeout<=600 or not 30<=args.max_frames<=1800:
        parser.error("timeout must be 10..600 seconds and max-frames 30..1800")
    executable=ROOT/"out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe"
    pinned=digest(executable)
    if pinned.lower()!=args.existing_build_sha256.lower():
        parser.error("Pinned candidate SHA256 mismatch; no session started")
    evidence_root=ROOT/"out/agent/rendering-evidence/vfx-authoring"
    output=(args.output or evidence_root/("character-"+args.backend.lower()+"-"+uuid.uuid4().hex[:12])).resolve()
    if not output.is_relative_to(evidence_root.resolve()) or output==evidence_root.resolve():
        parser.error("output must be a new child of out/agent/rendering-evidence/vfx-authoring")
    output.mkdir(parents=True,exist_ok=False)
    report=dict(status="RUNNING",backend=args.backend,executableSha256=pinned,inputs=input_hashes(),cases=[],
                sourceIdentityScope="Pinned binary runtime only; not a build of later source edits")
    with Storage(ROOT).workflow_scope(str(ROOT/"Project"),"vfx-character-acceptance",[output]) as storage_run:
        try:
            for case in (CASES if args.case=="both" else [args.case]):
                report["cases"].append(run_case(case,output/case,args.backend,pinned,args.timeout,args.max_frames))
                if report["cases"][-1].get("cleanupError"):
                    break  # Never start a second session while ownership remains unresolved.
            report["inputsUnchanged"]=input_hashes()==report["inputs"]
            report["binaryUnchanged"]=digest(executable)==pinned
            report["status"]="PASS" if report["inputsUnchanged"] and report["binaryUnchanged"] and all(case["status"]=="PASS" for case in report["cases"]) else "FAIL"
            storage_run["status"]="unknown" if any(case.get("cleanupError") for case in report["cases"]) else "success" if report["status"]=="PASS" else "failed"
        except Exception as error:
            report["status"]="FAIL"; report["error"]=str(error)[:1600]
            storage_run["status"]="unknown" if any(case.get("cleanupError") for case in report["cases"]) else "failed"
        finally:
            write_json(output/"report.json",report)
    print(json.dumps(dict(status=report["status"],report=str(output/"report.json"),cases=[
        dict(case=item["case"],status=item["status"],error=item.get("error","")) for item in report["cases"]])))
    return 0 if report["status"]=="PASS" else 1


if __name__=="__main__":
    raise SystemExit(main())
