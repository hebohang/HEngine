"""Configure a freshly created Scripted3D project for independent integration QA.

First create with hectl project create -Template Scripted3D. Only the expected
template Player and scene are changed. No MechanicGirlLab content is consumed.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    args = parser.parse_args()
    root = args.project.resolve()
    scene_path = root / "Assets/Scenes/Main.he"
    scene = json.loads(scene_path.read_text(encoding="utf-8-sig"))
    player = scene["3a6c5e51-8da7-4c11-9099-a0987f5ec261"]
    if player["ScriptComponent"]["Type"] != "template.player":
        raise RuntimeError("Expected an unmodified Scripted3D template")
    player["ScriptComponent"]["Type"] = "diagnostics.fsr-portability"
    player["TransformComponent"]["Translation"] = [-3, .5, 0]
    floor = scene["4a6c5e51-8da7-4c11-9099-a0987f5ec261"]
    floor["MeshComponent"]["MaterialOverrides"] = ["Assets/Portability/Checker.hmat"]
    scene_path.write_text(json.dumps(scene, indent=2), encoding="utf-8")
    shutil.copyfile(Path(__file__).parent / "Fixtures/FsrPortability/Portability.cs",
                    root / "Scripts/CSharp/Player.cs")
    navigation = {"Version": 1, "CellSize": .2, "CellHeight": .1,
                  "AgentHeight": 1, "AgentRadius": .25, "AgentMaxClimb": .3,
                  "AgentMaxSlope": 45, "MaxGridCells": 65536,
                  "Surfaces": [{"TagPrefix": "Floor", "Area": 0, "UseMesh": True}],
                  "AreaCosts": []}
    (root / "Config/Navigation.json").write_text(json.dumps(navigation, indent=2), encoding="utf-8")
    assets = root / "Assets/Portability"
    assets.mkdir()
    # Authored calibration texture, generated directly; runtime builds its mip chain.
    width = 2048
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, width, width, 32, 0x28)
    colors = (bytes((180, 90, 25, 255)), bytes((40, 180, 220, 255)))
    with (assets / "Checker.tga").open("wb") as stream:
        stream.write(header)
        rows = [b"".join(colors[((x // 128) + parity) % 2] for x in range(width)) for parity in range(2)]
        for y in range(width):
            stream.write(rows[(y // 128) % 2])
    material = {"Type": "Default", "ShaderPath": "Shaders/BasePBR.hlsl", "Properties": {
        "name": "PortabilityChecker", "alphaMode": "OPAQUE", "bUseAlbedoMap": True,
        "ExposedProperties": {"g_AlbedoMap": "Assets/Portability/Checker.tga"},
        "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0, "roughnessFactor": .8,
            "baseColorTexture": {"texCoord": 0, "textureRes": {
                "textureSourcePath": "Assets/Portability/Checker.tga",
                "sampler": {"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}}}}}}
    (assets / "Checker.hmat").write_text(json.dumps(material, indent=2), encoding="utf-8")
    print(json.dumps({"project": str(root), "sceneEntities": len(scene), "textureSize": width,
                      "usesMechanicGirlLab": False}))


if __name__ == "__main__":
    main()
