#!/usr/bin/env python3
"""Generate and validate the HEngine Cornell Box benchmark artifacts.

The checked-in JSON manifest is the only authored scene description. Generated
geometry is world-space in meters, has no node transform, and duplicates every
triangle vertex so geometric and shading normals are identical.

Run from any directory:
    python Tools/Content/Cornell/generate_cornell_scene.py
    python Tools/Content/Cornell/generate_cornell_scene.py --check
    python Tools/Content/Cornell/generate_cornell_scene.py --print-derived
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence


SCRIPT_PATH = Path(__file__).resolve()
REPOSITORY_ROOT = SCRIPT_PATH.parents[3]
DEFAULT_MANIFEST_PATH = SCRIPT_PATH.with_name("CornellSceneManifest.v1.json")
DEFAULT_OUTPUT_DIRECTORY = (
    REPOSITORY_ROOT / "Project" / "SourceAssets" / "CornellBox" / "Generated"
)

GLTF_FILENAME = "CornellBox.gltf"
BUFFER_FILENAME = "CornellBox.bin"
MEASURED_PBRT_FILENAME = "CornellBox_MeasuredSpectral.pbrt"
PARITY_PROXY_PBRT_FILENAME = "CornellBox_EngineParityRGB_SpectralProxy.pbrt"
# Kept as a source-compatibility alias for callers that previously referred to
# the single RGB-authored PBRT scene. It is intentionally a spectral proxy.
PARITY_PBRT_FILENAME = PARITY_PROXY_PBRT_FILENAME
PARITY_CHANNEL_PBRT_FILENAMES = {
    "R": "CornellBox_EngineParityRGB_R.pbrt",
    "G": "CornellBox_EngineParityRGB_G.pbrt",
    "B": "CornellBox_EngineParityRGB_B.pbrt",
}
MATERIAL_MASK_FILENAME = "CornellBox.material-id.pgm"
EDGE_MASK_FILENAME = "CornellBox.edge-mask.pgm"
ROI_MASK_FILENAME = "CornellBox.roi-mask.pgm"
ROI_METADATA_FILENAME = "CornellBox.roi.json"
DISOCCLUSION_MASK_FILENAME = "CornellBox.disocclusion-short-block-negx50mm.pgm"
DISOCCLUSION_METADATA_FILENAME = "CornellBox.disocclusion-short-block-negx50mm.json"
HENGINE_SCENE_FILENAME = "CornellBox.he"
OUTPUT_HASH_FILENAME = "CornellBox.outputs.sha256.json"

IDENTITY_MATRIX = [
    1.0, 0.0, 0.0, 0.0,
    0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0,
]


@dataclass(frozen=True)
class Face:
    name: str
    material: str
    normal_policy: str
    vertices: tuple[tuple[float, float, float], ...]


@dataclass(frozen=True)
class Triangle:
    surface: str
    material: str
    vertices: tuple[
        tuple[float, float, float],
        tuple[float, float, float],
        tuple[float, float, float],
    ]
    normal: tuple[float, float, float]


def _parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=DEFAULT_MANIFEST_PATH,
        help="Path to CornellSceneManifest.v1.json.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT_DIRECTORY,
        help="Directory that receives deterministic generated outputs.",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Validate the manifest and byte-compare all checked-in outputs without writing.",
    )
    parser.add_argument(
        "--print-derived",
        action="store_true",
        help="Print only compact derived values (RGB, counts, bounds, light area and hashes).",
    )
    return parser.parse_args()


def _load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as source:
        value = json.load(source)
    if not isinstance(value, dict):
        raise ValueError(f"expected a JSON object: {path}")
    return value


def _json_bytes(value: Any) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode("utf-8")


def _make_hengine_scene(manifest: dict[str, Any]) -> bytes:
    camera = manifest["camera"]
    meters_per_unit = float(manifest["coordinateSystem"]["metersPerSourceUnit"])
    position = [
        _round_json_float(float(value) * meters_per_unit)
        for value in camera["positionMm"]
    ]
    vertical_fov_radians = 2.0 * math.atan(
        float(camera["filmHeightM"]) / (2.0 * float(camera["focalLengthM"]))
    )
    identity_transform = {
        "Rotation": [0.0, 0.0, 0.0],
        "Scale": [1.0, 1.0, 1.0],
        "Translation": [0.0, 0.0, 0.0],
    }
    scene = {
        "2db5c629-8172-54f8-8b8c-955c6ef07c7a": {
            "MeshComponent": {
                "Path": "Assets/CornellBox/CornellBox.hmesh",
                "meshResPath": "Assets/CornellBox/CornellBox.hmesh",
            },
            "TagComponent": {"Tag": "Cornell Box Identity Mesh"},
            "TransformComponent": identity_transform,
        },
        "576c2ac0-bf6f-59c5-aa98-e11313004508": {
            "CameraComponent": {
                "Camera": {
                    "mAspectRatio": 1.0,
                    "mOrthographicFar": 1.0,
                    "mOrthographicNear": -1.0,
                    "mOrthographicSize": 10.0,
                    "mPerspectiveFOV": _round_json_float(vertical_fov_radians),
                    "mPerspectiveFar": 10.0,
                    "mPerspectiveNear": 0.01,
                    "mProjectionType": 0,
                },
                "FixedAspectRatio": True,
                "Primary": True,
            },
            "TagComponent": {"Tag": "Cornell Official Camera"},
            # HEngine cameras look down local -Z. A pi yaw maps that axis to
            # Cornell's measured +Z direction without changing the manifest.
            "TransformComponent": {
                "Rotation": [0.0, _round_json_float(math.pi), 0.0],
                "Scale": [1.0, 1.0, 1.0],
                "Translation": position,
            },
        },
        "95c55b69-a4df-560f-8d1d-f1cdbd35b152": {
            "EnvironmentLightComponent": {
                "Exposure": 1.0,
                "HdrTexturePath": "",
                "IBLIntensity": 0.0,
            },
            "TagComponent": {"Tag": "Cornell Fixed Exposure"},
            "TransformComponent": identity_transform,
        },
    }
    return _json_bytes(scene)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _canonical_text_bytes(path: Path) -> bytes:
    text = path.read_text(encoding="utf-8")
    return text.replace("\r\n", "\n").replace("\r", "\n").encode("utf-8")


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value))


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _vector_add(a: Sequence[float], b: Sequence[float]) -> tuple[float, float, float]:
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def _vector_sub(a: Sequence[float], b: Sequence[float]) -> tuple[float, float, float]:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _vector_scale(a: Sequence[float], scale: float) -> tuple[float, float, float]:
    return (a[0] * scale, a[1] * scale, a[2] * scale)


def _dot(a: Sequence[float], b: Sequence[float]) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a: Sequence[float], b: Sequence[float]) -> tuple[float, float, float]:
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def _length(a: Sequence[float]) -> float:
    return math.sqrt(_dot(a, a))


def _normalize(a: Sequence[float]) -> tuple[float, float, float]:
    length = _length(a)
    _require(length > 1.0e-12, "encountered a degenerate vector")
    return (a[0] / length, a[1] / length, a[2] / length)


def _centroid(points: Sequence[Sequence[float]]) -> tuple[float, float, float]:
    reciprocal = 1.0 / len(points)
    return (
        sum(point[0] for point in points) * reciprocal,
        sum(point[1] for point in points) * reciprocal,
        sum(point[2] for point in points) * reciprocal,
    )


def _triangle_normal(vertices: Sequence[Sequence[float]]) -> tuple[float, float, float]:
    return _normalize(_cross(_vector_sub(vertices[1], vertices[0]), _vector_sub(vertices[2], vertices[0])))


def _triangle_area(vertices: Sequence[Sequence[float]]) -> float:
    return 0.5 * _length(_cross(_vector_sub(vertices[1], vertices[0]), _vector_sub(vertices[2], vertices[0])))


def _as_vec3(value: Any, context: str) -> tuple[float, float, float]:
    _require(isinstance(value, list) and len(value) == 3, f"{context} must be a three-number array")
    _require(all(_is_number(component) for component in value), f"{context} contains a non-finite number")
    return (float(value[0]), float(value[1]), float(value[2]))


def _convert_quad_mm(quad: Any, meters_per_unit: float, context: str) -> tuple[tuple[float, float, float], ...]:
    _require(isinstance(quad, list) and len(quad) == 4, f"{context} must contain four vertices")
    return tuple(_vector_scale(_as_vec3(vertex, f"{context} vertex"), meters_per_unit) for vertex in quad)


def _ceiling_ring_faces(
    surface: dict[str, Any],
    meters_per_unit: float,
) -> list[Face]:
    outer = _convert_quad_mm(surface["quadsMm"][0], meters_per_unit, "ceiling outer quad")
    holes = surface.get("discontinuityMeshHolesMm")
    _require(isinstance(holes, list) and len(holes) == 1, "ceiling must provide exactly one light aperture")
    inner = _convert_quad_mm(holes[0], meters_per_unit, "ceiling light aperture")
    # Both source loops are ordered for a -Y normal. Connect matching corners
    # into four non-overlapping quads; no epsilon or geometry offset is used.
    quads = (
        (outer[0], inner[0], inner[3], outer[3]),
        (outer[0], outer[1], inner[1], inner[0]),
        (outer[1], outer[2], inner[2], inner[1]),
        (outer[2], outer[3], inner[3], inner[2]),
    )
    return [
        Face(
            name=f"ceiling_ring_{index}",
            material=str(surface["material"]),
            normal_policy=str(surface["normalPolicy"]),
            vertices=quad,
        )
        for index, quad in enumerate(quads)
    ]


def build_faces(manifest: dict[str, Any]) -> list[Face]:
    meters_per_unit = float(manifest["coordinateSystem"]["metersPerSourceUnit"])
    result: list[Face] = []
    for surface in manifest["geometry"]["surfaces"]:
        name = str(surface["name"])
        if name == "ceiling":
            result.extend(_ceiling_ring_faces(surface, meters_per_unit))
            continue
        for index, quad in enumerate(surface["quadsMm"]):
            result.append(
                Face(
                    name=name if len(surface["quadsMm"]) == 1 else f"{name}_{index}",
                    material=str(surface["material"]),
                    normal_policy=str(surface["normalPolicy"]),
                    vertices=_convert_quad_mm(quad, meters_per_unit, f"{name} quad {index}"),
                )
            )
    return result


def triangulate_faces(faces: Sequence[Face]) -> list[Triangle]:
    result: list[Triangle] = []
    for face in faces:
        for local_indices in ((0, 1, 2), (0, 2, 3)):
            vertices = tuple(face.vertices[index] for index in local_indices)
            normal = _triangle_normal(vertices)
            result.append(
                Triangle(
                    surface=face.name,
                    material=face.material,
                    vertices=vertices,  # type: ignore[arg-type]
                    normal=normal,
                )
            )
    return result


def _surface_source(manifest: dict[str, Any], name: str) -> dict[str, Any]:
    for surface in manifest["geometry"]["surfaces"]:
        if surface["name"] == name:
            return surface
    raise ValueError(f"missing source surface: {name}")


def _canonical_surface_name(generated_name: str) -> str:
    for source_name in (
        "short_block",
        "tall_block",
        "back_wall",
        "right_wall",
        "left_wall",
        "ceiling",
        "floor",
        "light",
    ):
        if generated_name == source_name or generated_name.startswith(source_name + "_"):
            return source_name
    raise ValueError(f"generated face has no source surface: {generated_name}")


def _scene_bounds(triangles: Sequence[Triangle]) -> tuple[list[float], list[float]]:
    points = [point for triangle in triangles for point in triangle.vertices]
    return (
        [min(point[axis] for point in points) for axis in range(3)],
        [max(point[axis] for point in points) for axis in range(3)],
    )


def _almost_equal_sequence(a: Sequence[float], b: Sequence[float], tolerance: float = 1.0e-9) -> bool:
    return len(a) == len(b) and all(abs(float(left) - float(right)) <= tolerance for left, right in zip(a, b))


def validate_manifest(manifest: dict[str, Any]) -> dict[str, Any]:
    _require(manifest.get("manifestVersion") == 1, "manifestVersion must be 1")
    _require(manifest.get("generatorVersion") == "1.2.0", "generatorVersion must match this generator")
    coordinate_system = manifest.get("coordinateSystem")
    _require(isinstance(coordinate_system, dict), "coordinateSystem must be an object")
    _require(coordinate_system.get("sourceUnit") == "millimeter", "source unit must remain millimeter")
    _require(coordinate_system.get("generatedUnit") == "meter", "generated unit must remain meter")
    _require(abs(float(coordinate_system.get("metersPerSourceUnit", 0.0)) - 0.001) < 1.0e-15, "mm-to-m scale must be exactly 0.001")

    materials = manifest.get("materials")
    _require(isinstance(materials, list), "materials must be an array")
    material_names = [material.get("name") for material in materials]
    material_ids = [material.get("id") for material in materials]
    _require(material_names == ["white", "green", "red", "light"], "material slots or ordering changed")
    _require(material_ids == [1, 2, 3, 4], "material IDs must be stable and contiguous")
    _require(all(material.get("bsdf", "").startswith("lambertian") for material in materials), "Cornell v1 permits only Lambertian materials")

    geometry = manifest.get("geometry")
    _require(isinstance(geometry, dict), "geometry must be an object")
    surfaces = geometry.get("surfaces")
    _require(isinstance(surfaces, list), "geometry surfaces must be an array")
    expected_surface_names = [
        "floor",
        "light",
        "ceiling",
        "back_wall",
        "right_wall",
        "left_wall",
        "short_block",
        "tall_block",
    ]
    _require([surface.get("name") for surface in surfaces] == expected_surface_names, "official surface slots or ordering changed")
    _require(sum(len(surface.get("quadsMm", [])) for surface in surfaces) == 16, "official source quad count changed")
    _require({surface.get("material") for surface in surfaces} == set(material_names), "geometry material assignment is incomplete")
    light_source = _surface_source(manifest, "light")
    ceiling_source = _surface_source(manifest, "ceiling")
    _require(
        ceiling_source.get("discontinuityMeshHolesMm") == light_source.get("quadsMm"),
        "ceiling aperture must exactly match the measured light quad",
    )
    floor_source = _surface_source(manifest, "floor")
    floor_holes = floor_source.get("discontinuityMeshHolesMm")
    _require(isinstance(floor_holes, list) and len(floor_holes) == 2, "floor discontinuity loops are incomplete")
    for hole, block_name in zip(floor_holes, ("short_block", "tall_block")):
        block_top = _surface_source(manifest, block_name)["quadsMm"][0]
        hole_xz = {(float(vertex[0]), float(vertex[2])) for vertex in hole}
        top_xz = {(float(vertex[0]), float(vertex[2])) for vertex in block_top}
        _require(hole_xz == top_xz, f"{block_name} footprint no longer matches its published floor loop")

    spectra = manifest.get("spectra")
    _require(isinstance(spectra, dict), "spectra must be an object")
    reflectance = spectra.get("surfaceReflectance")
    _require(isinstance(reflectance, dict), "surfaceReflectance must be an object")
    wavelengths = reflectance.get("wavelengths")
    _require(isinstance(wavelengths, list) and wavelengths == list(range(400, 701, 4)), "surface wavelengths must be 400..700 nm in 4 nm steps")
    for name in ("white", "green", "red"):
        samples = reflectance.get(name)
        _require(isinstance(samples, list) and len(samples) == len(wavelengths), f"{name} spectrum length mismatch")
        _require(all(_is_number(value) and 0.0 <= float(value) <= 1.0 for value in samples), f"{name} spectrum contains invalid reflectance")
    emission = spectra.get("lightEmission")
    _require(isinstance(emission, dict), "lightEmission must be an object")
    _require(emission.get("valueUnit") == "unspecified-by-upstream-source", "do not invent a physical unit for Cornell light samples")
    _require(emission.get("wavelengths") == [400.0, 500.0, 600.0, 700.0], "official light wavelengths changed")
    _require(emission.get("values") == [0.0, 8.0, 15.6, 18.4], "official light emission samples changed")

    camera = manifest.get("camera")
    _require(isinstance(camera, dict), "camera must be an object")
    _require(camera.get("positionMm") == [278.0, 273.0, -800.0], "official camera position changed")
    _require(camera.get("direction") == [0.0, 0.0, 1.0], "official camera direction changed")
    _require(camera.get("up") == [0.0, 1.0, 0.0], "official camera up changed")
    _require(abs(float(camera.get("focalLengthM", 0.0)) - 0.035) < 1.0e-15, "camera focal length must be 35 mm")
    _require(abs(float(camera.get("filmWidthM", 0.0)) - 0.025) < 1.0e-15, "film width must be 25 mm")
    _require(abs(float(camera.get("filmHeightM", 0.0)) - 0.025) < 1.0e-15, "film height must be 25 mm")
    expected_fov = math.degrees(2.0 * math.atan(0.5 * 0.025 / 0.035))
    _require(abs(float(camera.get("expectedVerticalFovDegrees", 0.0)) - expected_fov) < 1.0e-12, "camera FOV is inconsistent with film and focal length")

    provenance = manifest.get("provenance")
    _require(isinstance(provenance, dict), "provenance must be an object")
    sources = provenance.get("sources")
    _require(isinstance(sources, list) and len(sources) >= 4, "provenance sources are incomplete")
    authoritative = next((source for source in sources if source.get("role") == "authoritative-scene-data"), None)
    _require(isinstance(authoritative, dict), "authoritative Cornell source is missing")
    _require(authoritative.get("url") == "https://bowers.cornell.edu/computer-graphics/data", "Cornell source URL changed")
    _require(isinstance(authoritative.get("retrievedSha256"), str) and len(authoritative["retrievedSha256"]) == 64, "source retrieval hash is missing")
    license_info = provenance.get("license")
    _require(isinstance(license_info, dict), "license review is missing")
    _require(license_info.get("spdxIdentifier") is None, "do not infer an SPDX license absent an upstream grant")

    faces = build_faces(manifest)
    triangles = triangulate_faces(faces)
    output_contract = manifest["generatedOutputs"]
    pbrt_settings = output_contract["pbrtReferenceSettings"]
    _require(pbrt_settings.get("seed") == 1, "PBRT reference seed must remain fixed at 1")
    pixel_samples = int(pbrt_settings.get("pixelSamples", 0))
    _require(pixel_samples >= 1 and (pixel_samples & (pixel_samples - 1)) == 0, "Sobol pixelSamples must be a power of two")
    engine_bounces = int(pbrt_settings.get("engineMaxDiffuseBounces", -1))
    pbrt_max_depth = int(pbrt_settings.get("maxDepth", 0))
    _require(engine_bounces >= 0, "Engine maximum diffuse bounces must be non-negative")
    _require(
        pbrt_max_depth == engine_bounces + 1,
        "PBRT maxDepth must be Engine max diffuse bounces + 1 so terminal NEE semantics match",
    )
    _require(pbrt_settings.get("lightSampler") == "power", "PBRT reference must use power light sampling")
    _require(pbrt_settings.get("filter") == "box", "PBRT reference filter must remain box for comparison")
    _require(pbrt_settings.get("saveFp16") is False, "PBRT reference must save 32-bit float EXR")
    _require(float(pbrt_settings.get("filmIso", 0.0)) == 100.0, "PBRT reference ISO must preserve unit exposure")
    _require(float(pbrt_settings.get("whiteBalanceKelvin", -1.0)) == 0.0, "PBRT reference must not apply white balance")
    _require(
        pbrt_settings.get("spectrumEndpointPolicy")
        == "hold the nearest official 400/700 nm sample through PBRT's 360-830 nm visible range; generated PBRT writes the 360 and 830 nm endpoints explicitly",
        "PBRT spectrum endpoint policy changed",
    )
    offline_truth = manifest["engineParity"].get("offlineRgbTruth")
    _require(isinstance(offline_truth, dict), "strict offline RGB truth contract is missing")
    spectral_proxy = offline_truth.get("spectralProxy")
    _require(isinstance(spectral_proxy, dict), "RGB-authored spectral proxy contract is missing")
    _require(
        spectral_proxy.get("sceneFile") == PARITY_PROXY_PBRT_FILENAME,
        "RGB-authored spectral proxy filename changed",
    )
    channel_scenes = offline_truth.get("channelScenes")
    _require(isinstance(channel_scenes, dict), "channel-separated PBRT scenes are missing")
    _require(list(channel_scenes) == ["R", "G", "B"], "channel-separated PBRT order must remain R/G/B")
    for channel_index, channel_name in enumerate(("R", "G", "B")):
        scene = channel_scenes[channel_name]
        _require(isinstance(scene, dict), f"{channel_name} PBRT scene contract is invalid")
        _require(
            scene.get("sceneFile") == PARITY_CHANNEL_PBRT_FILENAMES[channel_name],
            f"{channel_name} PBRT scene filename changed",
        )
        _require(
            int(scene.get("sourceChannelIndex", -1)) == channel_index,
            f"{channel_name} PBRT source channel index changed",
        )
    orientation = offline_truth.get("orientationValidation")
    _require(isinstance(orientation, dict), "channel-combine orientation validation is missing")
    for key in (
        "redRenderRedWallToGreenWallMinimum",
        "greenRenderGreenWallToRedWallMinimum",
        "lightToNonEmissiveMinimum",
    ):
        _require(float(orientation.get(key, 0.0)) > 1.0, f"invalid strict RGB orientation threshold: {key}")
    _require(int(output_contract.get("maskEdgeRadiusPixels", -1)) in {1, 2}, "edge mask radius must remain one or two pixels")
    _require(float(output_contract.get("maskDepthEdgeAbsoluteM", 0.0)) > 0.0, "absolute depth-edge threshold must be positive")
    _require(float(output_contract.get("maskDepthEdgeRelative", 0.0)) > 0.0, "relative depth-edge threshold must be positive")
    _require(0.0 < float(output_contract.get("lightOcclusionOriginOffsetM", 0.0)) <= 1.0e-3, "light occlusion origin offset is invalid")
    disocclusion = output_contract.get("disocclusionValidation")
    _require(isinstance(disocclusion, dict), "disocclusion validation contract is missing")
    _require(disocclusion.get("name") == "short_block_negx50mm", "disocclusion scenario name changed")
    _require(disocclusion.get("movingSurfaceName") == "short_block", "disocclusion moving surface changed")
    _require(
        _almost_equal_sequence(disocclusion.get("translationM", []), [-0.05, 0.0, 0.0], 1.0e-12),
        "disocclusion translation must remain exactly -50 mm on X",
    )
    _require(
        disocclusion.get("allowedRevealedSurfaceNames") == ["floor", "back_wall", "tall_block"],
        "disocclusion revealed-surface contract changed",
    )
    _require(int(disocclusion.get("edgeExclusionRadiusPixels", -1)) == 2, "disocclusion edge exclusion must remain two pixels")
    _require(int(disocclusion.get("expectedRawPixelCount", 0)) > 0, "disocclusion raw pixel count is invalid")
    _require(
        disocclusion.get("expectedRawPixelBoundsTopLeftInclusiveExclusive") == [247, 328, 380, 482],
        "raw disocclusion pixel bounds changed",
    )
    _require(int(disocclusion.get("expectedPixelCount", 0)) > 0, "disocclusion pixel count is invalid")
    _require(
        disocclusion.get("expectedPixelBoundsTopLeftInclusiveExclusive") == [250, 331, 296, 470],
        "disocclusion pixel bounds changed",
    )
    _require(int(disocclusion.get("referenceSamples", 0)) == 512, "disocclusion Reference PT must remain 512 spp")
    _require(int(disocclusion.get("referenceMaxBounces", -1)) == 8, "disocclusion Reference PT bounce count changed")
    _require(int(disocclusion.get("postMoveCaptureFrames", 0)) == 16, "disocclusion sequence must contain 16 post-move frames")
    _require(abs(float(disocclusion.get("recoveryThreshold", -1.0)) - 0.05) < 1.0e-12, "disocclusion recovery threshold changed")
    _require(int(disocclusion.get("recoveryConsecutiveFrames", 0)) == 3, "disocclusion recovery confirmation length changed")
    _require(int(disocclusion.get("maxRecoveryFrames", -1)) == 8, "disocclusion recovery-frame gate changed")
    _require(len(triangles) == int(output_contract["expectedTriangleCount"]), "triangle count mismatch")
    _require(len(triangles) * 3 == int(output_contract["expectedVertexCount"]), "hard-split vertex count mismatch")

    bounds_min, bounds_max = _scene_bounds(triangles)
    expected_bounds = output_contract["expectedBoundsM"]
    _require(_almost_equal_sequence(bounds_min, expected_bounds["min"]), f"minimum bounds mismatch: {bounds_min}")
    _require(_almost_equal_sequence(bounds_max, expected_bounds["max"]), f"maximum bounds mismatch: {bounds_max}")

    room_center = (0.278, 0.2744, 0.2796)
    block_points: dict[str, list[tuple[float, float, float]]] = {"short_block": [], "tall_block": []}
    for triangle in triangles:
        source_name = _canonical_surface_name(triangle.surface)
        face_center = _centroid(triangle.vertices)
        if source_name in {"floor", "ceiling", "back_wall", "right_wall", "left_wall"}:
            _require(_dot(triangle.normal, _vector_sub(room_center, face_center)) > 1.0e-5, f"{triangle.surface} winding does not face the room")
        elif source_name == "light":
            _require(triangle.normal[1] < -0.999999, "light normal must point down")
        if source_name in block_points:
            block_points[source_name].extend(triangle.vertices)

    for block_name, points in block_points.items():
        block_center = _centroid(points)
        for triangle in triangles:
            if triangle.surface.startswith(block_name):
                face_center = _centroid(triangle.vertices)
                _require(_dot(triangle.normal, _vector_sub(face_center, block_center)) > 1.0e-5, f"{triangle.surface} normal does not point out of the block")

    light_triangles = [triangle for triangle in triangles if triangle.material == "light"]
    light_area = sum(_triangle_area(triangle.vertices) for triangle in light_triangles)
    _require(abs(light_area - float(output_contract["expectedLightAreaM2"])) < 1.0e-12, f"light area mismatch: {light_area}")
    _require(abs(float(_surface_source(manifest, "light")["quadsMm"][0][0][1]) - 548.8) < 1.0e-12, "light height changed")
    _require(abs(_triangle_area(light_triangles[0].vertices) - _triangle_area(light_triangles[1].vertices)) < 1.0e-15, "light diagonal must produce equal-area triangles")
    light_sampling = manifest["engineParity"]["emissiveTriangleSampling"]
    _require(light_sampling.get("distributionWeight") == "triangleAreaM2 * luminance(sceneLinearRadiance)", "emissive light weight contract changed")
    _require(light_sampling.get("luminanceCoefficients") == [0.2126, 0.7152, 0.0722], "emissive luminance coefficients changed")
    _require(light_sampling.get("twoSided") is False, "Cornell ceiling light must be one-sided")
    _require(
        float(manifest["engineParity"]["emissiveStrength"])
        == max(float(value) for value in emission["values"]),
        "EngineParity emissive strength must remain the single global scale anchored to the maximum official emission sample",
    )

    ceiling_faces = [face for face in faces if face.name.startswith("ceiling_ring_")]
    ceiling_area = sum(
        _triangle_area((face.vertices[0], face.vertices[1], face.vertices[2]))
        + _triangle_area((face.vertices[0], face.vertices[2], face.vertices[3]))
        for face in ceiling_faces
    )
    measured_ceiling = _convert_quad_mm(ceiling_source["quadsMm"][0], 0.001, "ceiling")
    measured_ceiling_area = (
        _triangle_area((measured_ceiling[0], measured_ceiling[1], measured_ceiling[2]))
        + _triangle_area((measured_ceiling[0], measured_ceiling[2], measured_ceiling[3]))
    )
    _require(
        abs((ceiling_area + light_area) - measured_ceiling_area) < 1.0e-12,
        "ceiling aperture decomposition has a gap or overlap",
    )

    return {
        "faces": faces,
        "triangles": triangles,
        "boundsMin": bounds_min,
        "boundsMax": bounds_max,
        "lightAreaM2": light_area,
        "verticalFovDegrees": expected_fov,
    }


def _cie_xyz_fit(wavelength_nm: float) -> tuple[float, float, float]:
    # Equation 3 from Wyman, Sloan and Shirley (JCGT 2013).
    x_t1 = (wavelength_nm - 442.0) * (0.0624 if wavelength_nm < 442.0 else 0.0374)
    x_t2 = (wavelength_nm - 599.8) * (0.0264 if wavelength_nm < 599.8 else 0.0323)
    x_t3 = (wavelength_nm - 501.1) * (0.0490 if wavelength_nm < 501.1 else 0.0382)
    x = (
        0.362 * math.exp(-0.5 * x_t1 * x_t1)
        + 1.056 * math.exp(-0.5 * x_t2 * x_t2)
        - 0.065 * math.exp(-0.5 * x_t3 * x_t3)
    )
    y_t1 = (wavelength_nm - 568.8) * (0.0213 if wavelength_nm < 568.8 else 0.0247)
    y_t2 = (wavelength_nm - 530.9) * (0.0613 if wavelength_nm < 530.9 else 0.0322)
    y = 0.821 * math.exp(-0.5 * y_t1 * y_t1) + 0.286 * math.exp(-0.5 * y_t2 * y_t2)
    z_t1 = (wavelength_nm - 437.0) * (0.0845 if wavelength_nm < 437.0 else 0.0278)
    z_t2 = (wavelength_nm - 459.0) * (0.0385 if wavelength_nm < 459.0 else 0.0725)
    z = 1.217 * math.exp(-0.5 * z_t1 * z_t1) + 0.681 * math.exp(-0.5 * z_t2 * z_t2)
    return (x, y, z)


def _interpolate_spectrum(wavelengths: Sequence[float], values: Sequence[float], wavelength: float) -> float:
    if wavelength <= wavelengths[0]:
        return float(values[0])
    if wavelength >= wavelengths[-1]:
        return float(values[-1])
    low = 0
    high = len(wavelengths) - 1
    while low + 1 < high:
        midpoint = (low + high) // 2
        if wavelengths[midpoint] <= wavelength:
            low = midpoint
        else:
            high = midpoint
    interval = float(wavelengths[high]) - float(wavelengths[low])
    fraction = (wavelength - float(wavelengths[low])) / interval
    return float(values[low]) * (1.0 - fraction) + float(values[high]) * fraction


def _integrate_xyz(
    wavelengths: Sequence[float],
    values: Sequence[float],
    illuminant: Sequence[float] | None = None,
) -> tuple[float, float, float]:
    xyz = [0.0, 0.0, 0.0]
    previous: tuple[float, float, float] | None = None
    for wavelength in range(400, 701):
        sample = _interpolate_spectrum(wavelengths, values, float(wavelength))
        if illuminant is not None:
            sample *= float(illuminant[wavelength - 400])
        matching = _cie_xyz_fit(float(wavelength))
        current = tuple(sample * component for component in matching)
        if previous is not None:
            for axis in range(3):
                xyz[axis] += 0.5 * (previous[axis] + current[axis])
        previous = current
    return (xyz[0], xyz[1], xyz[2])


def _matrix_vector(matrix: Sequence[Sequence[float]], vector: Sequence[float]) -> tuple[float, float, float]:
    return tuple(sum(float(matrix[row][column]) * float(vector[column]) for column in range(3)) for row in range(3))  # type: ignore[return-value]


def _bradford_adapt(
    xyz: Sequence[float],
    source_white: Sequence[float],
    target_white: Sequence[float],
) -> tuple[float, float, float]:
    bradford = (
        (0.8951, 0.2664, -0.1614),
        (-0.7502, 1.7135, 0.0367),
        (0.0389, -0.0685, 1.0296),
    )
    bradford_inverse = (
        (0.9869929, -0.1470543, 0.1599627),
        (0.4323053, 0.5183603, 0.0492912),
        (-0.0085287, 0.0400428, 0.9684867),
    )
    source_lms = _matrix_vector(bradford, source_white)
    target_lms = _matrix_vector(bradford, target_white)
    value_lms = _matrix_vector(bradford, xyz)
    adapted_lms = tuple(value_lms[index] * target_lms[index] / source_lms[index] for index in range(3))
    return _matrix_vector(bradford_inverse, adapted_lms)


def _xyz_to_linear_srgb(xyz: Sequence[float]) -> tuple[float, float, float]:
    matrix = (
        (3.2404542, -1.5371385, -0.4985314),
        (-0.9692660, 1.8760108, 0.0415560),
        (0.0556434, -0.2040259, 1.0572252),
    )
    return _matrix_vector(matrix, xyz)


def derive_engine_rgb(manifest: dict[str, Any]) -> dict[str, tuple[float, float, float]]:
    reflectance = manifest["spectra"]["surfaceReflectance"]
    wavelengths = [float(value) for value in reflectance["wavelengths"]]
    equal_energy = [1.0] * 301
    source_white_xyz = _integrate_xyz([400.0, 700.0], [1.0, 1.0], equal_energy)
    source_white_xyz = tuple(component / source_white_xyz[1] for component in source_white_xyz)
    d65_white = (0.95047, 1.0, 1.08883)

    result: dict[str, tuple[float, float, float]] = {}
    for name in ("white", "green", "red"):
        xyz = _integrate_xyz(wavelengths, [float(value) for value in reflectance[name]], equal_energy)
        xyz = tuple(component / _integrate_xyz([400.0, 700.0], [1.0, 1.0], equal_energy)[1] for component in xyz)
        adapted = _bradford_adapt(xyz, source_white_xyz, d65_white)
        rgb = _xyz_to_linear_srgb(adapted)
        result[name] = tuple(min(1.0, max(0.0, component)) for component in rgb)

    emission = manifest["spectra"]["lightEmission"]
    light_xyz = _integrate_xyz(
        [float(value) for value in emission["wavelengths"]],
        [float(value) for value in emission["values"]],
    )
    light_rgb = tuple(max(0.0, component) for component in _xyz_to_linear_srgb(light_xyz))
    maximum = max(light_rgb)
    _require(maximum > 0.0, "light spectrum converts to black")
    result["light"] = tuple(component / maximum for component in light_rgb)
    return result


def _align_buffer(buffer: bytearray, alignment: int = 4) -> None:
    while len(buffer) % alignment:
        buffer.append(0)


def _append_buffer_view(
    buffer: bytearray,
    buffer_views: list[dict[str, Any]],
    payload: bytes,
    *,
    target: int | None = None,
) -> int:
    _align_buffer(buffer)
    offset = len(buffer)
    buffer.extend(payload)
    view: dict[str, Any] = {
        "buffer": 0,
        "byteLength": len(payload),
        "byteOffset": offset,
    }
    if target is not None:
        view["target"] = target
    buffer_views.append(view)
    return len(buffer_views) - 1


def _pack_floats(values: Iterable[float]) -> bytes:
    values_list = [float(value) for value in values]
    return struct.pack("<" + "f" * len(values_list), *values_list)


def _pack_uint16(values: Iterable[int]) -> bytes:
    values_list = [int(value) for value in values]
    _require(all(0 <= value <= 65535 for value in values_list), "index does not fit uint16")
    return struct.pack("<" + "H" * len(values_list), *values_list)


def _material_lookup(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {str(material["name"]): material for material in manifest["materials"]}


def _round_json_float(value: float) -> float:
    # JSON numbers remain compact and deterministic without changing float32
    # payloads. Nine significant digits round-trip IEEE-754 binary32.
    return float(format(float(value), ".9g"))


def _make_gltf(
    manifest: dict[str, Any],
    triangles: Sequence[Triangle],
    engine_rgb: dict[str, tuple[float, float, float]],
    manifest_sha256: str,
    generator_sha256: str,
) -> tuple[bytes, bytes]:
    buffer = bytearray()
    buffer_views: list[dict[str, Any]] = []
    accessors: list[dict[str, Any]] = []
    primitives: list[dict[str, Any]] = []
    materials_by_name = _material_lookup(manifest)
    material_order = [str(material["name"]) for material in manifest["materials"]]

    gltf_materials: list[dict[str, Any]] = []
    for name in material_order:
        material_source = materials_by_name[name]
        base_rgb = (
            (float(material_source["reflectance"]),) * 3
            if name == "light"
            else engine_rgb[name]
        )
        material: dict[str, Any] = {
            "alphaMode": "OPAQUE",
            "doubleSided": False,
            "extras": {
                "cornellMaterialId": int(material_source["id"]),
                "cornellSpectrum": name if name != "light" else "constant-reflectance-0.78",
                "engineParityRgb": [_round_json_float(value) for value in base_rgb],
            },
            "name": name,
            "pbrMetallicRoughness": {
                "baseColorFactor": [_round_json_float(value) for value in base_rgb] + [1.0],
                "metallicFactor": float(material_source["metallic"]),
                "roughnessFactor": float(material_source["roughness"]),
            },
        }
        if name == "light":
            material["emissiveFactor"] = [_round_json_float(value) for value in engine_rgb["light"]]
            material["extensions"] = {
                "KHR_materials_emissive_strength": {
                    "emissiveStrength": float(manifest["engineParity"]["emissiveStrength"])
                }
            }
            material["extras"]["emissiveRadiance"] = [
                _round_json_float(value * float(manifest["engineParity"]["emissiveStrength"]))
                for value in engine_rgb["light"]
            ]
            material["extras"]["emissiveRadianceUnit"] = manifest["engineParity"]["radianceUnit"]
        gltf_materials.append(material)

    for material_index, material_name in enumerate(material_order):
        material_triangles = [triangle for triangle in triangles if triangle.material == material_name]
        _require(material_triangles, f"material {material_name} has no triangles")
        positions = [component for triangle in material_triangles for vertex in triangle.vertices for component in vertex]
        normals = [component for triangle in material_triangles for _vertex in triangle.vertices for component in triangle.normal]
        vertex_count = len(material_triangles) * 3
        indices = list(range(vertex_count))

        position_view = _append_buffer_view(buffer, buffer_views, _pack_floats(positions), target=34962)
        position_accessor = {
            "bufferView": position_view,
            "componentType": 5126,
            "count": vertex_count,
            "max": [_round_json_float(max(positions[axis::3])) for axis in range(3)],
            "min": [_round_json_float(min(positions[axis::3])) for axis in range(3)],
            "type": "VEC3",
        }
        accessors.append(position_accessor)
        position_accessor_index = len(accessors) - 1

        normal_view = _append_buffer_view(buffer, buffer_views, _pack_floats(normals), target=34962)
        accessors.append(
            {
                "bufferView": normal_view,
                "componentType": 5126,
                "count": vertex_count,
                "type": "VEC3",
            }
        )
        normal_accessor_index = len(accessors) - 1

        index_view = _append_buffer_view(buffer, buffer_views, _pack_uint16(indices), target=34963)
        accessors.append(
            {
                "bufferView": index_view,
                "componentType": 5123,
                "count": len(indices),
                "max": [max(indices)],
                "min": [0],
                "type": "SCALAR",
            }
        )
        index_accessor_index = len(accessors) - 1

        primitives.append(
            {
                "attributes": {
                    "NORMAL": normal_accessor_index,
                    "POSITION": position_accessor_index,
                },
                "extras": {
                    "cornellMaterialId": int(materials_by_name[material_name]["id"]),
                    "flatNormals": True,
                    "hardSplitVertices": True,
                    "triangleCount": len(material_triangles),
                    **(
                        {
                            "emissiveLight": True,
                            "emissiveTriangleSampling": manifest["engineParity"]["emissiveTriangleSampling"],
                        }
                        if material_name == "light"
                        else {}
                    ),
                },
                "indices": index_accessor_index,
                "material": material_index,
                "mode": 4,
            }
        )

    _align_buffer(buffer)
    camera = manifest["camera"]
    meters_per_unit = float(manifest["coordinateSystem"]["metersPerSourceUnit"])
    position_m = [float(value) * meters_per_unit for value in camera["positionMm"]]
    gltf = {
        "accessors": accessors,
        "asset": {
            "copyright": "Cornell measured numeric data, attribution and license status in CornellSceneManifest.v1.json",
            "extras": {
                "cornellCamera": {
                    "direction": camera["direction"],
                    "filmHeightM": camera["filmHeightM"],
                    "filmWidthM": camera["filmWidthM"],
                    "focalLengthM": camera["focalLengthM"],
                    "positionM": position_m,
                    "up": camera["up"],
                    "verticalFovDegrees": camera["expectedVerticalFovDegrees"],
                },
                "generatorVersion": manifest["generatorVersion"],
                "generatorSha256": generator_sha256,
                "manifestSha256": manifest_sha256,
                "sourceMetersPerUnit": manifest["coordinateSystem"]["metersPerSourceUnit"],
            },
            "generator": "HEngine Tools/Content/Cornell/generate_cornell_scene.py",
            "version": "2.0",
        },
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(buffer), "uri": BUFFER_FILENAME}],
        "extensionsUsed": ["KHR_materials_emissive_strength"],
        "materials": gltf_materials,
        "meshes": [
            {
                "extras": {
                    "bakedWorldSpace": True,
                    "triangleCount": len(triangles),
                    "vertexCount": len(triangles) * 3,
                },
                "name": "CornellBox",
                "primitives": primitives,
            }
        ],
        "nodes": [
            {
                "extras": {"identityTransformRequired": True},
                "matrix": IDENTITY_MATRIX,
                "mesh": 0,
                "name": "CornellBox_Identity",
            }
        ],
        "scene": 0,
        "scenes": [{"name": "CornellBox", "nodes": [0]}],
    }
    return _json_bytes(gltf), bytes(buffer)


def _format_number(value: float) -> str:
    if abs(value) < 5.0e-13:
        return "0"
    return format(float(value), ".9g")


def _format_pbrt_array(values: Iterable[float | int], indent: str = "    ") -> str:
    tokens = [_format_number(float(value)) for value in values]
    lines: list[str] = []
    for start in range(0, len(tokens), 12):
        lines.append(indent + " ".join(tokens[start:start + 12]))
    return "\n".join(lines)


def _spectrum_pairs(
    wavelengths: Sequence[float],
    values: Sequence[float],
    *,
    extend_pbrt_visible_range: bool = False,
) -> list[float]:
    _require(len(wavelengths) == len(values) and len(wavelengths) >= 2, "spectrum pair arrays are invalid")
    result: list[float] = []
    if extend_pbrt_visible_range and float(wavelengths[0]) > 360.0:
        result.extend((360.0, float(values[0])))
    for wavelength, value in zip(wavelengths, values):
        result.extend((float(wavelength), float(value)))
    if extend_pbrt_visible_range and float(wavelengths[-1]) < 830.0:
        result.extend((830.0, float(values[-1])))
    return result


def _pbrt_header(
    manifest: dict[str, Any],
    manifest_sha256: str,
    generator_sha256: str,
    output_filename: str,
) -> list[str]:
    camera = manifest["camera"]
    meters_per_unit = float(manifest["coordinateSystem"]["metersPerSourceUnit"])
    position = [float(value) * meters_per_unit for value in camera["positionMm"]]
    direction = [float(value) for value in camera["direction"]]
    target = [position[index] + direction[index] for index in range(3)]
    up = [float(value) for value in camera["up"]]
    screen_window = [float(value) for value in camera["pbrtScreenWindow"]]
    _require(
        screen_window == [1.0, -1.0, -1.0, 1.0],
        "PBRT screen window must preserve HEngine's horizontal camera orientation",
    )
    outputs = manifest["generatedOutputs"]
    pbrt_settings = outputs["pbrtReferenceSettings"]
    return [
        "# Deterministically generated. Do not edit; edit CornellSceneManifest.v1.json.",
        f"# manifest-sha256: {manifest_sha256}",
        f"# generator-sha256: {generator_sha256}",
        "# Geometry is meters; output is pre-tonemap linear HDR.",
        f'Option "seed" {int(pbrt_settings["seed"])}',
        'ColorSpace "srgb"',
        f'PixelFilter "{pbrt_settings["filter"]}" "float xradius" [0.5] "float yradius" [0.5]',
        f'Film "rgb" "string filename" ["{output_filename}"] "integer xresolution" [{int(outputs["imageWidth"])}] "integer yresolution" [{int(outputs["imageHeight"])}] "bool savefp16" [{str(bool(pbrt_settings["saveFp16"])).lower()}] "float iso" [{_format_number(float(pbrt_settings["filmIso"]))}] "float whitebalance" [{_format_number(float(pbrt_settings["whiteBalanceKelvin"]))}]',
        f'Sampler "sobol" "integer pixelsamples" [{int(pbrt_settings["pixelSamples"])}]',
        f'Integrator "path" "integer maxdepth" [{int(pbrt_settings["maxDepth"])}] "string lightsampler" ["{pbrt_settings["lightSampler"]}"]',
        "LookAt " + " ".join(_format_number(value) for value in (*position, *target, *up)),
        f'Camera "perspective" "float fov" [{_format_number(float(camera["expectedVerticalFovDegrees"]))}] "float screenwindow" [{" ".join(_format_number(value) for value in screen_window)}]',
        "WorldBegin",
        "",
    ]


def _pbrt_triangle_mesh(triangles: Sequence[Triangle]) -> list[str]:
    positions = [component for triangle in triangles for vertex in triangle.vertices for component in vertex]
    normals = [component for triangle in triangles for _vertex in triangle.vertices for component in triangle.normal]
    indices = list(range(len(triangles) * 3))
    return [
        'Shape "trianglemesh"',
        '    "point3 P" [',
        _format_pbrt_array(positions, "        "),
        "    ]",
        '    "normal N" [',
        _format_pbrt_array(normals, "        "),
        "    ]",
        '    "integer indices" [',
        _format_pbrt_array(indices, "        "),
        "    ]",
    ]


def _make_pbrt(
    manifest: dict[str, Any],
    triangles: Sequence[Triangle],
    engine_rgb: dict[str, tuple[float, float, float]],
    manifest_sha256: str,
    generator_sha256: str,
    *,
    mode: str,
    channel_name: str | None = None,
) -> bytes:
    _require(mode in {"measured", "spectral-proxy", "channel"}, f"unknown PBRT mode: {mode}")
    offline_truth = manifest["engineParity"]["offlineRgbTruth"]
    channel_index: int | None = None
    if mode == "measured":
        _require(channel_name is None, "measured PBRT scene cannot select an RGB channel")
        output_name = "CornellBox_MeasuredSpectral.exr"
    elif mode == "spectral-proxy":
        _require(channel_name is None, "spectral proxy cannot select an RGB channel")
        output_name = str(offline_truth["spectralProxy"]["outputFile"])
    else:
        _require(channel_name in {"R", "G", "B"}, "strict RGB PBRT channel must be R, G, or B")
        channel_contract = offline_truth["channelScenes"][channel_name]
        channel_index = int(channel_contract["sourceChannelIndex"])
        output_name = str(channel_contract["outputFile"])

    lines = _pbrt_header(manifest, manifest_sha256, generator_sha256, output_name)
    reflectance = manifest["spectra"]["surfaceReflectance"]
    wavelengths = [float(value) for value in reflectance["wavelengths"]]
    if mode == "measured":
        lines.extend(
            [
                "# Cornell's official samples span 400-700 nm.",
                "# Values at PBRT's 360/830 nm bounds explicitly hold the nearest official endpoint.",
                "",
            ]
        )
    elif mode == "spectral-proxy":
        lines.extend(
            [
                "# RGB-authored spectral proxy only.",
                "# PBRT v4 upsamples RGB inputs to spectra and transports them spectrally.",
                "# Do not use this scene as strict channel-wise HEngine RGB truth.",
                "# Use the generated R/G/B achromatic scenes and combine_engine_parity_rgb.py.",
                "",
            ]
        )
    else:
        lines.extend(
            [
                f"# Strict HEngine linear-RGB parity source for channel {channel_name}.",
                f"# Every RGB reflectance and radiance is achromatic and equals source channel {channel_index}.",
                f"# The combined truth takes channel {channel_name} from this render.",
                "",
            ]
        )

    for name in ("white", "green", "red"):
        if mode == "measured":
            pairs = _spectrum_pairs(
                wavelengths,
                [float(value) for value in reflectance[name]],
                extend_pbrt_visible_range=True,
            )
            lines.extend(
                [
                    f'MakeNamedMaterial "{name}" "string type" ["diffuse"]',
                    '    "spectrum reflectance" [',
                    _format_pbrt_array(pairs, "        "),
                    "    ]",
                ]
            )
        else:
            rgb = engine_rgb[name]
            if channel_index is not None:
                rgb = (rgb[channel_index],) * 3
            lines.append(
                f'MakeNamedMaterial "{name}" "string type" ["diffuse"] "rgb reflectance" [{" ".join(_format_number(value) for value in rgb)}]'
            )
    if mode == "measured":
        lines.append('MakeNamedMaterial "light" "string type" ["diffuse"] "spectrum reflectance" [360 0.78 830 0.78]')
    else:
        lines.append('MakeNamedMaterial "light" "string type" ["diffuse"] "rgb reflectance" [0.78 0.78 0.78]')
    lines.append("")

    material_order = [str(material["name"]) for material in manifest["materials"]]
    for material_name in material_order:
        material_triangles = [triangle for triangle in triangles if triangle.material == material_name]
        lines.append("AttributeBegin")
        lines.append(f'    NamedMaterial "{material_name}"')
        if material_name == "light":
            if mode == "measured":
                emission = manifest["spectra"]["lightEmission"]
                pairs = _spectrum_pairs(
                    [float(value) for value in emission["wavelengths"]],
                    [float(value) for value in emission["values"]],
                    extend_pbrt_visible_range=True,
                )
                lines.extend(
                    [
                        '    AreaLightSource "diffuse" "spectrum L" [',
                        _format_pbrt_array(pairs, "        "),
                        "    ]",
                    ]
                )
            else:
                strength = float(manifest["engineParity"]["emissiveStrength"])
                radiance = [value * strength for value in engine_rgb["light"]]
                if channel_index is not None:
                    radiance = [radiance[channel_index]] * 3
                lines.append(
                    f'    AreaLightSource "diffuse" "rgb L" [{" ".join(_format_number(value) for value in radiance)}]'
                )
        lines.extend("    " + line if line else line for line in _pbrt_triangle_mesh(material_triangles))
        lines.append("AttributeEnd")
        lines.append("")

    return ("\n".join(lines).rstrip() + "\n").encode("utf-8")


def _camera_basis(manifest: dict[str, Any]) -> tuple[
    tuple[float, float, float],
    tuple[float, float, float],
    tuple[float, float, float],
    tuple[float, float, float],
]:
    camera = manifest["camera"]
    meters_per_unit = float(manifest["coordinateSystem"]["metersPerSourceUnit"])
    position = tuple(float(value) * meters_per_unit for value in camera["positionMm"])
    forward = _normalize(_as_vec3(camera["direction"], "camera direction"))
    up_hint = _normalize(_as_vec3(camera["up"], "camera up"))
    right = _normalize(_cross(forward, up_hint))
    up = _normalize(_cross(right, forward))
    return position, forward, right, up


def _project_vertex(
    vertex: Sequence[float],
    position: Sequence[float],
    forward: Sequence[float],
    right: Sequence[float],
    up: Sequence[float],
    focal_length: float,
    film_width: float,
    film_height: float,
    width: int,
    height: int,
) -> tuple[float, float, float]:
    relative = _vector_sub(vertex, position)
    depth = _dot(relative, forward)
    _require(depth > 1.0e-6, "Cornell geometry crossed the camera plane")
    film_x = focal_length * _dot(relative, right) / depth
    film_y = focal_length * _dot(relative, up) / depth
    pixel_x = (film_x / film_width + 0.5) * width
    pixel_y = (0.5 - film_y / film_height) * height
    return (pixel_x, pixel_y, depth)


def _edge_function(a: Sequence[float], b: Sequence[float], point: Sequence[float]) -> float:
    return (point[0] - a[0]) * (b[1] - a[1]) - (point[1] - a[1]) * (b[0] - a[0])


def _rasterize_material_and_depth(
    manifest: dict[str, Any],
    triangles: Sequence[Triangle],
) -> tuple[
    bytearray,
    list[float],
    list[str | None],
    list[tuple[float, float, float] | None],
    list[tuple[float, float, float] | None],
]:
    output_contract = manifest["generatedOutputs"]
    width = int(output_contract["imageWidth"])
    height = int(output_contract["imageHeight"])
    _require(1 <= width <= 4096 and 1 <= height <= 4096, "mask dimensions are invalid")
    position, forward, right, up = _camera_basis(manifest)
    camera = manifest["camera"]
    focal_length = float(camera["focalLengthM"])
    film_width = float(camera["filmWidthM"])
    film_height = float(camera["filmHeightM"])
    material_ids = {str(material["name"]): int(material["id"]) for material in manifest["materials"]}
    material_mask = bytearray(width * height)
    depth_buffer = [math.inf] * (width * height)
    surface_mask: list[str | None] = [None] * (width * height)
    world_position_mask: list[tuple[float, float, float] | None] = [None] * (width * height)
    normal_mask: list[tuple[float, float, float] | None] = [None] * (width * height)

    for triangle in triangles:
        center = _centroid(triangle.vertices)
        if _dot(triangle.normal, _vector_sub(position, center)) <= 0.0:
            continue
        projected = [
            _project_vertex(
                vertex,
                position,
                forward,
                right,
                up,
                focal_length,
                film_width,
                film_height,
                width,
                height,
            )
            for vertex in triangle.vertices
        ]
        signed_area = _edge_function(projected[0], projected[1], projected[2])
        if abs(signed_area) <= 1.0e-10:
            continue
        min_x = max(0, int(math.floor(min(point[0] for point in projected))))
        max_x = min(width - 1, int(math.ceil(max(point[0] for point in projected))))
        min_y = max(0, int(math.floor(min(point[1] for point in projected))))
        max_y = min(height - 1, int(math.ceil(max(point[1] for point in projected))))
        material_id = material_ids[triangle.material]
        for pixel_y in range(min_y, max_y + 1):
            for pixel_x in range(min_x, max_x + 1):
                sample = (pixel_x + 0.5, pixel_y + 0.5)
                w0 = _edge_function(projected[1], projected[2], sample) / signed_area
                w1 = _edge_function(projected[2], projected[0], sample) / signed_area
                w2 = _edge_function(projected[0], projected[1], sample) / signed_area
                if min(w0, w1, w2) < -1.0e-9:
                    continue
                inverse_depth = (
                    w0 / projected[0][2]
                    + w1 / projected[1][2]
                    + w2 / projected[2][2]
                )
                if inverse_depth <= 0.0:
                    continue
                depth = 1.0 / inverse_depth
                offset = pixel_y * width + pixel_x
                if depth < depth_buffer[offset]:
                    depth_buffer[offset] = depth
                    material_mask[offset] = material_id
                    surface_mask[offset] = _canonical_surface_name(triangle.surface)
                    normal_mask[offset] = triangle.normal
                    world_position_mask[offset] = tuple(
                        (
                            w0 * triangle.vertices[0][axis] / projected[0][2]
                            + w1 * triangle.vertices[1][axis] / projected[1][2]
                            + w2 * triangle.vertices[2][axis] / projected[2][2]
                        )
                        / inverse_depth
                        for axis in range(3)
                    )
    return material_mask, depth_buffer, surface_mask, world_position_mask, normal_mask


def _ray_triangle_distance(
    origin: Sequence[float],
    direction: Sequence[float],
    triangle: Triangle,
) -> float | None:
    edge1 = _vector_sub(triangle.vertices[1], triangle.vertices[0])
    edge2 = _vector_sub(triangle.vertices[2], triangle.vertices[0])
    p = _cross(direction, edge2)
    determinant = _dot(edge1, p)
    if abs(determinant) < 1.0e-12:
        return None
    inverse_determinant = 1.0 / determinant
    offset = _vector_sub(origin, triangle.vertices[0])
    u = _dot(offset, p) * inverse_determinant
    if u < 0.0 or u > 1.0:
        return None
    q = _cross(offset, edge1)
    v = _dot(direction, q) * inverse_determinant
    if v < 0.0 or u + v > 1.0:
        return None
    distance = _dot(edge2, q) * inverse_determinant
    return distance if distance > 0.0 else None


def _make_light_area_occlusion_mask(
    triangles: Sequence[Triangle],
    surface_mask: Sequence[str | None],
    world_position_mask: Sequence[tuple[float, float, float] | None],
    origin_offset_m: float,
) -> bytearray:
    light_points = [point for triangle in triangles if triangle.material == "light" for point in triangle.vertices]
    _require(light_points, "light-area occlusion requires light geometry")
    light_minimum = tuple(min(point[axis] for point in light_points) for axis in range(3))
    light_maximum = tuple(max(point[axis] for point in light_points) for axis in range(3))
    light_samples = [
        (
            light_minimum[0] + (light_maximum[0] - light_minimum[0]) * x,
            light_minimum[1],
            light_minimum[2] + (light_maximum[2] - light_minimum[2]) * z,
        )
        for z in (0.0, 0.5, 1.0)
        for x in (0.0, 0.5, 1.0)
    ]
    blockers = [
        triangle
        for triangle in triangles
        if _canonical_surface_name(triangle.surface) in {"short_block", "tall_block"}
    ]
    result = bytearray(len(surface_mask))
    for offset, surface_name in enumerate(surface_mask):
        if surface_name != "floor":
            continue
        world_position = world_position_mask[offset]
        _require(world_position is not None, "visible floor pixel has no world position")
        origin = _vector_add(world_position, (0.0, origin_offset_m, 0.0))
        fully_occluded = True
        for light_sample in light_samples:
            to_light = _vector_sub(light_sample, origin)
            maximum_distance = _length(to_light)
            direction = _vector_scale(to_light, 1.0 / maximum_distance)
            sample_occluded = False
            for blocker in blockers:
                distance = _ray_triangle_distance(origin, direction, blocker)
                if distance is not None and distance < maximum_distance - origin_offset_m:
                    sample_occluded = True
                    break
            if not sample_occluded:
                fully_occluded = False
                break
        if fully_occluded:
            result[offset] = 1
    _require(1 in result and 0 in result, "light-area occlusion classification is degenerate")
    return result


def _make_edge_mask(
    material_mask: Sequence[int],
    depth_buffer: Sequence[float],
    surface_mask: Sequence[str | None],
    normal_mask: Sequence[tuple[float, float, float] | None],
    width: int,
    height: int,
    radius: int,
    absolute_depth_threshold_m: float,
    relative_depth_threshold: float,
) -> bytearray:
    base = bytearray(width * height)
    neighbors = ((-1, 0), (1, 0), (0, -1), (0, 1))
    for y in range(height):
        for x in range(width):
            offset = y * width + x
            material = material_mask[offset]
            for dx, dy in neighbors:
                nx = x + dx
                ny = y + dy
                if nx < 0 or nx >= width or ny < 0 or ny >= height:
                    continue
                neighbor_offset = ny * width + nx
                neighbor_material = material_mask[neighbor_offset]
                normal = normal_mask[offset]
                neighbor_normal = normal_mask[neighbor_offset]
                normal_discontinuity = (
                    normal is not None
                    and neighbor_normal is not None
                    and _dot(normal, neighbor_normal) < 0.999
                )
                if (
                    material != neighbor_material
                    or surface_mask[offset] != surface_mask[neighbor_offset]
                    or normal_discontinuity
                ):
                    base[offset] = 255
                    break
            if base[offset]:
                continue
            depth = depth_buffer[offset]
            if not math.isfinite(depth):
                continue
            # A first depth derivative is non-zero across every perspective
            # projection of a sloped plane and incorrectly classified most of
            # the floor as an edge. Opposing-neighbor second differences stay
            # near zero on one plane and spike at true depth discontinuities.
            for dx, dy in ((1, 0), (0, 1)):
                previous_x = x - dx
                previous_y = y - dy
                next_x = x + dx
                next_y = y + dy
                if not (
                    0 <= previous_x < width
                    and 0 <= previous_y < height
                    and 0 <= next_x < width
                    and 0 <= next_y < height
                ):
                    continue
                previous_depth = depth_buffer[previous_y * width + previous_x]
                next_depth = depth_buffer[next_y * width + next_x]
                if not (math.isfinite(previous_depth) and math.isfinite(next_depth)):
                    continue
                depth_curvature = abs(previous_depth - 2.0 * depth + next_depth)
                if depth_curvature > max(
                    absolute_depth_threshold_m,
                    relative_depth_threshold * depth,
                ):
                    base[offset] = 255
                    break
    if radius <= 0:
        return base
    dilated = bytearray(base)
    edge_offsets = [index for index, value in enumerate(base) if value]
    for offset in edge_offsets:
        x = offset % width
        y = offset // width
        for dy in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                nx = x + dx
                ny = y + dy
                if 0 <= nx < width and 0 <= ny < height:
                    dilated[ny * width + nx] = 255
    return dilated


def _pixel_bounds_top_left(
    offsets: Sequence[int],
    width: int,
) -> list[int]:
    _require(bool(offsets), "cannot compute bounds for an empty pixel selection")
    return [
        min(offset % width for offset in offsets),
        min(offset // width for offset in offsets),
        max(offset % width for offset in offsets) + 1,
        max(offset // width for offset in offsets) + 1,
    ]


def _make_disocclusion_validation_artifacts(
    manifest: dict[str, Any],
    triangles: Sequence[Triangle],
) -> dict[str, bytes]:
    output_contract = manifest["generatedOutputs"]
    scenario = output_contract["disocclusionValidation"]
    width = int(output_contract["imageWidth"])
    height = int(output_contract["imageHeight"])
    moving_surface = str(scenario["movingSurfaceName"])
    translation = _as_vec3(scenario["translationM"], "disocclusion translation")
    allowed_revealed_surfaces = [
        str(value) for value in scenario["allowedRevealedSurfaceNames"]
    ]
    allowed_revealed_set = set(allowed_revealed_surfaces)

    moved_triangles = [
        Triangle(
            surface=triangle.surface,
            material=triangle.material,
            vertices=tuple(
                _vector_add(vertex, translation)
                if _canonical_surface_name(triangle.surface) == moving_surface
                else vertex
                for vertex in triangle.vertices
            ),  # type: ignore[arg-type]
            normal=triangle.normal,
        )
        for triangle in triangles
    ]
    before = _rasterize_material_and_depth(manifest, triangles)
    after = _rasterize_material_and_depth(manifest, moved_triangles)
    edge_radius = int(scenario["edgeExclusionRadiusPixels"])
    before_edges = _make_edge_mask(
        before[0],
        before[1],
        before[2],
        before[4],
        width,
        height,
        edge_radius,
        float(output_contract["maskDepthEdgeAbsoluteM"]),
        float(output_contract["maskDepthEdgeRelative"]),
    )
    after_edges = _make_edge_mask(
        after[0],
        after[1],
        after[2],
        after[4],
        width,
        height,
        edge_radius,
        float(output_contract["maskDepthEdgeAbsoluteM"]),
        float(output_contract["maskDepthEdgeRelative"]),
    )

    raw_offsets = [
        offset
        for offset in range(width * height)
        if before[2][offset] == moving_surface
        and after[2][offset] in allowed_revealed_set
    ]
    clean_offsets = [
        offset
        for offset in raw_offsets
        if before_edges[offset] == 0 and after_edges[offset] == 0
    ]
    _require(
        len(raw_offsets) == int(scenario["expectedRawPixelCount"]),
        f"raw disocclusion pixel count changed: {len(raw_offsets)}",
    )
    raw_bounds = _pixel_bounds_top_left(raw_offsets, width)
    _require(
        raw_bounds == scenario["expectedRawPixelBoundsTopLeftInclusiveExclusive"],
        f"raw disocclusion bounds changed: {raw_bounds}",
    )
    _require(
        len(clean_offsets) == int(scenario["expectedPixelCount"]),
        f"edge-clean disocclusion pixel count changed: {len(clean_offsets)}",
    )
    clean_bounds = _pixel_bounds_top_left(clean_offsets, width)
    _require(
        clean_bounds == scenario["expectedPixelBoundsTopLeftInclusiveExclusive"],
        f"edge-clean disocclusion bounds changed: {clean_bounds}",
    )
    revealed_counts = {
        surface: sum(1 for offset in clean_offsets if after[2][offset] == surface)
        for surface in allowed_revealed_surfaces
    }
    _require(
        revealed_counts == scenario["expectedRevealedSurfacePixelCounts"],
        f"disocclusion revealed-surface counts changed: {revealed_counts}",
    )

    source_triangles = [
        triangle
        for triangle in triangles
        if _canonical_surface_name(triangle.surface) == moving_surface
    ]
    moved_surface_triangles = [
        triangle
        for triangle in moved_triangles
        if _canonical_surface_name(triangle.surface) == moving_surface
    ]
    tall_triangles = [
        triangle
        for triangle in triangles
        if _canonical_surface_name(triangle.surface) == "tall_block"
    ]
    source_bounds_min, source_bounds_max = _scene_bounds(source_triangles)
    moved_bounds_min, moved_bounds_max = _scene_bounds(moved_surface_triangles)
    tall_bounds_min, _tall_bounds_max = _scene_bounds(tall_triangles)
    scene_bounds_min, scene_bounds_max = _scene_bounds(triangles)
    _require(
        all(
            scene_bounds_min[axis] - 1.0e-12 <= moved_bounds_min[axis]
            and moved_bounds_max[axis] <= scene_bounds_max[axis] + 1.0e-12
            for axis in range(3)
        ),
        "moved short block leaves the Cornell room bounds",
    )
    tall_separation_m = tall_bounds_min[0] - moved_bounds_max[0]
    green_wall_clearance_m = moved_bounds_min[0] - scene_bounds_min[0]
    _require(tall_separation_m > 0.0, "moved short block intersects the tall block AABB")
    _require(green_wall_clearance_m > 0.0, "moved short block intersects the green wall")

    mask_pixels = bytearray(width * height)
    for offset in clean_offsets:
        mask_pixels[offset] = 255
    metadata = {
        "allowedRevealedSurfaceNames": allowed_revealed_surfaces,
        "coordinateConvention": "top-left origin; x right; y down; pixel bounds are [x0,y0,x1,y1] inclusive/exclusive",
        "edgeExclusion": {
            "absoluteDepthThresholdM": float(output_contract["maskDepthEdgeAbsoluteM"]),
            "radiusPixels": edge_radius,
            "relativeDepthThreshold": float(output_contract["maskDepthEdgeRelative"]),
            "rule": "Exclude the union of the before-move and after-move dilated edge masks.",
        },
        "generatorVersion": manifest["generatorVersion"],
        "greenWallClearanceM": green_wall_clearance_m,
        "height": height,
        "manifestVersion": manifest["manifestVersion"],
        "mask": DISOCCLUSION_MASK_FILENAME,
        "movedBoundsM": {"min": moved_bounds_min, "max": moved_bounds_max},
        "movingSurfaceName": moving_surface,
        "pixelBoundsTopLeftInclusiveExclusive": clean_bounds,
        "pixelCount": len(clean_offsets),
        "rawPixelBoundsTopLeftInclusiveExclusive": raw_bounds,
        "rawPixelCount": len(raw_offsets),
        "recoveryContract": {
            "consecutiveFrames": int(scenario["recoveryConsecutiveFrames"]),
            "maxRecoveryFrames": int(scenario["maxRecoveryFrames"]),
            "postMoveCaptureFrames": int(scenario["postMoveCaptureFrames"]),
            "referenceMaxBounces": int(scenario["referenceMaxBounces"]),
            "referenceSamples": int(scenario["referenceSamples"]),
            "relativeL1Threshold": float(scenario["recoveryThreshold"]),
        },
        "revealedSurfacePixelCounts": revealed_counts,
        "scenario": str(scenario["name"]),
        "schema": "https://henginedev.local/schemas/cornell-disocclusion-roi/v1",
        "selectionRule": "Before first-hit surface is short_block; after first-hit surface is an allowed static revealed surface; both before and after edge masks are zero.",
        "sourceBoundsM": {"min": source_bounds_min, "max": source_bounds_max},
        "tallBlockSeparationM": tall_separation_m,
        "translationM": list(translation),
        "width": width,
    }
    return {
        DISOCCLUSION_MASK_FILENAME: _pgm_bytes(
            width,
            height,
            mask_pixels,
            "exact short-block -50 mm X disocclusion ROI; before/after edges excluded",
        ),
        DISOCCLUSION_METADATA_FILENAME: _json_bytes(metadata),
    }


def _make_roi_mask_and_metadata(
    manifest: dict[str, Any],
    material_mask: Sequence[int],
    edge_mask: Sequence[int],
    surface_mask: Sequence[str | None],
    light_area_occlusion_mask: Sequence[int],
) -> tuple[bytearray, dict[str, Any]]:
    output_contract = manifest["generatedOutputs"]
    width = int(output_contract["imageWidth"])
    height = int(output_contract["imageHeight"])
    roi_mask = bytearray(width * height)
    labels: list[dict[str, Any]] = []
    seen_ids: set[int] = set()
    for roi in output_contract["rois"]:
        roi_id = int(roi["id"])
        _require(1 <= roi_id <= 254 and roi_id not in seen_ids, "ROI IDs must be unique bytes")
        seen_ids.add(roi_id)
        rectangle = roi["normalizedRectTopLeft"]
        _require(isinstance(rectangle, list) and len(rectangle) == 4, "ROI rectangle must have four values")
        x0, y0, x1, y1 = (float(value) for value in rectangle)
        _require(0.0 <= x0 < x1 <= 1.0 and 0.0 <= y0 < y1 <= 1.0, "ROI rectangle is outside [0,1]")
        min_x = max(0, min(width, int(math.floor(x0 * width))))
        max_x = max(0, min(width, int(math.ceil(x1 * width))))
        min_y = max(0, min(height, int(math.floor(y0 * height))))
        max_y = max(0, min(height, int(math.ceil(y1 * height))))
        allowed_materials = {int(value) for value in roi["allowedMaterialIds"]}
        allowed_surfaces = {str(value) for value in roi["allowedSurfaceNames"]}
        light_area_visibility = str(roi.get("lightAreaVisibility", "any"))
        _require(light_area_visibility in {"any", "fullyOccluded", "notFullyOccluded"}, "ROI light-area visibility rule is invalid")
        pixel_count = 0
        for y in range(min_y, max_y):
            for x in range(min_x, max_x):
                offset = y * width + x
                if (
                    edge_mask[offset] == 0
                    and material_mask[offset] in allowed_materials
                    and surface_mask[offset] in allowed_surfaces
                    and (
                        light_area_visibility == "any"
                        or (light_area_visibility == "fullyOccluded" and light_area_occlusion_mask[offset] != 0)
                        or (light_area_visibility == "notFullyOccluded" and light_area_occlusion_mask[offset] == 0)
                    )
                    and roi_mask[offset] == 0
                ):
                    roi_mask[offset] = roi_id
                    pixel_count += 1
        _require(pixel_count >= 64, f"ROI {roi['name']} is empty or too small after edge exclusion ({pixel_count} pixels)")
        labels.append(
            {
                "allowedMaterialIds": sorted(allowed_materials),
                "allowedSurfaceNames": sorted(allowed_surfaces),
                "id": roi_id,
                "lightAreaVisibility": light_area_visibility,
                "name": str(roi["name"]),
                "binaryMask": f"CornellBox.roi-{str(roi['name'])}.pgm",
                "normalizedRectTopLeft": [x0, y0, x1, y1],
                "pixelBoundsTopLeftInclusiveExclusive": [min_x, min_y, max_x, max_y],
                "pixelCount": pixel_count,
            }
        )
    meters_per_unit = float(manifest["coordinateSystem"]["metersPerSourceUnit"])
    light_quad_m = [
        _vector_scale(_as_vec3(vertex, "light vertex"), meters_per_unit)
        for vertex in _surface_source(manifest, "light")["quadsMm"][0]
    ]
    metadata = {
        "coordinateConvention": "top-left origin; x right; y down; normalized rectangles are [x0,y0,x1,y1]",
        "edgeExclusion": {
            "absoluteDepthThresholdM": float(output_contract["maskDepthEdgeAbsoluteM"]),
            "mask": EDGE_MASK_FILENAME,
            "radiusPixels": int(output_contract["maskEdgeRadiusPixels"]),
            "relativeDepthThreshold": float(output_contract["maskDepthEdgeRelative"]),
            "rule": "ROI pixels are selected only where the dilated edge mask is zero.",
        },
        "height": height,
        "labels": labels,
        "lightAreaOcclusion": {
            "blockerSurfaceNames": ["short_block", "tall_block"],
            "lightSampleGrid": "3x3 including the measured rectangular-light boundary and center",
            "originOffsetM": float(output_contract["lightOcclusionOriginOffsetM"]),
            "rule": "Two-sided segment intersections from the visible floor point offset +Y to all 3x3 measured light-quad samples; every sample must hit a short/tall block before the light to mark fully occluded.",
        },
        "manifestVersion": manifest["manifestVersion"],
        "materialIdMask": MATERIAL_MASK_FILENAME,
        "materialIdLabels": [
            {"id": 0, "name": "background"},
            *[
                {
                    "id": int(material["id"]),
                    "name": str(material["name"]),
                    "binaryMask": f"CornellBox.material-{str(material['name'])}.pgm",
                }
                for material in manifest["materials"]
            ],
        ],
        "roiMask": ROI_MASK_FILENAME,
        "width": width,
    }
    return roi_mask, metadata


def _pgm_bytes(width: int, height: int, pixels: bytes | bytearray, comment: str) -> bytes:
    _require(len(pixels) == width * height, "PGM payload size mismatch")
    _require("\n" not in comment and "\r" not in comment, "PGM comment must be one line")
    return f"P5\n# {comment}\n{width} {height}\n255\n".encode("ascii") + bytes(pixels)


def _make_masks(
    manifest: dict[str, Any],
    triangles: Sequence[Triangle],
) -> tuple[bytes, bytes, bytes, bytes, dict[str, bytes]]:
    output_contract = manifest["generatedOutputs"]
    width = int(output_contract["imageWidth"])
    height = int(output_contract["imageHeight"])
    material_mask, depth_buffer, surface_mask, world_position_mask, normal_mask = _rasterize_material_and_depth(
        manifest,
        triangles,
    )
    edge_mask = _make_edge_mask(
        material_mask,
        depth_buffer,
        surface_mask,
        normal_mask,
        width,
        height,
        int(output_contract["maskEdgeRadiusPixels"]),
        float(output_contract["maskDepthEdgeAbsoluteM"]),
        float(output_contract["maskDepthEdgeRelative"]),
    )
    light_area_occlusion_mask = _make_light_area_occlusion_mask(
        triangles,
        surface_mask,
        world_position_mask,
        float(output_contract["lightOcclusionOriginOffsetM"]),
    )
    roi_mask, roi_metadata = _make_roi_mask_and_metadata(
        manifest,
        material_mask,
        edge_mask,
        surface_mask,
        light_area_occlusion_mask,
    )
    _require(set(material_mask) == {0, 1, 2, 3, 4}, f"material mask does not contain all stable IDs: {sorted(set(material_mask))}")
    _require(255 in edge_mask and 0 in edge_mask, "edge mask must contain edge and interior pixels")
    named_masks: dict[str, bytes] = {}
    for material in manifest["materials"]:
        material_id = int(material["id"])
        material_name = str(material["name"])
        named_masks[f"CornellBox.material-{material_name}.pgm"] = _pgm_bytes(
            width,
            height,
            bytes(255 if value == material_id else 0 for value in material_mask),
            f"binary material ROI for {material_name}",
        )
    named_masks["CornellBox.material-non_emissive.pgm"] = _pgm_bytes(
        width,
        height,
        bytes(255 if value in {1, 2, 3} else 0 for value in material_mask),
        "binary non-emissive Cornell material ROI",
    )
    for roi in output_contract["rois"]:
        roi_id = int(roi["id"])
        roi_name = str(roi["name"])
        named_masks[f"CornellBox.roi-{roi_name}.pgm"] = _pgm_bytes(
            width,
            height,
            bytes(255 if value == roi_id else 0 for value in roi_mask),
            f"binary Cornell ROI for {roi_name}",
        )
    named_masks.update(
        _make_disocclusion_validation_artifacts(manifest, triangles)
    )
    return (
        _pgm_bytes(width, height, material_mask, "stable Cornell material IDs: 0 background, 1 white, 2 green, 3 red, 4 light"),
        _pgm_bytes(width, height, edge_mask, "255 excludes silhouettes and material/depth discontinuities; already dilated"),
        _pgm_bytes(width, height, roi_mask, "ROI labels are defined by CornellBox.roi.json; 0 is outside all ROIs"),
        _json_bytes(roi_metadata),
        named_masks,
    )


def _decode_accessor(gltf: dict[str, Any], binary: bytes, accessor_index: int) -> list[tuple[float, ...] | int]:
    accessor = gltf["accessors"][accessor_index]
    view = gltf["bufferViews"][accessor["bufferView"]]
    offset = int(view.get("byteOffset", 0)) + int(accessor.get("byteOffset", 0))
    count = int(accessor["count"])
    component_type = int(accessor["componentType"])
    value_type = accessor["type"]
    component_format = {5123: "H", 5126: "f"}[component_type]
    component_count = {"SCALAR": 1, "VEC3": 3}[value_type]
    stride = struct.calcsize("<" + component_format * component_count)
    result: list[tuple[float, ...] | int] = []
    for index in range(count):
        unpacked = struct.unpack_from("<" + component_format * component_count, binary, offset + index * stride)
        result.append(int(unpacked[0]) if component_count == 1 else tuple(float(value) for value in unpacked))
    return result


def _validate_gltf(
    gltf_bytes: bytes,
    binary: bytes,
    manifest: dict[str, Any],
    triangles: Sequence[Triangle],
    engine_rgb: dict[str, tuple[float, float, float]],
) -> None:
    gltf = json.loads(gltf_bytes.decode("utf-8"))
    _require(gltf["nodes"] == [{"extras": {"identityTransformRequired": True}, "matrix": IDENTITY_MATRIX, "mesh": 0, "name": "CornellBox_Identity"}], "glTF must contain one identity mesh node")
    _require(gltf["buffers"][0]["byteLength"] == len(binary), "glTF buffer length mismatch")
    _require(gltf["buffers"][0]["uri"] == BUFFER_FILENAME, "glTF buffer URI changed")
    _require(gltf["extensionsUsed"] == ["KHR_materials_emissive_strength"], "emissive extension contract changed")
    _require(len(gltf["materials"]) == 4 and len(gltf["meshes"][0]["primitives"]) == 4, "glTF material primitive slots changed")
    light_material = gltf["materials"][3]
    _require(light_material["name"] == "light", "light material slot changed")
    strength = light_material["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"]
    _require(abs(float(strength) - float(manifest["engineParity"]["emissiveStrength"])) < 1.0e-12, "glTF emissive strength mismatch")
    _require(_almost_equal_sequence(light_material["emissiveFactor"], engine_rgb["light"], 2.0e-8), "glTF light chromaticity mismatch")

    decoded_vertex_count = 0
    decoded_triangle_count = 0
    for primitive in gltf["meshes"][0]["primitives"]:
        positions = _decode_accessor(gltf, binary, primitive["attributes"]["POSITION"])
        normals = _decode_accessor(gltf, binary, primitive["attributes"]["NORMAL"])
        indices = _decode_accessor(gltf, binary, primitive["indices"])
        _require(len(positions) == len(normals) == len(indices), "hard-split glTF stream lengths differ")
        _require(indices == list(range(len(indices))), "hard-split glTF indices must be sequential")
        _require(len(indices) % 3 == 0, "glTF index count is not triangular")
        decoded_vertex_count += len(positions)
        decoded_triangle_count += len(indices) // 3
        for index in range(0, len(indices), 3):
            triangle_positions = [positions[index + offset] for offset in range(3)]
            triangle_normals = [normals[index + offset] for offset in range(3)]
            _require(all(isinstance(value, tuple) for value in triangle_positions), "position accessor decode failed")
            _require(all(isinstance(value, tuple) for value in triangle_normals), "normal accessor decode failed")
            geometric = _triangle_normal(triangle_positions)  # type: ignore[arg-type]
            for shading in triangle_normals:
                _require(_almost_equal_sequence(geometric, shading, 2.0e-6), "glTF contains a smoothed or incorrect normal")  # type: ignore[arg-type]
    _require(decoded_triangle_count == len(triangles), "glTF triangle count mismatch")
    _require(decoded_vertex_count == len(triangles) * 3, "glTF hard-split vertex count mismatch")


def generate_artifacts(manifest_path: Path) -> tuple[dict[str, bytes], dict[str, Any]]:
    manifest_bytes = _canonical_text_bytes(manifest_path)
    manifest = json.loads(manifest_bytes.decode("utf-8"))
    _require(isinstance(manifest, dict), "manifest root must be an object")
    derived = validate_manifest(manifest)
    triangles: list[Triangle] = derived["triangles"]
    engine_rgb = derive_engine_rgb(manifest)
    manifest_sha256 = _sha256(manifest_bytes)
    generator_sha256 = _sha256(_canonical_text_bytes(SCRIPT_PATH))

    gltf_bytes, binary = _make_gltf(
        manifest,
        triangles,
        engine_rgb,
        manifest_sha256,
        generator_sha256,
    )
    measured_pbrt = _make_pbrt(
        manifest,
        triangles,
        engine_rgb,
        manifest_sha256,
        generator_sha256,
        mode="measured",
    )
    parity_proxy_pbrt = _make_pbrt(
        manifest,
        triangles,
        engine_rgb,
        manifest_sha256,
        generator_sha256,
        mode="spectral-proxy",
    )
    parity_channel_pbrt = {
        channel_name: _make_pbrt(
            manifest,
            triangles,
            engine_rgb,
            manifest_sha256,
            generator_sha256,
            mode="channel",
            channel_name=channel_name,
        )
        for channel_name in ("R", "G", "B")
    }
    hengine_scene = _make_hengine_scene(manifest)
    material_mask, edge_mask, roi_mask, roi_metadata, named_masks = _make_masks(
        manifest,
        triangles,
    )

    artifacts = {
        GLTF_FILENAME: gltf_bytes,
        BUFFER_FILENAME: binary,
        MEASURED_PBRT_FILENAME: measured_pbrt,
        PARITY_PROXY_PBRT_FILENAME: parity_proxy_pbrt,
        MATERIAL_MASK_FILENAME: material_mask,
        EDGE_MASK_FILENAME: edge_mask,
        ROI_MASK_FILENAME: roi_mask,
        ROI_METADATA_FILENAME: roi_metadata,
        HENGINE_SCENE_FILENAME: hengine_scene,
    }
    artifacts.update(
        {
            PARITY_CHANNEL_PBRT_FILENAMES[channel_name]: payload
            for channel_name, payload in parity_channel_pbrt.items()
        }
    )
    artifacts.update(named_masks)
    hashes = {
        "files": {filename: _sha256(payload) for filename, payload in sorted(artifacts.items())},
        "generatorSha256": generator_sha256,
        "generatorSha256Scope": "UTF-8 generator text with all line endings normalized to LF",
        "generatorVersion": manifest["generatorVersion"],
        "manifest": manifest_path.name,
        "manifestSha256": manifest_sha256,
        "manifestSha256Scope": "UTF-8 manifest text with all line endings normalized to LF",
        "schema": "https://henginedev.local/schemas/cornell-generated-output-hashes/v1",
    }
    artifacts[OUTPUT_HASH_FILENAME] = _json_bytes(hashes)
    _validate_gltf(gltf_bytes, binary, manifest, triangles, engine_rgb)
    scene_document = json.loads(hengine_scene.decode("utf-8"))
    mesh_entities = [
        entity for entity in scene_document.values() if "MeshComponent" in entity
    ]
    camera_entities = [
        entity for entity in scene_document.values() if "CameraComponent" in entity
    ]
    _require(len(mesh_entities) == 1, "HEngine scene must contain one identity mesh entity")
    _require(
        mesh_entities[0]["TransformComponent"] == {
            "Rotation": [0.0, 0.0, 0.0],
            "Scale": [1.0, 1.0, 1.0],
            "Translation": [0.0, 0.0, 0.0],
        },
        "HEngine Cornell mesh transform is not identity",
    )
    _require(len(camera_entities) == 1, "HEngine scene must contain one primary camera")
    _require(
        camera_entities[0]["CameraComponent"]["FixedAspectRatio"] is True,
        "HEngine Cornell camera must preserve the square official film",
    )

    light_radiance = [
        value * float(manifest["engineParity"]["emissiveStrength"])
        for value in engine_rgb["light"]
    ]
    luminance_coefficients = manifest["engineParity"]["emissiveTriangleSampling"]["luminanceCoefficients"]
    light_luminance = sum(
        float(light_radiance[index]) * float(luminance_coefficients[index])
        for index in range(3)
    )
    summary = {
        "boundsM": {"min": derived["boundsMin"], "max": derived["boundsMax"]},
        "engineLinearRgb": {name: [_round_json_float(value) for value in rgb] for name, rgb in engine_rgb.items()},
        "fileSha256": hashes["files"],
        "lightAreaM2": derived["lightAreaM2"],
        "lightDistributionWeight": derived["lightAreaM2"] * light_luminance,
        "generatorSha256": generator_sha256,
        "manifestSha256": manifest_sha256,
        "triangleCount": len(triangles),
        "vertexCount": len(triangles) * 3,
        "verticalFovDegrees": derived["verticalFovDegrees"],
    }
    return artifacts, summary


def _write_artifacts(output_directory: Path, artifacts: dict[str, bytes]) -> None:
    output_directory.mkdir(parents=True, exist_ok=True)
    expected_names = set(artifacts)
    for filename, payload in artifacts.items():
        destination = output_directory / filename
        temporary = destination.with_name(destination.name + ".tmp")
        temporary.write_bytes(payload)
        temporary.replace(destination)
    unexpected = [path for path in output_directory.iterdir() if path.is_file() and path.name not in expected_names]
    _require(not unexpected, "generated directory contains unmanaged files: " + ", ".join(path.name for path in unexpected))


def _check_artifacts(output_directory: Path, artifacts: dict[str, bytes]) -> None:
    errors: list[str] = []
    expected_names = set(artifacts)
    for filename, expected in artifacts.items():
        path = output_directory / filename
        if not path.is_file():
            errors.append(f"missing {filename}")
            continue
        actual = path.read_bytes()
        if actual != expected:
            errors.append(f"stale {filename}: expected {_sha256(expected)}, found {_sha256(actual)}")
    if output_directory.is_dir():
        for path in output_directory.iterdir():
            if path.is_file() and path.name not in expected_names:
                errors.append(f"unmanaged generated file {path.name}")
    _require(not errors, "generated Cornell outputs failed byte validation:\n  " + "\n  ".join(errors))


def main() -> int:
    arguments = _parse_arguments()
    manifest_path = arguments.manifest.resolve()
    output_directory = arguments.output_dir.resolve()
    artifacts, summary = generate_artifacts(manifest_path)
    # Rebuild in memory to guard against iteration-order or hidden state leaks.
    second_artifacts, second_summary = generate_artifacts(manifest_path)
    _require(artifacts == second_artifacts and summary == second_summary, "generator is not deterministic within one process")
    if arguments.check:
        _check_artifacts(output_directory, artifacts)
        action = "verified"
    else:
        _write_artifacts(output_directory, artifacts)
        action = "generated"
    if arguments.print_derived:
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        print(
            f"Cornell benchmark {action}: {len(artifacts)} files, "
            f"{summary['triangleCount']} triangles, {summary['vertexCount']} hard-split vertices, "
            f"manifest {summary['manifestSha256']}"
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as exception:
        print(f"Cornell benchmark generation failed: {exception}", file=sys.stderr)
        raise SystemExit(1)
