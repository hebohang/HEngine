"""Populate a newly created Scripted3D validation project with deterministic API probes."""
import argparse
import json
from pathlib import Path
import shutil
import wave


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--project", required=True, type=Path)
    args = parser.parse_args()
    project = args.project.resolve(strict=True)
    if (project / "Scripts/CSharp/ApiSmoke.cs").exists():
        raise FileExistsError("Use a fresh disposable validation project.")
    scene = project / "Assets/Scenes/Main.he"
    data = json.loads(scene.read_text(encoding="utf-8-sig"))
    players = [value for value in data.values() if value.get("ScriptComponent", {}).get("Type") == "template.player"]
    if len(players) != 1:
        raise ValueError("Expected a newly created Scripted3D project.")
    players[0]["ScriptComponent"]["Type"] = "example.api-smoke"
    scene.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    shutil.copyfile(args.engine / "Examples/ScriptingBasics/ApiSmoke.cs", project / "Scripts/CSharp/ApiSmoke.cs")
    audio = project / "Assets/Audio/Probe.wav"
    audio.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(audio), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(22050)
        output.writeframes(b"\0\0" * 22050)
    ui = project / "Assets/UI/Probe.rml"
    ui.parent.mkdir(parents=True, exist_ok=True)
    ui.write_text('<rml><head><title>Script SDK probe</title><style>body { font-family: Open Sans; font-size: 18px; color: #ffffff; } #result { position: absolute; left: 16px; top: 16px; width: 260px; padding: 12px; background-color: #202830; }</style></head><body><div id="result">Starting</div></body></rml>', encoding="utf-8")
    print(json.dumps({"project": str(project), "probe": "example.api-smoke"}))
