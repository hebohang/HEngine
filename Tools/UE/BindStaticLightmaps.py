"""Bind UE static-lightmap metadata to imported HE scene entities by actor label.

The UE bridge reports internal Actor names; glTF names are editor labels. This
tool resolves that distinction, checks transform identity and UV1 availability,
then writes a project-owned binding manifest. With --embed-scene it also writes
the reusable MeshComponent static-lightmap fields consumed by HEngine runtime.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("scene", type=Path, help="HE project-relative .he scene")
    parser.add_argument("inspection", type=Path, help="UE actor inspection JSON")
    parser.add_argument("bindings", type=Path, help="HELightmapBridge JSON")
    parser.add_argument("textures", type=Path, help="PackHqLightmapDds manifest")
    parser.add_argument("output", type=Path, help="HE project-relative binding manifest")
    parser.add_argument("--exclude-actor", action="append", default=[])
    parser.add_argument("--embed-scene", action="store_true")
    parser.add_argument("--blend", type=float, default=1.0,
                        help="0..1 weight of baked diffuse against environment diffuse")
    args = parser.parse_args()
    if not math.isfinite(args.blend) or not 0.0 <= args.blend <= 1.0:
        parser.error("--blend must be finite and between 0 and 1")
    root = args.project.resolve()
    scene_path = (root / args.scene).resolve()
    output_path = (root / args.output).resolve()
    if root not in scene_path.parents or root not in output_path.parents:
        raise ValueError("Scene and output must stay inside the HE project")
    scene = json.loads(scene_path.read_text(encoding="utf-8"))
    actors = json.loads(args.inspection.read_text(encoding="utf-8"))["actors"]
    ue_bindings = json.loads(args.bindings.read_text(encoding="utf-8"))["bindings"]
    atlas_assets = json.loads(args.textures.read_text(encoding="utf-8"))["textures"]
    labels = {actor["name"]: actor for actor in actors}
    tags = {}
    for entity_id, entity in scene.items():
        if "MeshComponent" in entity:
            tags.setdefault(entity.get("TagComponent", {}).get("Tag", ""), []).append((entity_id, entity))
    excluded = set(args.exclude_actor)
    entries = []
    skipped = []
    checked_meshes = set()
    seen = set()
    for binding in ue_bindings:
        if binding["lod"] != 0:
            continue  # The current UE glTF export imports LOD0 meshes only.
        actor_name = binding["actor"].rsplit(".", 1)[-1]
        if actor_name in excluded:
            skipped.append(actor_name)
            continue
        actor = labels.get(actor_name)
        if actor is None:
            raise ValueError("UE inspection lacks Actor " + actor_name)
        tag = actor.get("label", actor_name)
        matches = tags.get(tag, [])
        if len(matches) != 1:
            raise ValueError(f"Expected one HE mesh entity for UE Actor {actor_name} label {tag}; found {len(matches)}")
        entity_id, entity = matches[0]
        if entity_id in seen:
            raise ValueError("Multiple UE LOD0 bindings target one HE entity: " + entity_id)
        seen.add(entity_id)
        x, y, z = actor["location"]
        he_location = entity["TransformComponent"]["Translation"]
        expected = (x / 100.0, z / 100.0, y / 100.0)
        error = sum((actual - target) ** 2 for actual, target in zip(he_location, expected)) ** 0.5
        if error > 0.01:
            raise ValueError(f"UE/HE transform mismatch for {actor_name}: {error:.4f} m")
        if binding["lightmapUvChannel"] != 1:
            raise ValueError("Only imported UE lightmap UV1 is supported: " + actor_name)
        mesh_path = entity["MeshComponent"]["Path"]
        if mesh_path not in checked_meshes:
            mesh = json.loads((root / mesh_path).read_text(encoding="utf-8"))
            if any("TEXCOORD_1" not in section["attributes"] for section in mesh["primitives"]):
                raise ValueError("HE mesh lost UE lightmap UV1: " + mesh_path)
            checked_meshes.add(mesh_path)
        atlas = next((texture for texture in binding["textures"] if texture["basis"] == 0), None)
        if atlas is None or atlas["path"] not in atlas_assets:
            raise ValueError("Missing packed HQ lightmap for " + actor_name)
        asset_path = atlas_assets[atlas["path"]]["assetPath"]
        if not (root / asset_path).is_file():
            raise FileNotFoundError(root / asset_path)
        if len(binding.get("coefficientScales", [])) != 2 or len(binding.get("coefficientAdds", [])) != 2:
            raise ValueError("Incomplete UE HQ coefficients for " + actor_name)
        entries.append({
            "entityId": entity_id,
            "ueActor": actor_name,
            "tag": tag,
            "meshAssetPath": mesh_path,
            "atlasAssetPath": asset_path,
            "uvChannel": 1,
            "coordinateScale": binding["coordinateScale"],
            "coordinateBias": binding["coordinateBias"],
            "coefficientScales": binding["coefficientScales"],
            "coefficientAdds": binding["coefficientAdds"],
        })
    if set(skipped) != excluded:
        raise ValueError("Unused exclude-actor entries: " + repr(sorted(excluded - set(skipped))))
    if args.embed_scene:
        for entry in entries:
            mesh = scene[entry["entityId"]]["MeshComponent"]
            mesh["StaticLightmapPath"] = entry["atlasAssetPath"]
            mesh["StaticLightmapUvScale"] = entry["coordinateScale"]
            mesh["StaticLightmapUvBias"] = entry["coordinateBias"]
            mesh["StaticLightmapScale0"] = entry["coefficientScales"][0]
            mesh["StaticLightmapAdd0"] = entry["coefficientAdds"][0]
            mesh["StaticLightmapScale1"] = entry["coefficientScales"][1]
            mesh["StaticLightmapAdd1"] = entry["coefficientAdds"][1]
            mesh["StaticLightmapBlend"] = args.blend
        scene_text = json.dumps(scene, ensure_ascii=False, indent=2) + "\n"
        if scene_path.read_text(encoding="utf-8") != scene_text:
            scene_path.write_text(scene_text, encoding="utf-8")
    result = {
        "schema": "he.ue.static-lightmap-bindings.v1",
        "sceneSha256": digest(scene_path),
        "ueInspectionSha256": digest(args.inspection),
        "ueBindingsSha256": digest(args.bindings),
        "textureManifestSha256": digest(args.textures),
        "entries": sorted(entries, key=lambda row: row["entityId"]),
        "excludedActors": skipped,
        "embeddedScene": args.embed_scene,
        "blend": args.blend,
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"boundEntities": len(entries), "checkedMeshes": len(checked_meshes),
                      "excludedActors": skipped, "output": str(output_path)}))


if __name__ == "__main__":
    main()
