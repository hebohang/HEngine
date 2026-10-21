"""Exercise Editor picking after an in-place scene transform in an owned check run."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import time
import uuid


ROOT = Path(__file__).resolve().parents[2]
EVIDENCE_ROOT = ROOT / "out/agent/rendering-evidence/vfx-authoring"


def atomic_json(path: Path, value: object) -> None:
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, separators=(",", ":")), encoding="utf-8")
    os.replace(temporary, path)


def run(project: Path) -> None:
    project = project.resolve()
    token = uuid.uuid4().hex
    evidence = EVIDENCE_ROOT / ("editor-pick-" + token)
    mailbox = evidence / "mailbox"
    scratch = project / "Assets/Effects/Authoring/EditorAcceptance" / token
    (mailbox / "requests").mkdir(parents=True)
    (mailbox / "responses").mkdir()
    scratch.mkdir(parents=True)
    entity = str(uuid.uuid4())
    scene_path = scratch / "PickAcceptance.he"
    atomic_json(scene_path, {entity: {
        "TagComponent": {"Tag": "PickTarget"},
        "TransformComponent": {"ParentUuid": "", "Translation": [0, 0, 0],
                               "Rotation": [0, 0, 0], "Scale": [1, 1, 1]},
        "MeshComponent": {"Path": "Assets/Models/Box/Box.hmesh", "meshResPath": ""},
    }})
    startup = scene_path.relative_to(project).as_posix()
    command = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
               str(ROOT / "Tools/Agent/hectl.ps1"), "editor", "check", "-Project", str(project),
               "-StartupScene", startup, "-Frames", "6000", "-Resolution", "1280x720",
               "-TimeoutSeconds", "180"]
    environment = os.environ.copy()
    environment.update(HE_EDITOR_SCENE_MAILBOX=str(mailbox), HE_EDITOR_SCENE_TOKEN=token,
                       HE_EDITOR_SCENE_ROOT=str(scratch))
    checks: list[str] = []
    result: dict = {"status": "FAIL", "command": command, "checks": checks,
                    "scene": str(scene_path), "entity": entity}
    process = None
    with (evidence / "check.stdout.log").open("w", encoding="utf-8") as stdout, \
         (evidence / "check.stderr.log").open("w", encoding="utf-8") as stderr:
        try:
            process = subprocess.Popen(command, cwd=ROOT, env=environment, stdout=stdout,
                                       stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW)
            deadline = time.monotonic() + 120
            while not (mailbox / "ready.json").exists():
                if process.poll() is not None:
                    raise RuntimeError("Editor exited before mailbox ready")
                if time.monotonic() >= deadline:
                    raise TimeoutError("Mailbox did not become ready")
                time.sleep(.1)

            def call(method: str, params: dict | None = None) -> dict:
                identifier = uuid.uuid4().hex
                atomic_json(mailbox / "requests" / (identifier + ".json"),
                            {"protocolVersion": 1, "requestId": identifier, "token": token,
                             "method": method, "params": params or {}})
                destination = mailbox / "responses" / (identifier + ".json")
                deadline = time.monotonic() + 20
                while not destination.exists():
                    if process.poll() is not None:
                        raise RuntimeError("Editor exited before " + method)
                    if time.monotonic() >= deadline:
                        raise TimeoutError(method)
                    time.sleep(.05)
                response = json.loads(destination.read_text(encoding="utf-8-sig"))
                if not response.get("ok"):
                    raise RuntimeError(f"{method}: {response.get('error')}")
                return response["result"]

            def scene(action: str, params: dict | None = None) -> dict:
                response = call("scene." + action, params)
                while response.get("completion") == "queued":
                    response = call("scene.result", {"ticket": response["ticket"]})
                    time.sleep(.05)
                if not response.get("ok"):
                    raise RuntimeError(f"scene.{action}: {response.get('error')}")
                return response

            def pick(x: float) -> dict:
                return call("editor.pick", {"origin": [x, 0, 5], "direction": [0, 0, -1],
                                            "maxDistance": 20})

            first = pick(0)
            if first.get("entity") != entity:
                raise AssertionError(f"Initial pick missed fixture: {first}")
            checks.append("initial box ray hits fixture UUID")
            state = scene("describe")
            request = {"sceneId": state["sceneId"], "baseRevision": state["revision"],
                       "requestId": str(uuid.uuid4()),
                       "operations": [{"op": "transform", "entity": entity,
                                       "patch": {"Translation": [100, 0, 0]}}]}
            planned = scene("plan", request)
            if planned.get("completion") != "planned":
                raise AssertionError(f"Transform planning failed: {planned}")
            applied = scene("apply", request)
            if applied.get("completion") != "authorCommitted":
                raise AssertionError(f"Transform was not committed: {applied}")
            old = pick(0)
            moved = pick(100)
            if old.get("hit") or moved.get("entity") != entity:
                raise AssertionError(f"Pick scene did not refresh: old={old}, moved={moved}")
            checks.append("after in-place transform old ray misses and moved ray hits same UUID")
            result["status"] = "PASS"
        except Exception as error:
            result["error"] = str(error)
        finally:
            if process is not None and process.poll() is None:
                try:
                    result["stop"] = call("session.stop")
                except Exception as error:
                    result["stopError"] = str(error)
                try:
                    process.wait(timeout=190)
                except subprocess.TimeoutExpired:
                    result["shutdownError"] = "Owned check did not stop within deadline"
                    result["status"] = "FAIL"
            if process is not None:
                result["exitCode"] = process.returncode
                if process.returncode != 0:
                    result["status"] = "FAIL"
            atomic_json(evidence / "result.json", result)
    print(json.dumps({"status": result["status"], "checks": checks,
                      "error": result.get("error"), "evidence": str(evidence)}, ensure_ascii=False))
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path(r"C:\WorkSpace\HEngineProjects\MechanicGirlLab"))
    run(parser.parse_args().project)
