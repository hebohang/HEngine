#!/usr/bin/env python3
"""Generate and verify the built-in 5x5 material showcase scene.

Run from any directory:
    python Tools/Content/generate_material_showcase.py
    python Tools/Content/generate_material_showcase.py --check
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
ENGINE_CONTENT_ROOT = REPOSITORY_ROOT / "Engine" / "Content"
ENGINE_COOK_PATH = ENGINE_CONTENT_ROOT / "Config" / "EngineCook.json"
SOURCE_MESH_PATH = (
    ENGINE_CONTENT_ROOT / "Assets" / "Models" / "MaterialPreview" / "MaterialBall.hmesh"
)
SOURCE_FLOOR_MESH_PATH = ENGINE_CONTENT_ROOT / "Assets" / "Models" / "Box" / "Box.hmesh"
SHOWCASE_SCENE_PATH = ENGINE_CONTENT_ROOT / "Assets" / "Scenes" / "MaterialShowcase.he"

SOURCE_MESH_LOGICAL_PATH = "Assets/Models/MaterialPreview/MaterialBall.hmesh"
SOURCE_BUFFER_LOGICAL_PATH = "Assets/Models/MaterialPreview/MaterialBall.bin"
NEUTRAL_MATERIAL_LOGICAL_PATH = "Assets/Models/MaterialPreview/shaderball.hmat"
SHOWCASE_MATERIAL_LOGICAL_DIRECTORY = "Assets/Materials/BuiltInShowcase"
SHOWCASE_SHADER_LOGICAL_DIRECTORY = "Shaders/Materials/BuiltInShowcase"
DEFAULT_HDR_LOGICAL_PATH = "Assets/Textures/Hdr/Default.hdr"
SOURCE_FLOOR_MESH_LOGICAL_PATH = "Assets/Models/Box/Box.hmesh"
SOURCE_FLOOR_BUFFER_LOGICAL_PATH = "Assets/Models/Box/Box.bin"
FLOOR_MATERIAL_LOGICAL_PATH = (
    f"{SHOWCASE_MATERIAL_LOGICAL_DIRECTORY}/MaterialShowcaseFloor.hmat"
)
FLOOR_SHADER_LOGICAL_PATH = f"{SHOWCASE_SHADER_LOGICAL_DIRECTORY}/ShowcaseFloor.hlsl"

GRID_SIZE = 5
GRID_SPACING = 3.5
EXPECTED_ENTITY_COUNT = GRID_SIZE * GRID_SIZE + 5
UUID_PREFIX = "https://henginedev.local/content/material-showcase/"
EXPECTED_SHADER_PATHS = {
    f"{SHOWCASE_SHADER_LOGICAL_DIRECTORY}/ShowcasePBR.hlsl",
    f"{SHOWCASE_SHADER_LOGICAL_DIRECTORY}/FresnelRim.hlsl",
    f"{SHOWCASE_SHADER_LOGICAL_DIRECTORY}/IridescentLaser.hlsl",
    f"{SHOWCASE_SHADER_LOGICAL_DIRECTORY}/ProceduralPattern.hlsl",
    FLOOR_SHADER_LOGICAL_PATH,
    f"{SHOWCASE_SHADER_LOGICAL_DIRECTORY}/HologramGlass.hlsl",
    f"{SHOWCASE_SHADER_LOGICAL_DIRECTORY}/MaskedDissolve.hlsl",
}


@dataclass(frozen=True)
class ShowcaseMaterial:
    code: str
    stem: str
    display_name: str
    category: str

    @property
    def material_filename(self) -> str:
        return f"{self.code}_{self.stem}.hmat"

    @property
    def material_logical_path(self) -> str:
        return f"{SHOWCASE_MATERIAL_LOGICAL_DIRECTORY}/{self.material_filename}"


SHOWCASE_MATERIALS = (
    ShowcaseMaterial("M01", "RubyPlastic", "Ruby Plastic", "Everyday Surfaces"),
    ShowcaseMaterial("M02", "IvoryPlastic", "Ivory Plastic", "Everyday Surfaces"),
    ShowcaseMaterial("M03", "MatteRubber", "Matte Rubber", "Everyday Surfaces"),
    ShowcaseMaterial("M04", "RoughCeramic", "Rough Ceramic", "Everyday Surfaces"),
    ShowcaseMaterial("M05", "GlazedCeramic", "Glazed Ceramic", "Everyday Surfaces"),
    ShowcaseMaterial("M06", "BrushedAluminum", "Brushed Aluminum", "Metals"),
    ShowcaseMaterial("M07", "PolishedSilver", "Polished Silver", "Metals"),
    ShowcaseMaterial("M08", "WarmGold", "Warm Gold", "Metals"),
    ShowcaseMaterial("M09", "AgedCopper", "Aged Copper", "Metals"),
    ShowcaseMaterial("M10", "Gunmetal", "Gunmetal", "Metals"),
    ShowcaseMaterial("M11", "Jade", "Jade", "Detailed and Emissive"),
    ShowcaseMaterial("M12", "RubyGem", "Ruby Gem", "Detailed and Emissive"),
    ShowcaseMaterial("M13", "SapphireGem", "Sapphire Gem", "Detailed and Emissive"),
    ShowcaseMaterial("M14", "NormalDetail", "Normal Detail", "Detailed and Emissive"),
    ShowcaseMaterial("M15", "EmissiveLava", "Emissive Lava", "Detailed and Emissive"),
    ShowcaseMaterial("M16", "FresnelAzure", "Fresnel Azure", "Stylized Optics"),
    ShowcaseMaterial("M17", "FresnelMagenta", "Fresnel Magenta", "Stylized Optics"),
    ShowcaseMaterial("M18", "IridescentPearl", "Iridescent Pearl", "Stylized Optics"),
    ShowcaseMaterial("M19", "LaserChrome", "Laser Chrome", "Stylized Optics"),
    ShowcaseMaterial("M20", "ProceduralChecker", "Procedural Checker", "Stylized Optics"),
    ShowcaseMaterial("M21", "ToonBands", "Toon Bands", "Special Rendering"),
    ShowcaseMaterial("M22", "DoubleSidedStripes", "Double-Sided Stripes", "Special Rendering"),
    ShowcaseMaterial("M23", "ClearGlass", "Clear Glass", "Special Rendering"),
    ShowcaseMaterial("M24", "HologramScan", "Hologram Scan", "Special Rendering"),
    ShowcaseMaterial("M25", "MaskedDissolve", "Masked Dissolve", "Special Rendering"),
)


def _parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Verify checked-in outputs and all referenced engine assets without writing files.",
    )
    parser.add_argument(
        "--manifest-root",
        type=Path,
        help="Also validate published manifests and DXIL under this generated Shader output root.",
    )
    return parser.parse_args()


def _load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as source:
        value = json.load(source)
    if not isinstance(value, dict):
        raise ValueError(f"Expected a JSON object: {path}")
    return value


def _json_bytes(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=4, sort_keys=True) + "\n").encode("utf-8")


def _validate_logical_content_path(
    path: str,
    required_prefix: str,
    suffix: str,
) -> None:
    if not path.startswith(required_prefix):
        raise ValueError(f"Content path must start with {required_prefix}: {path}")
    if "\\" in path or "//" in path or ":" in path or "\x00" in path:
        raise ValueError(f"Content path must be a normalized project-relative path: {path}")
    path_parts = Path(path).parts
    if Path(path).is_absolute() or ".." in path_parts or "." in path_parts:
        raise ValueError(f"Content path must remain relative to a content root: {path}")
    if not path.casefold().endswith(suffix.casefold()):
        raise ValueError(f"Content path must end with {suffix}: {path}")


def _validate_logical_asset_path(path: str, suffix: str) -> None:
    _validate_logical_content_path(path, "Assets/", suffix)


def _validate_logical_shader_path(path: str) -> None:
    _validate_logical_content_path(path, f"{SHOWCASE_SHADER_LOGICAL_DIRECTORY}/", ".hlsl")


def _resolve_engine_asset(logical_path: str) -> Path:
    _validate_logical_asset_path(logical_path, Path(logical_path).suffix)
    candidate = (ENGINE_CONTENT_ROOT / Path(logical_path)).resolve()
    try:
        candidate.relative_to(ENGINE_CONTENT_ROOT.resolve())
    except ValueError as exception:
        raise ValueError(f"Asset path escaped Engine/Content: {logical_path}") from exception
    return candidate


def _resolve_engine_shader(logical_path: str) -> Path:
    _validate_logical_shader_path(logical_path)
    candidate = (ENGINE_CONTENT_ROOT / Path(logical_path)).resolve()
    try:
        candidate.relative_to(ENGINE_CONTENT_ROOT.resolve())
    except ValueError as exception:
        raise ValueError(f"Shader path escaped Engine/Content: {logical_path}") from exception
    return candidate


def _validate_material_document(
    material: ShowcaseMaterial,
    document: dict[str, Any],
) -> tuple[list[str], str | None]:
    context = material.material_logical_path
    errors: list[str] = []

    def require(condition: bool, message: str) -> None:
        if not condition:
            errors.append(f"invalid showcase material {context}: {message}")

    require(document.get("SchemaVersion") == 2, "SchemaVersion must be 2")
    require(document.get("Type") == "Custom", "Type must be Custom")
    require(document.get("Domain") == "Surface", "Domain must be Surface")
    require(
        document.get("ShadingModel") in {"Lit", "Unlit"},
        "ShadingModel must be Lit or Unlit",
    )

    shader_path = document.get("ShaderPath")
    pass_name = document.get("PassName")
    require(isinstance(shader_path, str) and bool(shader_path), "ShaderPath must be a string")
    require(isinstance(pass_name, str) and bool(pass_name), "PassName must be a string")
    if isinstance(shader_path, str) and shader_path:
        try:
            _validate_logical_shader_path(shader_path)
        except ValueError as exception:
            errors.append(f"invalid showcase material {context}: {exception}")
        else:
            require(shader_path in EXPECTED_SHADER_PATHS, "ShaderPath is outside the showcase built-ins")
            require(
                isinstance(pass_name, str) and pass_name == Path(shader_path).stem,
                "PassName must match the ShaderPath stem",
            )
            require(_resolve_engine_shader(shader_path).is_file(), "Shader source is missing")

    variants = document.get("VariantKeywords")
    require(
        isinstance(variants, list) and all(isinstance(value, str) for value in variants),
        "VariantKeywords must be a string array",
    )

    blend_mode = document.get("BlendMode")
    cull_mode = document.get("CullMode")
    depth_write = document.get("DepthWrite")
    require(blend_mode in {"Opaque", "Mask", "Transparent"}, "BlendMode is invalid")
    require(cull_mode in {"Back", "None"}, "CullMode is invalid")
    require(isinstance(depth_write, bool), "DepthWrite must be a bool")
    if blend_mode in {"Opaque", "Mask", "Transparent"} and isinstance(depth_write, bool):
        require(
            depth_write == (blend_mode != "Transparent"),
            "DepthWrite must be disabled only for Transparent",
        )

    alpha_cutoff = document.get("AlphaCutoff")
    require(
        isinstance(alpha_cutoff, (int, float))
        and not isinstance(alpha_cutoff, bool)
        and math.isfinite(float(alpha_cutoff))
        and 0.0 <= float(alpha_cutoff) <= 1.0,
        "AlphaCutoff must be finite and in [0, 1]",
    )

    parameters = document.get("Parameters")
    textures = document.get("Textures")
    orphaned = document.get("OrphanedParameters")
    properties = document.get("Properties")
    require(isinstance(parameters, dict), "Parameters must be an object")
    require(isinstance(textures, dict), "Textures must be an object")
    require(isinstance(orphaned, dict), "OrphanedParameters must be an object")
    require(isinstance(properties, dict), "Properties must be an object")

    supported_parameter_types = {"float", "float2", "float3", "color", "bool", "int"}
    if isinstance(parameters, dict):
        for name, parameter in parameters.items():
            require(isinstance(name, str) and bool(name), "parameter names must be non-empty")
            require(isinstance(parameter, dict), f"parameter {name} must be an object")
            if not isinstance(parameter, dict):
                continue
            require(
                isinstance(parameter.get("StableId"), str) and bool(parameter["StableId"]),
                f"parameter {name} must have a StableId",
            )
            require(
                parameter.get("Type") in supported_parameter_types,
                f"parameter {name} has an unsupported Type",
            )
            require("Value" in parameter, f"parameter {name} must have a Value")

    supported_semantics = {
        "None",
        "BaseColor",
        "Normal",
        "MetallicRoughness",
        "Occlusion",
        "Emissive",
        "Data",
    }
    if isinstance(textures, dict):
        for name, texture in textures.items():
            require(isinstance(name, str) and bool(name), "texture names must be non-empty")
            require(isinstance(texture, dict), f"texture {name} must be an object")
            if not isinstance(texture, dict):
                continue
            require(
                isinstance(texture.get("StableId"), str) and bool(texture["StableId"]),
                f"texture {name} must have a StableId",
            )
            require(texture.get("Type") == "texture2d", f"texture {name} Type must be texture2d")
            texture_path = texture.get("Path")
            require(isinstance(texture_path, str), f"texture {name} Path must be a string")
            if isinstance(texture_path, str) and texture_path:
                try:
                    _validate_logical_asset_path(texture_path, Path(texture_path).suffix)
                except ValueError as exception:
                    errors.append(f"invalid showcase material {context}: texture {name}: {exception}")
                else:
                    require(_resolve_engine_asset(texture_path).is_file(), f"texture {name} is missing")
            require(
                texture.get("Fallback") in {"white", "black", "normal"},
                f"texture {name} has an invalid Fallback",
            )
            require(
                texture.get("ColorSpace") in {"Linear", "SRGB"},
                f"texture {name} has an invalid ColorSpace",
            )
            require(
                texture.get("Semantic") in supported_semantics,
                f"texture {name} has an invalid Semantic",
            )

    return errors, shader_path if isinstance(shader_path, str) else None


def _validate_catalog() -> None:
    if len(SHOWCASE_MATERIALS) != GRID_SIZE * GRID_SIZE:
        raise ValueError("The material catalog must contain exactly 25 entries.")

    expected_codes = [f"M{index:02d}" for index in range(1, GRID_SIZE * GRID_SIZE + 1)]
    actual_codes = [entry.code for entry in SHOWCASE_MATERIALS]
    if actual_codes != expected_codes:
        raise ValueError(f"Material codes must be contiguous row-major M01-M25: {actual_codes}")

    if len({entry.material_filename.casefold() for entry in SHOWCASE_MATERIALS}) != len(
        SHOWCASE_MATERIALS
    ):
        raise ValueError("Material filenames must be unique under Windows case-folding rules.")
    for row in range(GRID_SIZE):
        row_entries = SHOWCASE_MATERIALS[row * GRID_SIZE : (row + 1) * GRID_SIZE]
        if len({entry.category for entry in row_entries}) != 1:
            raise ValueError(f"Showcase row {row + 1} must contain a single material category.")

    for entry in SHOWCASE_MATERIALS:
        _validate_logical_asset_path(entry.material_logical_path, ".hmat")


def _validate_source_mesh(source_mesh: dict[str, Any]) -> None:
    if source_mesh.get("version") != 1:
        raise ValueError("MaterialBall.hmesh must use mesh schema version 1.")

    buffers = source_mesh.get("bufferVec")
    if not isinstance(buffers, list) or len(buffers) != 1:
        raise ValueError("MaterialBall.hmesh must contain exactly one shared buffer.")
    if buffers[0].get("uri") != SOURCE_BUFFER_LOGICAL_PATH:
        raise ValueError(
            "MaterialBall.hmesh must retain its canonical shared buffer path "
            f"'{SOURCE_BUFFER_LOGICAL_PATH}'."
        )

    primitives = source_mesh.get("primitives")
    if not isinstance(primitives, list) or len(primitives) != 2:
        raise ValueError("MaterialBall.hmesh must expose preview surface slot 0 and neutral slot 1.")
    if primitives[1].get("materialResPath") != NEUTRAL_MATERIAL_LOGICAL_PATH:
        raise ValueError(
            "MaterialBall.hmesh primitive 1 must retain the neutral shader-ball material."
        )
    for index, primitive in enumerate(primitives):
        if not isinstance(primitive, dict) or not isinstance(primitive.get("attributes"), dict):
            raise ValueError(f"MaterialBall.hmesh primitive {index} is malformed.")
        if not isinstance(primitive.get("materialResPath"), str):
            raise ValueError(f"MaterialBall.hmesh primitive {index} has no material path.")

    for required_key in ("accessorVec", "bufferViewVec"):
        if not isinstance(source_mesh.get(required_key), list) or not source_mesh[required_key]:
            raise ValueError(f"MaterialBall.hmesh is missing {required_key}.")

    if not _resolve_engine_asset(SOURCE_BUFFER_LOGICAL_PATH).is_file():
        raise FileNotFoundError(f"Shared material-ball buffer is missing: {SOURCE_BUFFER_LOGICAL_PATH}")
    if not _resolve_engine_asset(NEUTRAL_MATERIAL_LOGICAL_PATH).is_file():
        raise FileNotFoundError(
            f"Neutral material-ball material is missing: {NEUTRAL_MATERIAL_LOGICAL_PATH}"
        )


def _validate_source_floor_mesh(source_floor_mesh: dict[str, Any]) -> None:
    if source_floor_mesh.get("version") != 1:
        raise ValueError("Box.hmesh must use mesh schema version 1.")
    buffers = source_floor_mesh.get("bufferVec")
    if not isinstance(buffers, list) or len(buffers) != 1:
        raise ValueError("Box.hmesh must contain exactly one shared buffer.")
    buffer_path = buffers[0].get("uri")
    if not isinstance(buffer_path, str) or buffer_path.replace("\\", "/") != (
        SOURCE_FLOOR_BUFFER_LOGICAL_PATH
    ):
        raise ValueError("Box.hmesh must retain its canonical Box.bin buffer path.")
    primitives = source_floor_mesh.get("primitives")
    if not isinstance(primitives, list) or len(primitives) != 1:
        raise ValueError("Box.hmesh must contain exactly one floor primitive.")
    if not isinstance(primitives[0], dict) or not isinstance(primitives[0].get("attributes"), dict):
        raise ValueError("Box.hmesh floor primitive is malformed.")
    if not _resolve_engine_asset(SOURCE_FLOOR_BUFFER_LOGICAL_PATH).is_file():
        raise FileNotFoundError(
            f"Shared floor buffer is missing: {SOURCE_FLOOR_BUFFER_LOGICAL_PATH}"
        )


def _entity_uuid(name: str) -> str:
    return str(uuid.uuid5(uuid.NAMESPACE_URL, UUID_PREFIX + name))


def _transform(
    translation: tuple[float, float, float],
    rotation: tuple[float, float, float] = (0.0, 0.0, 0.0),
    scale: tuple[float, float, float] = (1.0, 1.0, 1.0),
) -> dict[str, Any]:
    return {
        "ParentUuid": "",
        "Rotation": list(rotation),
        "Scale": list(scale),
        "Translation": list(translation),
    }


def _validate_scene_contract(scene: dict[str, Any]) -> None:
    if len(scene) != EXPECTED_ENTITY_COUNT:
        raise ValueError(f"Expected {EXPECTED_ENTITY_COUNT} scene entities, got {len(scene)}.")

    primary_cameras = 0
    environment_lights = 0
    directional_lights = 0
    point_lights = 0
    tags: dict[str, dict[str, Any]] = {}
    for entity_uuid, entity in scene.items():
        parsed_uuid = uuid.UUID(entity_uuid)
        if parsed_uuid.version != 5:
            raise ValueError(f"Showcase entity UUID must be deterministic version 5: {entity_uuid}")
        if not isinstance(entity, dict):
            raise ValueError(f"Showcase entity must be an object: {entity_uuid}")

        tag_component = entity.get("TagComponent")
        tag = tag_component.get("Tag") if isinstance(tag_component, dict) else None
        if not isinstance(tag, str) or not tag:
            raise ValueError(f"Showcase entity must have a non-empty tag: {entity_uuid}")
        if tag in tags:
            raise ValueError(f"Showcase entity tags must be unique: {tag}")
        tags[tag] = entity

        camera = entity.get("CameraComponent")
        if isinstance(camera, dict) and camera.get("Primary") is True:
            primary_cameras += 1
        environment_lights += int("EnvironmentLightComponent" in entity)
        directional_lights += int("DirectionalLightComponent" in entity)
        point_lights += int("PointLightComponent" in entity)

    if (primary_cameras, environment_lights, directional_lights, point_lights) != (1, 1, 1, 1):
        raise ValueError(
            "Showcase scene must contain one primary camera, one environment light, "
            "one directional light and one point light."
        )

    key_light = tags.get("Material Showcase Key Light")
    key_transform = key_light.get("TransformComponent") if isinstance(key_light, dict) else None
    key_rotation = key_transform.get("Rotation") if isinstance(key_transform, dict) else None
    if (
        not isinstance(key_rotation, list)
        or len(key_rotation) != 3
        or not (-math.pi < key_rotation[0] < -math.pi * 0.5)
    ):
        raise ValueError("Showcase key light must point down toward the material grid.")

    for index, material in enumerate(SHOWCASE_MATERIALS):
        expected_tag = f"{material.code} - {material.display_name}"
        entity = tags.get(expected_tag)
        if entity is None:
            raise ValueError(f"Showcase scene is missing material entity: {expected_tag}")
        mesh_component = entity.get("MeshComponent")
        if not isinstance(mesh_component, dict):
            raise ValueError(f"Showcase material entity has no mesh component: {expected_tag}")
        if (
            mesh_component.get("Path") != SOURCE_MESH_LOGICAL_PATH
            or mesh_component.get("meshResPath") != SOURCE_MESH_LOGICAL_PATH
            or mesh_component.get("MaterialOverrides") != [material.material_logical_path]
        ):
            raise ValueError(
                f"Showcase material entity must share MaterialBall.hmesh and override slot 0: {expected_tag}"
            )

        row = index // GRID_SIZE
        column = index % GRID_SIZE
        expected_translation = [
            (column - (GRID_SIZE - 1) * 0.5) * GRID_SPACING,
            0.0,
            (row - (GRID_SIZE - 1) * 0.5) * GRID_SPACING,
        ]
        transform = entity.get("TransformComponent")
        if not isinstance(transform, dict) or transform.get("Translation") != expected_translation:
            raise ValueError(f"Showcase material entity is outside its row-major grid slot: {expected_tag}")

    floor = tags.get("Material Showcase Floor")
    floor_mesh = floor.get("MeshComponent") if isinstance(floor, dict) else None
    if (
        not isinstance(floor_mesh, dict)
        or floor_mesh.get("Path") != SOURCE_FLOOR_MESH_LOGICAL_PATH
        or floor_mesh.get("meshResPath") != SOURCE_FLOOR_MESH_LOGICAL_PATH
        or floor_mesh.get("MaterialOverrides") != [FLOOR_MATERIAL_LOGICAL_PATH]
    ):
        raise ValueError("Showcase floor must share Box.hmesh and override material slot 0.")


def _build_scene() -> dict[str, Any]:
    scene: dict[str, Any] = {}

    scene[_entity_uuid("camera")] = {
        "CameraComponent": {
            "Camera": {
                "mAspectRatio": 16.0 / 9.0,
                "mOrthographicFar": 1.0,
                "mOrthographicNear": -1.0,
                "mOrthographicSize": 10.0,
                "mPerspectiveFOV": math.radians(28.0),
                "mPerspectiveFar": 120.0,
                "mPerspectiveNear": 0.1,
                "mProjectionType": 0,
            },
            "FixedAspectRatio": False,
            "Primary": True,
        },
        "TagComponent": {"Tag": "Material Showcase Camera"},
        "TransformComponent": _transform(
            (0.0, 13.0, 32.0),
            (math.radians(-21.0), 0.0, 0.0),
        ),
    }

    scene[_entity_uuid("environment-light")] = {
        "EnvironmentLightComponent": {
            "ExposureEV100": 0.286304185156641,
            "ExposureCompensation": 0.0,
            "HdrTexturePath": DEFAULT_HDR_LOGICAL_PATH,
            "IBLIntensity": 1.15,
        },
        "TagComponent": {"Tag": "Material Showcase Environment"},
        "TransformComponent": _transform((0.0, 0.0, 0.0)),
    }

    scene[_entity_uuid("directional-light")] = {
        "DirectionalLightComponent": {
            "CascadeCount": 4,
            "CascadeLambda": 0.75,
            "Color": [1.0, 0.96, 0.88],
            "DepthBias": 0.0015,
            "Intensity": 2.8,
            "NormalBias": 0.05,
            "PCFRadius": 1.5,
            "ShadowDistance": 80.0,
            "ShadowMapSize": 2048,
            "ShadowStrength": 1.0,
            "bCastShadows": True,
            "bUsePCF": True,
        },
        "TagComponent": {"Tag": "Material Showcase Key Light"},
        "TransformComponent": _transform(
            (0.0, 8.0, 0.0),
            (math.radians(-132.0), math.radians(-35.0), math.radians(-20.0)),
        ),
    }

    scene[_entity_uuid("point-light")] = {
        "PointLightComponent": {
            "IntensityUnits": 1,
            "Intensity": 5.0,
            "LightColor": [0.38, 0.72, 1.0],
            "Radius": 28.0,
        },
        "TagComponent": {"Tag": "Material Showcase Fill Light"},
        "TransformComponent": _transform((-7.0, 7.0, 10.0)),
    }

    scene[_entity_uuid("floor")] = {
        "MeshComponent": {
            "Path": SOURCE_FLOOR_MESH_LOGICAL_PATH,
            "meshResPath": SOURCE_FLOOR_MESH_LOGICAL_PATH,
            "MaterialOverrides": [FLOOR_MATERIAL_LOGICAL_PATH],
        },
        "TagComponent": {"Tag": "Material Showcase Floor"},
        "TransformComponent": _transform(
            (0.0, -0.1, 0.0),
            scale=(80.0, 0.2, 80.0),
        ),
    }

    for index, material in enumerate(SHOWCASE_MATERIALS):
        row = index // GRID_SIZE
        column = index % GRID_SIZE
        x = (column - (GRID_SIZE - 1) * 0.5) * GRID_SPACING
        z = (row - (GRID_SIZE - 1) * 0.5) * GRID_SPACING
        scene[_entity_uuid(material.code.casefold())] = {
            "MeshComponent": {
                "Path": SOURCE_MESH_LOGICAL_PATH,
                "meshResPath": SOURCE_MESH_LOGICAL_PATH,
                "MaterialOverrides": [material.material_logical_path],
            },
            "TagComponent": {"Tag": f"{material.code} - {material.display_name}"},
            "TransformComponent": _transform((x, 0.0, z)),
        }

    _validate_scene_contract(scene)
    return scene


def _build_expected_outputs() -> dict[Path, bytes]:
    return {SHOWCASE_SCENE_PATH: _json_bytes(_build_scene())}


def _validate_compiled_manifests(manifest_root: Path) -> list[str]:
    errors: list[str] = []
    generated_root = manifest_root.resolve()
    caches_root = (generated_root / "CACHES").resolve()
    if not caches_root.is_dir():
        return [f"compiled shader cache root is missing: {caches_root}"]

    material_paths = [material.material_logical_path for material in SHOWCASE_MATERIALS]
    material_paths.append(FLOOR_MATERIAL_LOGICAL_PATH)
    type_aliases = {
        "float": "float",
        "float2": "float2",
        "float3": "float3",
        "color": "color",
        "float4": "color",
        "bool": "bool",
        "int": "int",
        "texture": "texture2d",
        "texture2d": "texture2d",
    }

    for material_path in material_paths:
        document = _load_json(_resolve_engine_asset(material_path))
        shader_path = document.get("ShaderPath")
        if not isinstance(shader_path, str) or not shader_path.startswith("Shaders/"):
            errors.append(f"{material_path}: invalid ShaderPath for manifest validation")
            continue
        source_path = shader_path[len("Shaders/") :]
        manifest_path = caches_root / "Materials" / Path(source_path + ".material.json")
        if not manifest_path.is_file():
            errors.append(f"{material_path}: missing compiled manifest {manifest_path}")
            continue
        try:
            manifest = _load_json(manifest_path)
        except (OSError, ValueError, json.JSONDecodeError) as exception:
            errors.append(f"{material_path}: invalid compiled manifest: {exception}")
            continue

        for material_field, manifest_field in (
            ("Domain", "Domain"),
            ("ShadingModel", "ShadingModel"),
            ("BlendMode", "BlendMode"),
            ("CullMode", "CullMode"),
            ("DepthWrite", "DepthWrite"),
        ):
            if document.get(material_field) != manifest.get(manifest_field):
                errors.append(
                    f"{material_path}: {material_field} disagrees with compiled manifest"
                )
        if manifest.get("SourcePath") != source_path:
            errors.append(f"{material_path}: manifest SourcePath does not match ShaderPath")

        serialized_entries: dict[str, tuple[str, dict[str, Any]]] = {}
        for collection_name in ("Parameters", "Textures"):
            collection = document.get(collection_name, {})
            if not isinstance(collection, dict):
                continue
            for name, entry in collection.items():
                if not isinstance(entry, dict):
                    continue
                stable_id = entry.get("StableId")
                if not isinstance(stable_id, str) or not stable_id:
                    continue
                if stable_id in serialized_entries:
                    errors.append(f"{material_path}: duplicate serialized StableId {stable_id}")
                    continue
                serialized_entries[stable_id] = (name, entry)

        manifest_properties = manifest.get("Properties")
        if not isinstance(manifest_properties, list):
            errors.append(f"{material_path}: manifest Properties must be an array")
            continue
        expected_stable_ids: set[str] = set()
        for property_document in manifest_properties:
            if not isinstance(property_document, dict):
                continue
            stable_id = property_document.get("StableId")
            property_name = property_document.get("Name")
            if not isinstance(stable_id, str) or not stable_id:
                continue
            expected_stable_ids.add(stable_id)
            serialized = serialized_entries.get(stable_id)
            if serialized is None:
                errors.append(f"{material_path}: missing property StableId {stable_id}")
                continue
            serialized_name, serialized_entry = serialized
            if serialized_name != property_name:
                errors.append(
                    f"{material_path}: property {stable_id} name {serialized_name} != {property_name}"
                )
            serialized_type = type_aliases.get(str(serialized_entry.get("Type", "")).casefold())
            manifest_type = type_aliases.get(str(property_document.get("Type", "")).casefold())
            if serialized_type != manifest_type:
                errors.append(f"{material_path}: property {stable_id} type mismatch")
        unexpected_stable_ids = sorted(set(serialized_entries) - expected_stable_ids)
        for stable_id in unexpected_stable_ids:
            errors.append(f"{material_path}: unexpected serialized StableId {stable_id}")

        diagnostics = manifest.get("Diagnostics", [])
        if isinstance(diagnostics, list):
            for diagnostic in diagnostics:
                if isinstance(diagnostic, dict) and diagnostic.get("Severity") == "Error":
                    errors.append(
                        f"{material_path}: manifest error {diagnostic.get('Code', 'unknown')}"
                    )

        artifacts = manifest.get("Artifacts")
        if not isinstance(artifacts, list) or not artifacts:
            errors.append(f"{material_path}: compiled manifest has no artifacts")
            continue
        for artifact in artifacts:
            if not isinstance(artifact, dict):
                errors.append(f"{material_path}: malformed manifest artifact")
                continue
            relative_path = artifact.get("RelativePath")
            expected_hash = artifact.get("ContentHash")
            if not isinstance(relative_path, str) or not isinstance(expected_hash, str):
                errors.append(f"{material_path}: artifact identity is incomplete")
                continue
            artifact_path = (caches_root / Path(relative_path)).resolve()
            try:
                artifact_path.relative_to(caches_root)
            except ValueError:
                errors.append(f"{material_path}: artifact escaped generated cache root")
                continue
            if not artifact_path.is_file():
                errors.append(f"{material_path}: compiled artifact is missing: {relative_path}")
                continue
            actual_hash = hashlib.sha256(artifact_path.read_bytes()).hexdigest()
            if actual_hash.casefold() != expected_hash.casefold():
                errors.append(f"{material_path}: artifact SHA-256 mismatch: {relative_path}")
    return errors


def _validate_references(manifest_root: Path | None = None) -> list[str]:
    errors: list[str] = []
    try:
        engine_cook = _load_json(ENGINE_COOK_PATH)
        root_files = engine_cook.get("RootFiles")
        if not isinstance(root_files, list) or "Assets/Scenes/MaterialShowcase.he" not in root_files:
            errors.append("EngineCook RootFiles must retain Assets/Scenes/MaterialShowcase.he")
    except (OSError, ValueError, json.JSONDecodeError) as exception:
        errors.append(f"invalid EngineCook config: {exception}")

    for logical_path in (
        SOURCE_MESH_LOGICAL_PATH,
        SOURCE_BUFFER_LOGICAL_PATH,
        NEUTRAL_MATERIAL_LOGICAL_PATH,
        FLOOR_MATERIAL_LOGICAL_PATH,
        DEFAULT_HDR_LOGICAL_PATH,
        SOURCE_FLOOR_MESH_LOGICAL_PATH,
        SOURCE_FLOOR_BUFFER_LOGICAL_PATH,
    ):
        if not _resolve_engine_asset(logical_path).is_file():
            errors.append(f"missing engine asset: {logical_path}")

    observed_shader_paths: set[str] = set()
    for material in SHOWCASE_MATERIALS:
        material_path = _resolve_engine_asset(material.material_logical_path)
        if not material_path.is_file():
            errors.append(f"missing showcase material: {material.material_logical_path}")
            continue
        try:
            document = _load_json(material_path)
        except (OSError, ValueError, json.JSONDecodeError) as exception:
            errors.append(f"invalid showcase material {material.material_logical_path}: {exception}")
            continue
        document_errors, shader_path = _validate_material_document(material, document)
        errors.extend(document_errors)
        if shader_path:
            observed_shader_paths.add(shader_path)

    floor_material_path = _resolve_engine_asset(FLOOR_MATERIAL_LOGICAL_PATH)
    try:
        floor_document = _load_json(floor_material_path)
    except (OSError, ValueError, json.JSONDecodeError) as exception:
        errors.append(f"invalid showcase floor material {FLOOR_MATERIAL_LOGICAL_PATH}: {exception}")
    else:
        floor_shader_path = floor_document.get("ShaderPath")
        if floor_document.get("SchemaVersion") != 2:
            errors.append("showcase floor material SchemaVersion must be 2")
        if floor_document.get("Type") != "Custom":
            errors.append("showcase floor material Type must be Custom")
        if floor_document.get("Domain") != "Surface" or floor_document.get("ShadingModel") != "Lit":
            errors.append("showcase floor material must be a Lit Surface material")
        if floor_document.get("PassName") != Path(FLOOR_SHADER_LOGICAL_PATH).stem:
            errors.append("showcase floor material PassName must match its ShaderPath stem")
        if floor_shader_path != FLOOR_SHADER_LOGICAL_PATH:
            errors.append("showcase floor material must use the dedicated ShowcaseFloor shader")
        elif not _resolve_engine_shader(floor_shader_path).is_file():
            errors.append("showcase floor shader source is missing")
        else:
            observed_shader_paths.add(floor_shader_path)

    if observed_shader_paths != EXPECTED_SHADER_PATHS:
        missing = sorted(EXPECTED_SHADER_PATHS - observed_shader_paths)
        unexpected = sorted(observed_shader_paths - EXPECTED_SHADER_PATHS)
        if missing:
            errors.append("showcase material catalog does not use shaders: " + ", ".join(missing))
        if unexpected:
            errors.append("showcase material catalog uses unexpected shaders: " + ", ".join(unexpected))

    material_directory = _resolve_engine_asset(f"{SHOWCASE_MATERIAL_LOGICAL_DIRECTORY}/catalog.hmat").parent
    if material_directory.is_dir():
        expected_names = {
            *(material.material_filename.casefold() for material in SHOWCASE_MATERIALS),
            Path(FLOOR_MATERIAL_LOGICAL_PATH).name.casefold(),
        }
        unexpected_materials = sorted(
            path.name
            for path in material_directory.glob("*.hmat")
            if path.name.casefold() not in expected_names
        )
        errors.extend(
            f"unexpected showcase material: {name}" for name in unexpected_materials
        )

    shader_directory = ENGINE_CONTENT_ROOT / Path(SHOWCASE_SHADER_LOGICAL_DIRECTORY)
    if shader_directory.is_dir():
        expected_shader_names = {Path(path).name.casefold() for path in EXPECTED_SHADER_PATHS}
        unexpected_shaders = sorted(
            path.name
            for path in shader_directory.glob("*.hlsl")
            if path.name.casefold() not in expected_shader_names
        )
        errors.extend(f"unexpected showcase shader: {name}" for name in unexpected_shaders)
    if manifest_root is not None:
        errors.extend(_validate_compiled_manifests(manifest_root))
    return errors


def _check_outputs(outputs: dict[Path, bytes], manifest_root: Path | None = None) -> int:
    errors = _validate_references(manifest_root)
    for output_path, expected_bytes in outputs.items():
        if not output_path.is_file():
            errors.append(f"missing generated output: {output_path.relative_to(REPOSITORY_ROOT)}")
            continue
        actual_bytes = output_path.read_bytes()
        if actual_bytes != expected_bytes:
            errors.append(
                "stale generated output: " + str(output_path.relative_to(REPOSITORY_ROOT))
            )

    legacy_mesh_directory = (
        ENGINE_CONTENT_ROOT / "Assets" / "Models" / "MaterialPreview" / "Showcase"
    )
    if legacy_mesh_directory.is_dir():
        obsolete_meshes = sorted(path.name for path in legacy_mesh_directory.glob("*.hmesh"))
        errors.extend(
            f"obsolete per-material mesh view must be removed: {name}"
            for name in obsolete_meshes
        )

    if errors:
        print("Material showcase contract: FAIL", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print(
        "Material showcase contract: PASS "
        f"({len(SHOWCASE_MATERIALS)} material balls, 1 shared material-ball mesh, "
        "1 distance-grid floor material, 1 shared floor mesh, "
        f"{EXPECTED_ENTITY_COUNT} scene entities)"
    )
    return 0


def _write_outputs(outputs: dict[Path, bytes]) -> int:
    written = 0
    unchanged = 0
    for output_path, content in outputs.items():
        if output_path.is_file() and output_path.read_bytes() == content:
            unchanged += 1
            continue
        output_path.parent.mkdir(parents=True, exist_ok=True)
        temporary_path = output_path.with_name(output_path.name + ".tmp")
        temporary_path.write_bytes(content)
        temporary_path.replace(output_path)
        written += 1

    print(f"Material showcase generation complete: {written} written, {unchanged} unchanged.")
    return 0


def main() -> int:
    arguments = _parse_arguments()
    if arguments.manifest_root is not None and not arguments.check:
        raise ValueError("--manifest-root is only valid together with --check")
    _validate_catalog()
    source_mesh = _load_json(SOURCE_MESH_PATH)
    _validate_source_mesh(source_mesh)
    source_floor_mesh = _load_json(SOURCE_FLOOR_MESH_PATH)
    _validate_source_floor_mesh(source_floor_mesh)
    outputs = _build_expected_outputs()
    if arguments.check:
        return _check_outputs(outputs, arguments.manifest_root)
    return _write_outputs(outputs)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as exception:
        print(f"Material showcase generation failed: {exception}", file=sys.stderr)
        raise SystemExit(1) from exception
