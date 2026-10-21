#!/usr/bin/env python3
"""Editor publish-panel adapter with receipts, ownership and failure preservation."""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--action", choices=("Check", "Build"), default="Build")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("macOS applications must be built on a macOS host")
    repo = Path(__file__).resolve().parents[2]
    os.environ["PATH"] = os.pathsep.join([str(path) for path in (repo / "out/agent/dependencies/macos/bin", Path("/opt/homebrew/bin"), Path("/usr/local/bin")) if path.is_dir()] + [os.environ.get("PATH", "")])
    project = args.project.resolve()
    if not re.fullmatch("[A-Za-z0-9_-]+", args.profile):
        parser.error("Unsafe build profile identifier")
    profiles = json.loads((project / "Config/BuildProfiles.json").read_text(encoding="utf-8-sig"))
    selected = [p for p in profiles["profiles"] if p["id"] == args.profile]
    if len(selected) != 1 or selected[0]["platform"] != "macOS" or selected[0]["configuration"] != "Release":
        parser.error("Select one macOS Release build profile")
    profile = selected[0]
    descriptor = json.loads((project / "Project.heproject").read_text(encoding="utf-8-sig"))
    output_root = Path(profile["output"])
    if not output_root.is_absolute():
        output_root = project / output_root
    output = output_root.resolve() / (descriptor["ExecutableName"] + ".app")
    if output.resolve() == project or project.is_relative_to(output.resolve()):
        parser.error("Output must not contain the source project")
    key = hashlib.sha256(str(project).encode()).hexdigest()[:12]
    work = repo / "out/agent/build-publish" / key / args.profile
    work.mkdir(parents=True, exist_ok=True)
    receipt_path = project / "Saved/Build" / (args.profile + ".json")
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    receipt = {"schema": 1, "profile": args.profile, "platform": "macOS", "action": args.action,
               "success": False, "status": "Running", "output": str(output), "log": str(work / "build.log"),
               "error": "", "started": datetime.datetime.now(datetime.timezone.utc).isoformat()}
    def save():
        temporary = receipt_path.with_name(receipt_path.name + ".tmp-" + uuid.uuid4().hex)
        temporary.write_text(json.dumps(receipt, indent=2) + "\n")
        temporary.replace(receipt_path)
    with (work / "publish.lock").open("a") as guard:
        fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
        save()
        try:
            with (work / "build.log").open("w") as log:
                for tool in ("xcrun", "cmake", "ninja", "pwsh", "codesign"):
                    if not shutil.which(tool):
                        raise RuntimeError("Required build tool is missing: " + tool)
                preflight = [sys.executable, str(repo / "Tools/Build/Preflight.py"), "--project", str(project),
                             "--profile", args.profile]
                if descriptor.get("RuntimeMode") == "Scene":
                    preflight.append("--precompiled-player")
                subprocess.run(preflight, check=True, stdout=log, stderr=subprocess.STDOUT)
                if args.action == "Build":
                    command = [sys.executable, str(repo / "Tools/macOS/hectl.py"), "package", "-Project", str(project),
                               "--build-directory", str(work / "build"),
                               "--cmake-arg=-DHE_MACOS_PACKAGE_OUTPUT=" + str(output)]
                    for key, variable in (("gameConfig", "HE_PACKAGE_GAME_CONFIG"), ("pluginConfig", "HE_PLUGIN_CONFIG")):
                        value = profile.get(key)
                        if value:
                            if key == "pluginConfig":
                                value = str((project / value).resolve())
                            command.append("--cmake-arg=-D" + variable + "=" + value)
                    subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
                log.write("macOS " + args.action + " PASS\n")
            receipt.update(success=True, status="Succeeded")
        except BaseException as error:
            receipt.update(status="Failed", error=str(error))
            raise
        finally:
            receipt["completed"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
            save()
    print("macOS " + args.action + " PASS: " + str(output))


if __name__ == "__main__":
    main()
