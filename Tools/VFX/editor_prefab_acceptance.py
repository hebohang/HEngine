"""Real bounded Editor scene transactions through the opt-in main-thread mailbox.

Launches hectl editor check (which may build); never uses HGame as Editor evidence.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import uuid

from preview import ROOT, Storage, digest, write_json


VALIDATION_ERROR = re.compile(
    r"VUID-|Vulkan validation: Validation Error:|NVRHI validation error:|DX12 Debug Layer:|"
    r"NVRHI Vulkan: Device Removed|VK_ERROR_DEVICE_LOST|DXGI_ERROR_DEVICE_(REMOVED|HUNG|RESET)", re.I)


def collect_runtime_logs(output, report, log_root=None):
    """Retain both native streams even when check failed; never accept wrapper tails."""
    log_root = (log_root or ROOT/"out/agent/logs").resolve()
    wrapper = (output/"editor-check.stdout.log").read_text(encoding="utf-8-sig", errors="replace")
    matches = re.findall(r"run HEngineEditor: STARTED pid=(\d+); stdout: ([^;\r\n]+); stderr: ([^\r\n]+)", wrapper)
    if len(matches) != 1:
        raise RuntimeError("Expected exactly one native Editor STARTED receipt")
    pid, stdout_path, stderr_path = matches[0]
    report["nativeProcessId"] = int(pid)
    report["runtimeLogs"] = {}
    errors = []
    for stream, source_path in (("stdout", stdout_path), ("stderr", stderr_path)):
        source = Path(source_path.strip()).resolve()
        if not source.is_relative_to(log_root) or not source.is_file():
            raise RuntimeError("Native Editor log missing or outside Agent logs: "+str(source))
        destination = output/("editor."+stream+".log")
        shutil.copyfile(source, destination)
        report["runtimeLogs"][stream] = dict(path=str(destination), sha256=digest(destination))
        with destination.open(encoding="utf-8-sig", errors="replace") as lines:
            for number, line in enumerate(lines, 1):
                if VALIDATION_ERROR.search(line):
                    errors.append(dict(stream=stream, line=number, text=line.strip()[:1800]))
    write_json(output/"runtime-validation-errors.json", errors)
    report["validationErrorCount"] = len(errors)
    report["nativeCheckPassed"] = "run HEngineEditor: PASS exit=0" in wrapper
    if errors or not report["nativeCheckPassed"]:
        raise RuntimeError("Native Editor validation or bounded check failed; inspect retained logs")


def input_hashes():
    source = ROOT/"Project/Assets/Effects/Authoring/CharacterDemo"
    paths = [source/name for name in ("Actor.hprefab", "ActorA.hprefab", "ActorB.hprefab",
             "Actor.propagation-v2.hprefab", "CharacterAcceptance.he")]
    paths += [ROOT/"Project/Scripts/VFXCharacterAcceptance.py", Path(__file__).resolve()]
    return {str(path.relative_to(ROOT)): digest(path) for path in paths}


def atomic(path, value):
    temporary=path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value,separators=(",",":")),encoding="utf-8")
    os.replace(temporary,path)


class Mailbox:
    def __init__(self, directory, token, process, evidence):
        self.directory,self.token,self.process,self.evidence=directory,token,process,evidence
        self.index=0

    def call(self, method, params=None, timeout=15):
        identifier=uuid.uuid4().hex
        request=dict(protocolVersion=1,requestId=identifier,token=self.token,method=method,params=params or {})
        atomic(self.directory/"requests"/(identifier+".json"),request)
        destination=self.directory/"responses"/(identifier+".json")
        deadline=time.monotonic()+timeout
        while not destination.exists():
            if self.process.poll() is not None:
                raise RuntimeError("Owned Editor check ended before response to "+method)
            if time.monotonic()>deadline:
                raise TimeoutError("Editor response timeout: "+method)
            time.sleep(.05)
        response=json.loads(destination.read_text(encoding="utf-8-sig"))
        self.index+=1
        write_json(self.evidence/(f"{self.index:03d}-"+method+".json"),dict(method=method,params=params or {},response=response))
        if response.get("requestId")!=identifier or response.get("protocolVersion")!=1 or response.get("ok") is not True:
            raise RuntimeError("Editor transport rejected "+method+": "+str(response.get("error"))[:1200])
        return response["result"]

    def scene(self, action, params=None):
        result=self.call("scene."+action,params)
        deadline=time.monotonic()+20
        while result.get("completion")=="queued":
            if time.monotonic()>deadline:
                raise TimeoutError("Editor scene ticket timeout")
            result=self.call("scene.result",dict(ticket=result["ticket"]))
            if result.get("completion")=="queued": time.sleep(.05)
        if result.get("ok") is not True:
            raise RuntimeError("Editor scene "+action+" failed: "+str(result.get("error"))[:1600])
        return result

    def describe(self):
        return self.scene("describe")

    def edit(self, operations):
        state=self.describe()
        request=dict(sceneId=state["sceneId"],baseRevision=state["revision"],requestId=str(uuid.uuid4()),operations=operations)
        planned=self.scene("plan",request)
        if planned.get("completion")!="planned" or planned.get("revision")!=state["revision"] or planned.get("sceneId")!=state["sceneId"]:
            raise RuntimeError("Editor plan changed revision or returned an invalid receipt")
        applied=self.scene("apply",request)
        if applied.get("completion")!="authorCommitted" or applied.get("sceneId")!=state["sceneId"]:
            raise RuntimeError("Editor apply did not author-commit the requested scene")
        return applied

    def control(self, action, **extra):
        state=self.describe()
        return self.scene(action,dict(sceneId=state["sceneId"],baseRevision=state["revision"],requestId=str(uuid.uuid4()),**extra))

    def entities(self):
        state=self.describe()
        result=self.scene("query",dict(sceneId=state["sceneId"],limit=100,fields=["uuid","name","parent","TransformComponent","AnimatorComponent"]))
        if result.get("nextOffset") is not None:
            raise RuntimeError("Acceptance fixture unexpectedly exceeds its bounded query page")
        return result["entities"]


def prepare(scratch):
    source=ROOT/"Project/Assets/Effects/Authoring/CharacterDemo"
    prefix=scratch.relative_to(ROOT/"Project").as_posix()+"/"
    old_prefix="Assets/Effects/Authoring/CharacterDemo/"
    def remap(value):
        if isinstance(value,dict): return {key:remap(item) for key,item in value.items()}
        if isinstance(value,list): return [remap(item) for item in value]
        if isinstance(value,str):
            if value in (old_prefix+"Actor.hprefab",): return prefix+"Actor.hprefab"
            if value=="Scripts.VFXCharacterAcceptance.py": return "Scripts/VFXCharacterAcceptance.py"
        return value
    for name in ("Actor.hprefab","ActorA.hprefab","ActorB.hprefab","Actor.propagation-v2.hprefab"):
        write_json(scratch/name,remap(json.loads((source/name).read_text(encoding="utf-8"))))
    scene=json.loads((source/"CharacterAcceptance.he").read_text(encoding="utf-8"))
    for value in scene.values(): value.pop("PythonScriptComponent",None)
    write_json(scratch/"EditorAcceptance.he",scene)
    return prefix


def verify_state(rows, roots, updated):
    by_id={row["uuid"]:row for row in rows}
    found={}
    for label,root in roots.items():
        actor=next(row for row in rows if row["parent"]==root and row["name"]=="CharacterActor"+label)
        hand=next(row for row in rows if row["parent"]==actor["uuid"] and row["name"]=="CharacterHand")
        visual=next(row for row in rows if row["parent"]==actor["uuid"] and row["name"]=="CharacterVisual")
        expected=1.4 if updated and label=="B" else 1.2
        if abs(hand["TransformComponent"]["Translation"][1]-expected)>1e-5:
            raise AssertionError("hand override/propagation failed for "+label)
        if abs(visual["AnimatorComponent"]["playSpeed"]-(1 if label=="A" else .65))>1e-5:
            raise AssertionError("nested speed override lost for "+label)
        badge=[row for row in rows if row["parent"]==actor["uuid"] and row["name"]=="TemplateAddedBadge"]
        if len(badge)!=(1 if updated else 0): raise AssertionError("template child propagation failed for "+label)
        found[label]=dict(actor=actor["uuid"],hand=hand["uuid"],visual=visual["uuid"])
    if roots["A"]==roots["B"] or set(found["A"].values()) & set(found["B"].values()):
        raise AssertionError("Instances share runtime entity IDs")
    # Every ancestry chain in either subtree must reach its own root, not the other instance.
    for label,group in found.items():
        for identifier in group.values():
            seen=set()
            while identifier!=roots[label]:
                if identifier in seen or identifier not in by_id: raise AssertionError("Invalid parent chain")
                seen.add(identifier); identifier=by_id[identifier]["parent"]
    return found


def verify_actor_scripts(document, identities):
    for group in identities.values():
        path=document[group["actor"]]["PythonScriptComponent"]["Path"]
        if path!="Scripts/VFXCharacterAcceptance.py" or not (ROOT/"Project"/path).is_file():
            raise AssertionError("Saved actor script path is not the real project script: "+str(path))


def exercise(client,scratch,prefix,report):
    result=client.edit([dict(op="prefab",path=prefix+"ActorA.hprefab",temporaryId="$a"),
                        dict(op="prefab",path=prefix+"ActorB.hprefab",temporaryId="$b")])
    roots={"A":result["temporaryIds"]["$a"],"B":result["temporaryIds"]["$b"]}
    original_rows=client.entities()
    identities=verify_state(original_rows,roots,False)
    report["checks"].append("Editor main-thread prefab plan/apply creates two isolated nested character instances")
    client.edit([dict(op="prefab_override",entity=identities["A"]["hand"],pointer="/TransformComponent/Translation",mode="set")])
    # This file belongs exclusively to this run. Keep both candidates as evidence.
    baseline=(scratch/"Actor.hprefab").read_bytes()
    atomic(scratch/"Actor.hprefab",json.loads((scratch/"Actor.propagation-v2.hprefab").read_text(encoding="utf-8")))
    try:
        client.edit([dict(op="prefab_sync",entity=roots[label]) for label in ("A","B")])
        verify_state(client.entities(),roots,True)
        report["checks"].append("explicit same-value vector override preserved while nested template fields/children propagate")
        client.control("undo")
        verify_state(client.entities(),roots,False)
        client.control("redo")
        expected_rows=client.entities()
        verify_state(expected_rows,roots,True)
        report["checks"].append("Editor undo and redo restore both subtree versions atomically")
        saved_path=prefix+"Reopened.he"
        client.control("save",path=saved_path)
        saved=json.loads((scratch/"Reopened.he").read_text(encoding="utf-8"))
        verify_actor_scripts(saved,identities)
        override=json.loads(saved[identities["A"]["hand"]]["PrefabInstanceComponent"]["OverridePathsJson"])
        if "/TransformComponent/Translation" not in override: raise AssertionError("Saved explicit override metadata missing")
        client.call("editor.open",dict(path=saved_path))
        reopened=client.describe()
        if reopened["path"]!=saved_path: raise AssertionError("Editor did not reopen saved scene")
        actual_rows=client.entities()
        verify_state(actual_rows,roots,True)
        if actual_rows!=expected_rows: raise AssertionError("Editor reopen changed author component values or IDs")
        client.control("save")
        after=json.loads((scratch/"Reopened.he").read_text(encoding="utf-8"))
        verify_actor_scripts(after,identities)
        if after!=saved: raise AssertionError("Editor save/reopen/save changed serialized scene")
        report["checks"].append("real Editor save/open/save preserves UUIDs, references and explicit override metadata")
        report["roots"]=roots
        report["savedScene"]=str(scratch/"Reopened.he")
    finally:
        (scratch/"Actor.hprefab").write_bytes(baseline)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend",choices=["DX12","Vulkan"],required=True)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--expected-editor-sha256",required=True)
    parser.add_argument("--startup-timeout",type=int,default=900)
    args=parser.parse_args()
    if not 30<=args.startup_timeout<=1800: parser.error("startup-timeout must be 30..1800 seconds")
    allowed=(ROOT/"out/agent/rendering-evidence/vfx-authoring").resolve()
    output=args.output.resolve()
    if not output.is_relative_to(allowed) or output==allowed: parser.error("output must be a new vfx-authoring rendering-evidence child")
    executable=ROOT/"out/agent/editor-relwithdebinfo/bin/HEngineEditor.exe"
    if digest(executable).lower()!=args.expected_editor_sha256.lower(): parser.error("Expected Editor binary does not match; build the reviewed candidate first")
    inputs_before=input_hashes()
    output.mkdir(parents=True,exist_ok=False)
    token=uuid.uuid4().hex
    mailbox=output/"mailbox"
    for name in ("requests","responses"): (mailbox/name).mkdir(parents=True)
    scratch=ROOT/"Project/Assets/Effects/Authoring/EditorAcceptance"/token
    scratch.mkdir(parents=True,exist_ok=False)
    prefix=prepare(scratch)
    report=dict(status="RUNNING",host="Editor",backend=args.backend,evidenceType="runtime-manual",checks=[],
                scratch=str(scratch),expectedEditorSha256=args.expected_editor_sha256,visualApproval="NOT_CAPTURED",
                visualScope="Main-thread author transactions only; mailbox has no capture method. No UI layout, animation playback or rendered appearance approval.",
                captures=[],inputsBefore=inputs_before,
                sourceIdentityScope="Hashes cover this driver and original author fixtures/script; pinned Editor is not evidence of later source changes.")
    process=None; client=None
    environment=os.environ.copy()
    environment.update(HE_EDITOR_SCENE_MAILBOX=str(mailbox),HE_EDITOR_SCENE_TOKEN=token,HE_EDITOR_SCENE_ROOT=str(scratch))
    command=["powershell.exe","-NoProfile","-ExecutionPolicy","Bypass","-File",str(ROOT/"Tools/Agent/hectl.ps1"),
             "editor","check","-Configuration","RelWithDebInfo","-Parallel","2","-Frames","6000","-TimeoutSeconds","180",
             "-StartupScene",prefix+"EditorAcceptance.he","-GraphicsApi",args.backend,"-StrictGraphicsApi","-RenderValidation"]
    report["command"]=command
    with Storage(ROOT).workflow_scope(str(ROOT/"Project"),"vfx-editor-prefab",[output]) as storage_run:
        # Propagate the outer Storage identity, set by workflow_scope after the original env snapshot.
        if os.environ.get("HE_STORAGE_PARENT"): environment["HE_STORAGE_PARENT"]=os.environ["HE_STORAGE_PARENT"]
        with (output/"editor-check.stdout.log").open("w",encoding="utf-8") as stdout, (output/"editor-check.stderr.log").open("w",encoding="utf-8") as stderr:
            try:
                process=subprocess.Popen(command,cwd=ROOT,env=environment,stdout=stdout,stderr=stderr,creationflags=subprocess.CREATE_NO_WINDOW)
                deadline=time.monotonic()+args.startup_timeout
                while not (mailbox/"ready.json").exists():
                    if process.poll() is not None: raise RuntimeError("Editor check exited before mailbox ready")
                    if time.monotonic()>deadline: raise TimeoutError("Editor mailbox startup deadline exceeded")
                    time.sleep(.1)
                report["ready"]=json.loads((mailbox/"ready.json").read_text(encoding="utf-8-sig"))
                client=Mailbox(mailbox,token,process,output)
                report["editorSha256"]=digest(executable)
                if report["editorSha256"].lower()!=args.expected_editor_sha256.lower(): raise RuntimeError("hectl check rebuilt a different Editor; reject this candidate")
                exercise(client,scratch,prefix,report)
                report["status"]="PASS"
            except Exception as error:
                report["status"]="FAIL"; report["error"]=str(error)[:1800]
            finally:
                if process and process.poll() is None:
                    if client:
                        try:
                            stop=client.call("session.stop",timeout=5)
                            report["stopAcknowledged"]=stop.get("stopping") is True
                            if not report["stopAcknowledged"]: raise RuntimeError("Editor did not acknowledge stopping")
                        except Exception as error: report["stopError"]=str(error)[:1000]
                    try: process.wait(timeout=195)
                    except subprocess.TimeoutExpired:
                        # hectl owns the bounded Editor shutdown. Its process can
                        # outlive Editor while Storage collects history; killing
                        # the tree here would interrupt that protected collector.
                        report["cleanupError"]="Owned check has not finished shutdown/Storage cleanup; process retained for recovery"
                        report["cleanupProcessId"]=process.pid
                        report["status"]="FAIL"
                if process:
                    report["exitCode"]=process.returncode
                    if process.returncode!=0: report["status"]="FAIL"
                report["normalStop"]=bool(report.get("stopAcknowledged") and process and process.returncode==0)
                if not report["normalStop"]: report["status"]="FAIL"
                try:
                    report["editorSha256After"]=digest(executable)
                    report["inputsAfter"]=input_hashes()
                    report["inputsUnchanged"]=report["inputsAfter"]==inputs_before
                    report["binaryUnchanged"]=report["editorSha256After"].lower()==args.expected_editor_sha256.lower()
                    if not report["inputsUnchanged"] or not report["binaryUnchanged"]:
                        raise RuntimeError("Original author inputs, driver or pinned Editor changed during acceptance")
                except Exception as error:
                    report["evidenceError"]=str(error)[:1800]; report["status"]="FAIL"
                try:
                    stdout.flush(); stderr.flush()
                    collect_runtime_logs(output,report)
                except Exception as error:
                    report["runtimeEvidenceError"]=str(error)[:1800]; report["status"]="FAIL"
                if process and process.returncode is not None:
                    # Pending requests contain the token. Keep payloads only in sanitized command receipts.
                    pending_requests=[*(mailbox/"requests").glob("*.json"),*(mailbox/"requests").glob("*.claimed")]
                    for pending in pending_requests:
                        value=json.loads(pending.read_text(encoding="utf-8")); value.pop("token",None); atomic(pending,value)
                shutil.copytree(scratch,output/"scratch-evidence")
                storage_run["status"]="unknown" if report.get("cleanupError") else "success" if report["status"]=="PASS" else "failed"
                write_json(output/"report.json",report)
    print(json.dumps(dict(status=report["status"],checks=len(report["checks"]),error=report.get("error",""),report=str(output/"report.json"))))
    return 0 if report["status"]=="PASS" else 1


if __name__=="__main__":
    raise SystemExit(main())
