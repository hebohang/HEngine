#!/usr/bin/env python3
"""Generate editable, bounded CPU projectile VFX from reviewed local CC0 sources.

The three outputs deliberately use new names.  They never replace the legacy
Survivor packages, and every byte of third-party artwork is verified against a
reviewed ACCEPT/CC0 intake record before it is copied into a derived package.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple
import xml.etree.ElementTree as ET
import zipfile


FIXED_ZIP_TIME = (2026, 8, 23, 0, 0, 0)
EFFEKSEER_VERSION = "1.80.6"
OUTPUT_ROOT = Path("Project/Assets/Effects/Survivor")
DERIVED_ROOT = Path(
    "Project/SourceAssets/EffekseerInbox/survivor-toon-projectiles-v2"
)
STALE_DERIVED_FILES = (
    Path("derived/PetalStarburstToon/Model/14.fbx"),
    Path("derived/PetalStarburstToon/Model/14.efkmodel"),
    Path("derived/PetalStarburstToon/Texture/sakura-1.png"),
    Path("derived/PetalStarburstToon/Texture/Particle_Soft.png"),
    Path("derived/PetalStarburstToon/Texture/particle2.png"),
    Path(
        "derived/PetalStarburstToon/LICENSES/"
        "effekseer01-official-CC0-1.0.txt"
    ),
)

INTAKES = {
    "effekseer01-official": {
        "manifest": Path(
            "Project/SourceAssets/EffekseerInbox/effekseer01-official/asset.json"
        ),
        "license": Path(
            "Project/SourceAssets/EffekseerInbox/effekseer01-official/LICENSE.txt"
        ),
        "license_sha256": (
            "59ab6b0767014a80f8a1487278bd737fc04946bf0b5c4fe43b1df6510af99e4c"
        ),
    },
    "hato01": {
        "manifest": Path(
            "Project/SourceAssets/EffekseerInbox/hato01/asset.json"
        ),
        "license": Path(
            "Project/SourceAssets/EffekseerInbox/hato01/LICENSE.txt"
        ),
        "license_sha256": (
            "b0f64e438a10952e4059f5d4bbe0a450e190a3dc758a0ca611433e29559e249d"
        ),
    },
    "pierre02": {
        "manifest": Path(
            "Project/SourceAssets/EffekseerInbox/pierre02/asset.json"
        ),
        "license": Path(
            "Project/SourceAssets/EffekseerInbox/pierre02/LICENSE.txt"
        ),
        "license_sha256": (
            "d3f89b5877210437dcde35fd97214dccf2918547d925488883209a1ea00af22c"
        ),
    },
}

SOURCES = {
    "base_project": (
        Path(
            "Project/SourceAssets/EffekseerInbox/pierre02/source/"
            "Pierre02/FireBall.efkproj"
        ),
        "258d3d953740a9c7739edebc0f6992a972185a41630fd4ba5d53978c4becb490",
        "pierre02",
    ),
    "official_glow": (
        Path(
            "Project/SourceAssets/EffekseerInbox/effekseer01-official/source/"
            "Effekseer01/Texture/Particle04_clear_soft.png"
        ),
        "1abd57329f048e4f1d9deb447d5f769202dfab7219c848998df0b7b1f7ac6a5a",
        "effekseer01-official",
    ),
    "official_mote": (
        Path(
            "Project/SourceAssets/EffekseerInbox/effekseer01-official/source/"
            "Effekseer01/Texture/Particle01.png"
        ),
        "b1346d2bfafa6e783661453680b282edc6bc0e0b85711414b9914aac03a3a881",
        "effekseer01-official",
    ),
    "hato_wing": (
        Path(
            "Project/SourceAssets/EffekseerInbox/hato01/source/"
            "HATO-Springhascome/Butterfliesflutter/texture/aoi-hane.png"
        ),
        "60fcefd84bab54978979d3b6d2d811980e9011acc7468683b73d2057e094011a",
        "hato01",
    ),
    "pierre_feather": (
        Path(
            "Project/SourceAssets/EffekseerInbox/pierre02/source/"
            "Pierre02/Texture/Feather.png"
        ),
        "133bf745deea947f8b5ea75b5daa2931c86f2139cc2ab23143f017e09622e1ad",
        "pierre02",
    ),
    "pierre_star": (
        Path(
            "Project/SourceAssets/EffekseerInbox/pierre02/source/"
            "Pierre02/Texture/Star.png"
        ),
        "66e7da511593b77a572348946e790b7e5058ae94835233ae698ec76c2fb6fee3",
        "pierre02",
    ),
    "pierre_soft": (
        Path(
            "Project/SourceAssets/EffekseerInbox/pierre02/source/"
            "Pierre02/Texture/Particle_Soft.png"
        ),
        "e4ba9d50691c9848b7f99cbe1b77b8f78249ef4d9fbb555f16b881d545a6d0db",
        "pierre02",
    ),
    "pierre_ring": (
        Path(
            "Project/SourceAssets/EffekseerInbox/pierre02/source/"
            "Pierre02/Texture/Shockwave.png"
        ),
        "1cbb9e0dd7ff34deda5ab89895ed6ba04dfd7f605f7fda6089ac007c1db26090",
        "pierre02",
    ),
    "pierre_fire": (
        Path(
            "Project/SourceAssets/EffekseerInbox/pierre02/source/"
            "Pierre02/Texture/Fire_Single.png"
        ),
        "18240f4ab6e124c7763a4b3fae59c010ad09c3c1964088a014b8cb9532db0fa7",
        "pierre02",
    ),
    "pierre_fire_blur": (
        Path(
            "Project/SourceAssets/EffekseerInbox/pierre02/source/"
            "Pierre02/Texture/Fire_Blur.png"
        ),
        "157f6de7c6d6fa8f508b0f6ce442b078ab66ec05a70472d0cbd8b91494c04895",
        "pierre02",
    ),
    "pierre_splash": (
        Path(
            "Project/SourceAssets/EffekseerInbox/pierre02/source/"
            "Pierre02/Texture/Splash.png"
        ),
        "e51d5df1a6477b3e5213ffa889b1c72b48bc7311faeb5130d2c6a80ea350a554",
        "pierre02",
    ),
}


class GenerationError(RuntimeError):
    pass


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_verified(root: Path, source_id: str) -> bytes:
    relative, expected, _ = SOURCES[source_id]
    path = root / relative
    if not path.is_file():
        raise GenerationError("required reviewed source is missing: {0}".format(path))
    data = path.read_bytes()
    actual = sha256_bytes(data)
    if actual != expected:
        raise GenerationError(
            "reviewed source hash mismatch for {0}: expected {1}, got {2}".format(
                relative, expected, actual
            )
        )
    return data


def validate_intakes(root: Path) -> Dict[str, Mapping[str, object]]:
    manifests: Dict[str, Mapping[str, object]] = {}
    for asset_id, paths in INTAKES.items():
        manifest_path = root / paths["manifest"]
        license_path = root / paths["license"]
        if not manifest_path.is_file() or not license_path.is_file():
            raise GenerationError("reviewed intake is incomplete: {0}".format(asset_id))
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        checks = {
            "asset_id": asset_id,
            "license_decision": "ACCEPT",
            "license_identifier": "CC0-1.0",
            "commercial_use": True,
            "modification_allowed": True,
        }
        for field, expected in checks.items():
            if manifest.get(field) != expected:
                raise GenerationError(
                    "intake {0} has unsafe {1}: expected {2!r}, got {3!r}".format(
                        asset_id, field, expected, manifest.get(field)
                    )
                )
        actual_license_hash = sha256_bytes(license_path.read_bytes())
        if actual_license_hash != paths["license_sha256"]:
            raise GenerationError(
                "license evidence changed for {0}: expected {1}, got {2}".format(
                    asset_id, paths["license_sha256"], actual_license_hash
                )
            )
        manifests[asset_id] = manifest
    return manifests


def add_text(parent: ET.Element, name: str, value: object) -> ET.Element:
    child = ET.SubElement(parent, name)
    if isinstance(value, bool):
        child.text = "True" if value else "False"
    else:
        child.text = str(value)
    return child


def add_range(
    parent: ET.Element,
    name: str,
    center: float,
    minimum: Optional[float] = None,
    maximum: Optional[float] = None,
) -> ET.Element:
    if minimum is None:
        minimum = center
    if maximum is None:
        maximum = center
    value = ET.SubElement(parent, name)
    add_text(value, "Center", center)
    add_text(value, "Max", maximum)
    add_text(value, "Min", minimum)
    return value


def add_vector_ranges(
    parent: ET.Element,
    name: str,
    centers: Sequence[float],
    minimums: Optional[Sequence[float]] = None,
    maximums: Optional[Sequence[float]] = None,
) -> ET.Element:
    if minimums is None:
        minimums = centers
    if maximums is None:
        maximums = centers
    vector = ET.SubElement(parent, name)
    for axis, center, minimum, maximum in zip(
        ("X", "Y", "Z"), centers, minimums, maximums
    ):
        add_range(vector, axis, center, minimum, maximum)
    return vector


def common_values(
    max_generation: int,
    life: int,
    generation_time: float = 9999.0,
    generation_offset: float = 0.0,
) -> ET.Element:
    common = ET.Element("CommonValues")
    maximum = ET.SubElement(common, "MaxGeneration")
    add_text(maximum, "Value", max_generation)
    add_text(maximum, "Infinite", False)
    add_text(common, "LocationEffectType", 2)
    add_text(common, "RotationEffectType", 2)
    add_text(common, "ScaleEffectType", 2)
    add_text(common, "RemoveWhenLifeIsExtinct", True)
    add_text(common, "RemoveWhenParentIsRemoved", False)
    add_text(common, "RemoveWhenAllChildrenAreRemoved", False)
    add_range(common, "Life", life)
    add_range(common, "GenerationTime", generation_time)
    add_range(common, "GenerationTimeOffset", generation_offset)
    return common


def location_values(
    location: Sequence[float] = (0.0, 0.0, 0.0),
    location_min: Optional[Sequence[float]] = None,
    location_max: Optional[Sequence[float]] = None,
    velocity: Sequence[float] = (0.0, 0.0, 0.0),
    velocity_min: Optional[Sequence[float]] = None,
    velocity_max: Optional[Sequence[float]] = None,
    acceleration: Sequence[float] = (0.0, 0.0, 0.0),
) -> ET.Element:
    values = ET.Element("LocationValues")
    add_text(values, "Type", 1)
    pva = ET.SubElement(values, "PVA")
    add_vector_ranges(pva, "Location", location, location_min, location_max)
    add_vector_ranges(pva, "Velocity", velocity, velocity_min, velocity_max)
    add_vector_ranges(pva, "Acceleration", acceleration)
    return values


def rotation_values(
    rotation: Sequence[float] = (0.0, 0.0, 0.0),
    rotation_min: Optional[Sequence[float]] = None,
    rotation_max: Optional[Sequence[float]] = None,
    velocity: Sequence[float] = (0.0, 0.0, 0.0),
    velocity_min: Optional[Sequence[float]] = None,
    velocity_max: Optional[Sequence[float]] = None,
) -> ET.Element:
    values = ET.Element("RotationValues")
    add_text(values, "Type", 1)
    pva = ET.SubElement(values, "PVA")
    add_vector_ranges(pva, "Rotation", rotation, rotation_min, rotation_max)
    add_vector_ranges(pva, "Velocity", velocity, velocity_min, velocity_max)
    add_vector_ranges(pva, "Acceleration", (0.0, 0.0, 0.0))
    return values


def scaling_values(
    start: Sequence[float],
    end: Sequence[float],
) -> ET.Element:
    values = ET.Element("ScalingValues")
    add_text(values, "Type", 2)
    easing = ET.SubElement(values, "Easing")
    add_vector_ranges(easing, "Start", start)
    add_vector_ranges(easing, "End", end)
    add_text(easing, "StartSpeed", 0)
    add_text(easing, "EndSpeed", 0)
    return values


def renderer_common(
    texture: str,
    alpha_blend: int,
    fade_in: int,
    fade_out: int,
    uv: Optional[Tuple[int, int, int, int]] = None,
) -> ET.Element:
    values = ET.Element("RendererCommonValues")
    add_text(values, "ColorTexture", texture)
    add_text(values, "AlphaBlend", alpha_blend)
    if fade_in > 0:
        add_text(values, "FadeInType", 1)
        fade = ET.SubElement(values, "FadeIn")
        add_text(fade, "Frame", fade_in)
    if fade_out > 0:
        add_text(values, "FadeOutType", 1)
        fade = ET.SubElement(values, "FadeOut")
        add_text(fade, "Frame", fade_out)
    if uv is not None:
        add_text(values, "UV", 1)
        fixed = ET.SubElement(values, "UVFixed")
        start = ET.SubElement(fixed, "Start")
        add_text(start, "X", uv[0])
        add_text(start, "Y", uv[1])
        size = ET.SubElement(fixed, "Size")
        add_text(size, "X", uv[2])
        add_text(size, "Y", uv[3])
    return values


def sprite_drawing(
    color: Tuple[int, int, int, int],
    billboard: int = 0,
) -> ET.Element:
    drawing = ET.Element("DrawingValues")
    add_text(drawing, "Type", 2)
    sprite = ET.SubElement(drawing, "Sprite")
    add_text(sprite, "RenderingOrder", 0)
    add_text(sprite, "Billboard", billboard)
    add_text(sprite, "ColorAll", 0)
    fixed = ET.SubElement(sprite, "ColorAll_Fixed")
    for name, value in zip(("R", "G", "B", "A"), color):
        add_text(fixed, name, value)
    return drawing


def make_sprite(
    name: str,
    texture: str,
    color: Tuple[int, int, int, int],
    start_scale: Sequence[float],
    end_scale: Sequence[float],
    life: int,
    max_generation: int = 1,
    generation_time: float = 9999.0,
    generation_offset: float = 0.0,
    location: Sequence[float] = (0.0, 0.0, 0.0),
    location_min: Optional[Sequence[float]] = None,
    location_max: Optional[Sequence[float]] = None,
    velocity: Sequence[float] = (0.0, 0.0, 0.0),
    velocity_min: Optional[Sequence[float]] = None,
    velocity_max: Optional[Sequence[float]] = None,
    rotation: Sequence[float] = (0.0, 0.0, 0.0),
    rotation_min: Optional[Sequence[float]] = None,
    rotation_max: Optional[Sequence[float]] = None,
    rotation_velocity: Sequence[float] = (0.0, 0.0, 0.0),
    rotation_velocity_min: Optional[Sequence[float]] = None,
    rotation_velocity_max: Optional[Sequence[float]] = None,
    alpha_blend: int = 2,
    fade_in: int = 1,
    fade_out: int = 10,
    billboard: int = 0,
    uv: Optional[Tuple[int, int, int, int]] = None,
) -> ET.Element:
    node = ET.Element("Node")
    node.append(
        common_values(max_generation, life, generation_time, generation_offset)
    )
    node.append(
        location_values(
            location,
            location_min,
            location_max,
            velocity,
            velocity_min,
            velocity_max,
        )
    )
    node.append(
        rotation_values(
            rotation,
            rotation_min,
            rotation_max,
            rotation_velocity,
            rotation_velocity_min,
            rotation_velocity_max,
        )
    )
    node.append(scaling_values(start_scale, end_scale))
    node.append(renderer_common(texture, alpha_blend, fade_in, fade_out, uv))
    node.append(sprite_drawing(color, billboard))
    add_text(node, "IsRendered", True)
    add_text(node, "Name", name)
    ET.SubElement(node, "Children")
    return node


def replace_text(root: ET.Element, path: str, value: object) -> None:
    node = root.find(path)
    if node is None:
        raise GenerationError("base project is missing XML path: {0}".format(path))
    node.text = "True" if value is True else "False" if value is False else str(value)


def build_project(
    base_project: bytes,
    nodes: Iterable[ET.Element],
    end_frame: int,
) -> bytes:
    root = ET.fromstring(base_project)
    project_root = root.find("Root")
    if project_root is None:
        raise GenerationError("base project has no Root")
    old_children = project_root.find("Children")
    if old_children is None:
        raise GenerationError("base project has no Root/Children")
    new_children = ET.Element("Children")
    for node in nodes:
        new_children.append(node)
    child_index = list(project_root).index(old_children)
    project_root.remove(old_children)
    project_root.insert(child_index, new_children)

    replace_text(root, "EndFrame", end_frame)
    replace_text(root, "IsLoop", False)
    culling_type = root.find("Culling/Type")
    if culling_type is not None:
        culling_type.text = "0"
    indent_xml(root)
    return (
        b'<?xml version="1.0" encoding="utf-8"?>\n'
        + ET.tostring(root, encoding="utf-8")
    )


def indent_xml(element: ET.Element, level: int = 0) -> None:
    indent = "\n" + level * "  "
    child_indent = "\n" + (level + 1) * "  "
    if len(element):
        if not element.text or not element.text.strip():
            element.text = child_indent
        for child in element:
            indent_xml(child, level + 1)
        if not element[-1].tail or not element[-1].tail.strip():
            element[-1].tail = indent
    if level and (not element.tail or not element.tail.strip()):
        element.tail = indent


def bubble_nodes() -> List[ET.Element]:
    return [
        make_sprite(
            "BubbleGlassBody",
            "Texture/Particle04_clear_soft.png",
            (58, 164, 255, 120),
            (1.15, 1.15, 1.15),
            (1.00, 1.00, 1.00),
            48,
            alpha_blend=1,
            fade_in=0,
            fade_out=12,
        ),
        make_sprite(
            "BubbleCyanRim",
            "Texture/Shockwave.png",
            (25, 205, 255, 230),
            (1.36, 1.36, 1.36),
            (1.14, 1.14, 1.14),
            48,
            rotation=(0.0, 0.0, 12.0),
            rotation_velocity=(0.0, 0.0, 0.65),
            fade_in=0,
            fade_out=12,
        ),
        make_sprite(
            "BubbleInnerGlow",
            "Texture/Particle_Soft.png",
            (68, 212, 255, 150),
            (0.78, 0.78, 0.78),
            (0.62, 0.62, 0.62),
            48,
            fade_in=0,
            fade_out=12,
        ),
        make_sprite(
            "BubbleSpecularStar",
            "Texture/Star.png",
            (225, 252, 255, 235),
            (0.27, 0.27, 0.27),
            (0.20, 0.20, 0.20),
            48,
            location=(-0.25, 0.24, 0.0),
            rotation_velocity=(0.0, 0.0, 1.7),
            fade_in=0,
            fade_out=10,
        ),
        make_sprite(
            "BubbleOrbitMotes",
            "Texture/Particle01.png",
            (190, 244, 255, 210),
            (0.16, 0.16, 0.16),
            (0.035, 0.035, 0.035),
            20,
            max_generation=5,
            generation_time=1.5,
            location_min=(-0.62, -0.52, -0.12),
            location_max=(0.62, 0.52, 0.12),
            velocity_min=(-0.012, -0.005, -0.012),
            velocity_max=(0.012, 0.018, 0.012),
            fade_in=0,
            fade_out=10,
        ),
        make_sprite(
            "BubbleIridescentRingA",
            "Texture/Shockwave.png",
            (118, 238, 255, 175),
            (1.18, 0.82, 1.18),
            (0.94, 0.66, 0.94),
            48,
            rotation=(0.0, 0.0, -22.0),
            rotation_velocity=(0.0, 0.0, -0.72),
            alpha_blend=1,
            fade_in=2,
            fade_out=12,
        ),
        make_sprite(
            "BubbleIridescentRingB",
            "Texture/Shockwave.png",
            (204, 156, 255, 150),
            (0.86, 1.16, 0.86),
            (0.68, 0.92, 0.68),
            48,
            rotation=(0.0, 0.0, 28.0),
            rotation_velocity=(0.0, 0.0, 0.58),
            alpha_blend=1,
            fade_in=3,
            fade_out=12,
        ),
        make_sprite(
            "BubblePearlHighlight",
            "Texture/Particle04_clear_soft.png",
            (238, 255, 255, 215),
            (0.34, 0.22, 0.34),
            (0.24, 0.16, 0.24),
            48,
            location=(-0.29, 0.31, -0.02),
            rotation=(0.0, 0.0, -28.0),
            fade_in=1,
            fade_out=10,
        ),
        make_sprite(
            "BubbleTwinkleCluster",
            "Texture/Star.png",
            (202, 250, 255, 220),
            (0.18, 0.18, 0.18),
            (0.035, 0.035, 0.035),
            22,
            max_generation=4,
            generation_time=4.0,
            location_min=(-0.72, -0.56, -0.14),
            location_max=(0.72, 0.62, 0.14),
            velocity_min=(-0.010, 0.002, -0.008),
            velocity_max=(0.010, 0.024, 0.008),
            rotation_velocity_min=(0.0, 0.0, -2.8),
            rotation_velocity_max=(0.0, 0.0, 2.8),
            fade_in=1,
            fade_out=10,
        ),
        make_sprite(
            "BubbleTrailingPearls",
            "Texture/Particle01.png",
            (91, 212, 255, 185),
            (0.12, 0.12, 0.12),
            (0.025, 0.025, 0.025),
            20,
            max_generation=6,
            generation_time=3.0,
            location_min=(-0.46, -0.68, -0.14),
            location_max=(0.46, -0.18, 0.14),
            velocity_min=(-0.010, -0.026, -0.008),
            velocity_max=(0.010, -0.008, 0.008),
            fade_in=0,
            fade_out=10,
        ),
    ]


def petal_nodes() -> List[ET.Element]:
    # The reviewed HATO flower model is intentionally not used in the
    # high-frequency projectile.  Its 3 MB animated runtime payload caused a
    # one-time 180 ms Debug upload spike.  Layered RGBA silhouettes preserve
    # volume/readability without that first-cast hitch.
    feather_common = {
        "texture": "Texture/Feather.png",
        "start_scale": (0.38, 0.38, 0.38),
        "end_scale": (0.16, 0.16, 0.16),
        "life": 34,
        "max_generation": 1,
        "generation_time": 9999.0,
        "location_min": (-0.28, -0.18, -0.18),
        "location_max": (0.28, 0.24, 0.18),
        "velocity_min": (-0.022, -0.002, -0.018),
        "velocity_max": (0.022, 0.026, 0.018),
        "rotation_min": (0.0, 0.0, -180.0),
        "rotation_max": (0.0, 0.0, 180.0),
        "rotation_velocity_min": (0.0, 0.0, -3.2),
        "rotation_velocity_max": (0.0, 0.0, 3.2),
        "alpha_blend": 1,
        "fade_in": 1,
        "fade_out": 14,
    }
    return [
        make_sprite(
            "PetalSilkVolume",
            "Texture/aoi-hane.png",
            (255, 112, 184, 238),
            (0.58, 0.82, 0.58),
            (0.48, 0.68, 0.48),
            75,
            rotation=(0.0, 0.0, -24.0),
            rotation_velocity=(0.0, 0.0, 0.65),
            alpha_blend=1,
            fade_in=0,
            fade_out=18,
        ),
        make_sprite(
            "PetalRoseA",
            color=(255, 112, 176, 240),
            uv=(0, 256, 128, 128),
            **feather_common
        ),
        make_sprite(
            "PetalRoseB",
            color=(255, 178, 218, 235),
            generation_offset=4.0,
            uv=(256, 384, 128, 128),
            **feather_common
        ),
        make_sprite(
            "PetalStarHeart",
            "Texture/Star.png",
            (255, 226, 132, 245),
            (0.64, 0.64, 0.64),
            (0.48, 0.48, 0.48),
            75,
            rotation_velocity=(0.0, 0.0, 1.4),
            fade_out=18,
        ),
        make_sprite(
            "PetalSparkles",
            "Texture/Star.png",
            (255, 234, 247, 220),
            (0.20, 0.20, 0.20),
            (0.04, 0.04, 0.04),
            24,
            max_generation=2,
            generation_time=12.0,
            location_min=(-0.48, -0.34, -0.18),
            location_max=(0.48, 0.42, 0.18),
            velocity_min=(-0.009, 0.004, -0.009),
            velocity_max=(0.009, 0.020, 0.009),
            rotation_velocity_min=(0.0, 0.0, -2.0),
            rotation_velocity_max=(0.0, 0.0, 2.0),
            fade_in=0,
            fade_out=12,
        ),
    ]


def fireball_nodes() -> List[ET.Element]:
    return [
        make_sprite(
            "FireballGoldenCore",
            "Texture/Particle_Soft.png",
            (255, 226, 92, 235),
            (1.20, 1.20, 1.20),
            (0.96, 0.96, 0.96),
            80,
            fade_out=18,
        ),
        make_sprite(
            "FireballCoralOutline",
            "Texture/Shockwave.png",
            (255, 84, 38, 230),
            (1.58, 1.58, 1.58),
            (1.34, 1.34, 1.34),
            80,
            rotation_velocity=(0.0, 0.0, 0.8),
            fade_out=18,
        ),
        make_sprite(
            "FireballFlameLobeA",
            "Texture/Fire_Single.png",
            (255, 123, 38, 235),
            (0.72, 1.18, 0.72),
            (0.58, 0.92, 0.58),
            80,
            location=(0.14, 0.31, 0.0),
            rotation=(0.0, 0.0, -15.0),
            rotation_velocity=(0.0, 0.0, 0.35),
            alpha_blend=1,
            fade_out=18,
        ),
        make_sprite(
            "FireballFlameLobeB",
            "Texture/Fire_Blur.png",
            (255, 72, 28, 210),
            (0.78, 1.05, 0.78),
            (0.54, 0.82, 0.54),
            80,
            location=(-0.17, 0.16, 0.0),
            rotation=(0.0, 0.0, 18.0),
            rotation_velocity=(0.0, 0.0, -0.42),
            fade_out=18,
            uv=(0, 0, 256, 256),
        ),
        make_sprite(
            "FireballStarGlint",
            "Texture/Star.png",
            (255, 250, 198, 245),
            (0.42, 0.42, 0.42),
            (0.30, 0.30, 0.30),
            80,
            location=(-0.17, 0.22, -0.02),
            rotation_velocity=(0.0, 0.0, 1.8),
            fade_out=16,
        ),
        make_sprite(
            "FireballEmberMotes",
            "Texture/Splash.png",
            (255, 154, 62, 225),
            (0.22, 0.22, 0.22),
            (0.04, 0.04, 0.04),
            24,
            max_generation=5,
            generation_time=9.0,
            location_min=(-0.55, -0.38, -0.16),
            location_max=(0.55, 0.48, 0.16),
            velocity_min=(-0.017, 0.004, -0.012),
            velocity_max=(0.017, 0.028, 0.012),
            rotation_velocity_min=(0.0, 0.0, -2.4),
            rotation_velocity_max=(0.0, 0.0, 2.4),
            fade_in=0,
            fade_out=12,
            uv=(0, 0, 256, 256),
        ),
        make_sprite(
            "FireballWhiteHotHeart",
            "Texture/Particle_Soft.png",
            (255, 253, 220, 250),
            (0.54, 0.54, 0.54),
            (0.42, 0.42, 0.42),
            80,
            fade_in=0,
            fade_out=16,
        ),
        make_sprite(
            "FireballFlameCrown",
            "Texture/Fire_Single.png",
            (255, 184, 48, 230),
            (0.92, 1.38, 0.92),
            (0.68, 1.04, 0.68),
            80,
            location=(0.0, 0.38, 0.02),
            rotation=(0.0, 0.0, 4.0),
            rotation_velocity=(0.0, 0.0, -0.28),
            alpha_blend=1,
            fade_out=18,
        ),
        make_sprite(
            "FireballFlameCrownEcho",
            "Texture/Fire_Blur.png",
            (255, 74, 24, 185),
            (1.05, 1.52, 1.05),
            (0.74, 1.10, 0.74),
            80,
            location=(-0.06, 0.29, 0.0),
            rotation=(0.0, 0.0, -9.0),
            rotation_velocity=(0.0, 0.0, 0.34),
            fade_out=18,
            uv=(0, 0, 256, 256),
        ),
        make_sprite(
            "FireballOrbitingSparks",
            "Texture/Star.png",
            (255, 230, 120, 235),
            (0.19, 0.19, 0.19),
            (0.035, 0.035, 0.035),
            26,
            max_generation=4,
            generation_time=4.5,
            location_min=(-0.72, -0.42, -0.15),
            location_max=(0.72, 0.62, 0.15),
            velocity_min=(-0.018, 0.004, -0.010),
            velocity_max=(0.018, 0.032, 0.010),
            rotation_velocity_min=(0.0, 0.0, -3.4),
            rotation_velocity_max=(0.0, 0.0, 3.4),
            fade_in=0,
            fade_out=12,
        ),
        make_sprite(
            "FireballHeatHalo",
            "Texture/Shockwave.png",
            (255, 118, 32, 145),
            (1.82, 1.82, 1.82),
            (1.44, 1.44, 1.44),
            80,
            rotation=(0.0, 0.0, 33.0),
            rotation_velocity=(0.0, 0.0, -0.52),
            alpha_blend=1,
            fade_in=2,
            fade_out=18,
        ),
        make_sprite(
            "FireballTrailingCinders",
            "Texture/Splash.png",
            (255, 92, 30, 205),
            (0.16, 0.16, 0.16),
            (0.03, 0.03, 0.03),
            22,
            max_generation=6,
            generation_time=3.0,
            location_min=(-0.46, -0.78, -0.14),
            location_max=(0.46, -0.22, 0.14),
            velocity_min=(-0.014, -0.030, -0.010),
            velocity_max=(0.014, -0.006, 0.010),
            rotation_velocity_min=(0.0, 0.0, -2.6),
            rotation_velocity_max=(0.0, 0.0, 2.6),
            fade_in=0,
            fade_out=10,
            uv=(0, 0, 256, 256),
        ),
    ]


def moonblade_nodes() -> List[ET.Element]:
    return [
        make_sprite("MoonBladePearlCore", "Texture/Particle_Soft.png", (208, 244, 255, 225),
                    (0.72, 0.72, 0.72), (0.52, 0.52, 0.52), 48, fade_out=14),
        make_sprite("MoonBladeLavenderHalo", "Texture/Shockwave.png", (132, 156, 255, 210),
                    (1.46, 1.46, 1.46), (1.12, 1.12, 1.12), 48,
                    rotation_velocity=(0.0, 0.0, 0.75), fade_out=14),
        make_sprite("MoonBladeCrescentA", "Texture/Feather.png", (226, 244, 255, 245),
                    (0.34, 1.28, 0.34), (0.25, 1.00, 0.25), 48,
                    location=(0.28, 0.0, 0.0), rotation=(0.0, 0.0, -42.0),
                    alpha_blend=1, fade_out=14),
        make_sprite("MoonBladeCrescentB", "Texture/Feather.png", (154, 178, 255, 225),
                    (0.30, 1.12, 0.30), (0.22, 0.88, 0.22), 48,
                    location=(-0.25, 0.0, 0.0), rotation=(0.0, 0.0, 42.0),
                    alpha_blend=1, fade_out=14),
        make_sprite("MoonBladeStarSeal", "Texture/Star.png", (255, 250, 202, 240),
                    (0.34, 0.34, 0.34), (0.22, 0.22, 0.22), 48,
                    rotation_velocity=(0.0, 0.0, -1.8), fade_out=12),
        make_sprite("MoonBladeOrbitStars", "Texture/Star.png", (182, 214, 255, 220),
                    (0.15, 0.15, 0.15), (0.03, 0.03, 0.03), 22,
                    max_generation=4, generation_time=4.0,
                    location_min=(-0.68, -0.46, -0.12), location_max=(0.68, 0.46, 0.12),
                    velocity_min=(-0.012, 0.002, -0.008), velocity_max=(0.012, 0.024, 0.008),
                    rotation_velocity_min=(0.0, 0.0, -2.8), rotation_velocity_max=(0.0, 0.0, 2.8),
                    fade_in=0, fade_out=10),
        make_sprite("MoonBladeSilverTrail", "Texture/Particle01.png", (208, 238, 255, 190),
                    (0.12, 0.12, 0.12), (0.025, 0.025, 0.025), 20,
                    max_generation=6, generation_time=3.0,
                    location_min=(-0.38, -0.82, -0.12), location_max=(0.38, -0.20, 0.12),
                    velocity_min=(-0.012, -0.028, -0.008), velocity_max=(0.012, -0.006, 0.008),
                    fade_in=0, fade_out=10),
        make_sprite("MoonBladeOuterPulse", "Texture/Shockwave.png", (104, 214, 255, 145),
                    (1.78, 1.78, 1.78), (1.34, 1.34, 1.34), 48,
                    rotation=(0.0, 0.0, 24.0), rotation_velocity=(0.0, 0.0, -0.48),
                    alpha_blend=1, fade_in=2, fade_out=14),
    ]


def dagger_nodes() -> List[ET.Element]:
    return [
        make_sprite("DaggerIvoryBlade", "Texture/Feather.png", (255, 251, 226, 245),
                    (0.20, 1.22, 0.20), (0.15, 0.94, 0.15), 42,
                    rotation=(0.0, 0.0, -8.0), alpha_blend=1, fade_out=12),
        make_sprite("DaggerSkyOutline", "Texture/Feather.png", (94, 206, 255, 220),
                    (0.28, 1.38, 0.28), (0.20, 1.04, 0.20), 42,
                    rotation=(0.0, 0.0, 8.0), alpha_blend=1, fade_out=12),
        make_sprite("DaggerGoldenGuard", "Texture/Star.png", (255, 214, 94, 240),
                    (0.38, 0.38, 0.38), (0.26, 0.26, 0.26), 42,
                    location=(0.0, -0.20, 0.0), rotation_velocity=(0.0, 0.0, 1.5), fade_out=12),
        make_sprite("DaggerBlueGlow", "Texture/Particle_Soft.png", (82, 190, 255, 180),
                    (0.72, 1.08, 0.72), (0.48, 0.82, 0.48), 42, fade_out=12),
        make_sprite("DaggerWingA", "Texture/Feather.png", (196, 238, 255, 220),
                    (0.16, 0.62, 0.16), (0.10, 0.42, 0.10), 42,
                    location=(-0.26, -0.18, 0.0), rotation=(0.0, 0.0, -54.0),
                    alpha_blend=1, fade_out=12),
        make_sprite("DaggerWingB", "Texture/Feather.png", (255, 218, 236, 210),
                    (0.16, 0.62, 0.16), (0.10, 0.42, 0.10), 42,
                    location=(0.26, -0.18, 0.0), rotation=(0.0, 0.0, 54.0),
                    alpha_blend=1, fade_out=12),
        make_sprite("DaggerSparkTrail", "Texture/Particle01.png", (196, 238, 255, 210),
                    (0.12, 0.12, 0.12), (0.02, 0.02, 0.02), 18,
                    max_generation=6, generation_time=2.8,
                    location_min=(-0.32, -0.82, -0.10), location_max=(0.32, -0.28, 0.10),
                    velocity_min=(-0.010, -0.026, -0.006), velocity_max=(0.010, -0.008, 0.006),
                    fade_in=0, fade_out=9),
        make_sprite("DaggerLaunchRing", "Texture/Shockwave.png", (255, 220, 122, 150),
                    (1.20, 1.20, 1.20), (0.86, 0.86, 0.86), 42,
                    rotation_velocity=(0.0, 0.0, -0.72), alpha_blend=1, fade_in=2, fade_out=12),
    ]


def starwheel_nodes() -> List[ET.Element]:
    return [
        make_sprite("StarWheelAquaAura", "Texture/Particle_Soft.png", (84, 220, 255, 190),
                    (1.04, 1.04, 1.04), (0.80, 0.80, 0.80), 54, fade_out=16),
        make_sprite("StarWheelGoldenOuterRing", "Texture/Shockwave.png", (255, 220, 102, 230),
                    (1.62, 1.62, 1.62), (1.26, 1.26, 1.26), 54,
                    rotation_velocity=(0.0, 0.0, 1.4), fade_out=16),
        make_sprite("StarWheelCyanInnerRing", "Texture/Shockwave.png", (96, 226, 255, 210),
                    (1.18, 1.18, 1.18), (0.92, 0.92, 0.92), 54,
                    rotation=(0.0, 0.0, 22.0), rotation_velocity=(0.0, 0.0, -1.8), fade_out=16),
        make_sprite("StarWheelIvoryGlint", "Texture/Star.png", (255, 252, 224, 245),
                    (0.48, 0.48, 0.48), (0.32, 0.32, 0.32), 54,
                    rotation_velocity=(0.0, 0.0, 2.8), fade_out=14),
        make_sprite("StarWheelOrbitStars", "Texture/Star.png", (255, 198, 228, 220),
                    (0.15, 0.15, 0.15), (0.03, 0.03, 0.03), 22,
                    max_generation=4, generation_time=4.0,
                    location_min=(-0.72, -0.58, -0.12), location_max=(0.72, 0.58, 0.12),
                    velocity_min=(-0.012, 0.0, -0.008), velocity_max=(0.012, 0.022, 0.008),
                    rotation_velocity_min=(0.0, 0.0, -3.0), rotation_velocity_max=(0.0, 0.0, 3.0),
                    fade_in=0, fade_out=10),
        make_sprite("StarWheelCandyMotes", "Texture/Particle01.png", (180, 244, 255, 205),
                    (0.12, 0.12, 0.12), (0.025, 0.025, 0.025), 20,
                    max_generation=5, generation_time=3.2,
                    location_min=(-0.58, -0.58, -0.12), location_max=(0.58, 0.58, 0.12),
                    velocity_min=(-0.014, -0.014, -0.008), velocity_max=(0.014, 0.014, 0.008),
                    fade_in=0, fade_out=10),
        make_sprite("StarWheelReturnTrail", "Texture/Particle01.png", (255, 222, 136, 190),
                    (0.11, 0.11, 0.11), (0.02, 0.02, 0.02), 18,
                    max_generation=5, generation_time=3.0,
                    location_min=(-0.36, -0.78, -0.10), location_max=(0.36, -0.20, 0.10),
                    velocity_min=(-0.010, -0.028, -0.006), velocity_max=(0.010, -0.008, 0.006),
                    fade_in=0, fade_out=9),
        make_sprite("StarWheelRainbowPulse", "Texture/Shockwave.png", (214, 162, 255, 135),
                    (1.92, 1.92, 1.92), (1.48, 1.48, 1.48), 54,
                    rotation=(0.0, 0.0, -28.0), rotation_velocity=(0.0, 0.0, 0.44),
                    alpha_blend=1, fade_in=2, fade_out=16),
    ]


def wisp_nodes() -> List[ET.Element]:
    return [
        make_sprite("WispPearlHeart", "Texture/Particle_Soft.png", (238, 255, 248, 245),
                    (0.58, 0.58, 0.58), (0.42, 0.42, 0.42), 52, fade_out=15),
        make_sprite("WispSkyFlame", "Texture/Fire_Single.png", (78, 218, 255, 225),
                    (0.62, 1.12, 0.62), (0.46, 0.84, 0.46), 52,
                    location=(0.0, 0.22, 0.0), rotation_velocity=(0.0, 0.0, 0.38),
                    alpha_blend=1, fade_out=15),
        make_sprite("WispLavenderEcho", "Texture/Fire_Blur.png", (170, 126, 255, 185),
                    (0.76, 1.30, 0.76), (0.54, 0.96, 0.54), 52,
                    location=(-0.08, 0.14, 0.0), rotation=(0.0, 0.0, -14.0),
                    rotation_velocity=(0.0, 0.0, -0.42), fade_out=15, uv=(0, 0, 256, 256)),
        make_sprite("WispAquaHalo", "Texture/Shockwave.png", (78, 220, 255, 190),
                    (1.34, 1.34, 1.34), (1.02, 1.02, 1.02), 52,
                    rotation_velocity=(0.0, 0.0, 0.76), fade_out=15),
        make_sprite("WispStarGlint", "Texture/Star.png", (255, 248, 192, 240),
                    (0.34, 0.34, 0.34), (0.22, 0.22, 0.22), 52,
                    location=(-0.18, 0.20, 0.0), rotation_velocity=(0.0, 0.0, 2.2), fade_out=13),
        make_sprite("WispChoirSparks", "Texture/Star.png", (198, 230, 255, 215),
                    (0.14, 0.14, 0.14), (0.025, 0.025, 0.025), 20,
                    max_generation=4, generation_time=3.8,
                    location_min=(-0.58, -0.42, -0.12), location_max=(0.58, 0.52, 0.12),
                    velocity_min=(-0.012, 0.002, -0.008), velocity_max=(0.012, 0.026, 0.008),
                    rotation_velocity_min=(0.0, 0.0, -3.0), rotation_velocity_max=(0.0, 0.0, 3.0),
                    fade_in=0, fade_out=10),
        make_sprite("WispCometMotes", "Texture/Particle01.png", (122, 232, 255, 205),
                    (0.12, 0.12, 0.12), (0.025, 0.025, 0.025), 20,
                    max_generation=5, generation_time=3.0,
                    location_min=(-0.42, -0.72, -0.12), location_max=(0.42, -0.16, 0.12),
                    velocity_min=(-0.012, -0.028, -0.008), velocity_max=(0.012, -0.006, 0.008),
                    fade_in=0, fade_out=10),
        make_sprite("WispPastelTrail", "Texture/Splash.png", (116, 172, 255, 185),
                    (0.14, 0.14, 0.14), (0.025, 0.025, 0.025), 20,
                    max_generation=6, generation_time=3.0,
                    location_min=(-0.40, -0.82, -0.12), location_max=(0.40, -0.22, 0.12),
                    velocity_min=(-0.014, -0.030, -0.008), velocity_max=(0.014, -0.008, 0.008),
                    rotation_velocity_min=(0.0, 0.0, -2.4), rotation_velocity_max=(0.0, 0.0, 2.4),
                    fade_in=0, fade_out=10, uv=(0, 0, 256, 256)),
    ]


PROFILER_BASELINES = {
    "BubbleBounceToon": {
        "status": "PendingRemeasurement",
        "scale": 0.82,
        "peak_cpu_instances": "Estimated: 22",
        "p95_cpu_instances": "Unavailable",
        "peak_draw_calls": "Unavailable",
        "p95_draw_calls": "Unavailable",
        "peak_vertices": "Unavailable",
        "p95_vertices": "Unavailable",
        "cpu_update_p95_ms": "Unavailable",
        "cpu_update_peak_ms": "Unavailable",
        "cpu_draw_submit_p95_ms": "Unavailable",
        "cpu_draw_submit_peak_ms": "Unavailable",
        "effect_load_ms": "Unavailable",
        "first_play_ms": "Unavailable",
        "budget_rejected_count": "Unavailable",
    },
    "PetalStarburstToon": {
        "status": "MeasuredPartial",
        "scale": 0.42,
        "peak_cpu_instances": 7,
        "p95_cpu_instances": 7,
        "peak_draw_calls": 3,
        "p95_draw_calls": 3,
        "peak_vertices": 24,
        "p95_vertices": 24,
        "cpu_update_p95_ms": 0.0639,
        "cpu_update_peak_ms": 0.1847,
        "cpu_draw_submit_p95_ms": 0.2083,
        "cpu_draw_submit_peak_ms": 3.9576,
        "effect_load_ms": 40.9589,
        "first_play_ms": 0.1107,
        "budget_rejected_count": 0,
        "concurrency_sample": {
            "handles": 8,
            "peak_cpu_instances": 56,
            "peak_draw_calls": 24,
            "peak_vertices": 192,
            "cpu_update_p95_ms": 0.1988,
            "cpu_draw_submit_p95_ms": 1.2088,
            "budget_rejected_count": 0,
        },
    },
    "FireballChibiToon": {
        "status": "PendingRemeasurement",
        "scale": 0.56,
        "peak_cpu_instances": "Estimated: 22",
        "p95_cpu_instances": "Unavailable",
        "peak_draw_calls": "Unavailable",
        "p95_draw_calls": "Unavailable",
        "peak_vertices": "Unavailable",
        "p95_vertices": "Unavailable",
        "cpu_update_p95_ms": "Unavailable",
        "cpu_update_peak_ms": "Unavailable",
        "cpu_draw_submit_p95_ms": "Unavailable",
        "cpu_draw_submit_peak_ms": "Unavailable",
        "effect_load_ms": "Unavailable",
        "first_play_ms": "Unavailable",
        "budget_rejected_count": "Unavailable",
    },
    "MoonBladeToon": {
        "status": "PendingRemeasurement", "scale": 0.69,
        "peak_cpu_instances": "Estimated: 19", "p95_cpu_instances": "Unavailable",
        "peak_draw_calls": "Unavailable", "p95_draw_calls": "Unavailable",
        "peak_vertices": "Unavailable", "p95_vertices": "Unavailable",
        "cpu_update_p95_ms": "Unavailable", "cpu_update_peak_ms": "Unavailable",
        "cpu_draw_submit_p95_ms": "Unavailable", "cpu_draw_submit_peak_ms": "Unavailable",
        "effect_load_ms": "Unavailable", "first_play_ms": "Unavailable",
        "budget_rejected_count": "Unavailable",
    },
    "StarDaggerToon": {
        "status": "PendingRemeasurement", "scale": 0.68,
        "peak_cpu_instances": "Estimated: 19", "p95_cpu_instances": "Unavailable",
        "peak_draw_calls": "Unavailable", "p95_draw_calls": "Unavailable",
        "peak_vertices": "Unavailable", "p95_vertices": "Unavailable",
        "cpu_update_p95_ms": "Unavailable", "cpu_update_peak_ms": "Unavailable",
        "cpu_draw_submit_p95_ms": "Unavailable", "cpu_draw_submit_peak_ms": "Unavailable",
        "effect_load_ms": "Unavailable", "first_play_ms": "Unavailable",
        "budget_rejected_count": "Unavailable",
    },
    "StarWheelToon": {
        "status": "PendingRemeasurement", "scale": 0.78,
        "peak_cpu_instances": "Estimated: 19", "p95_cpu_instances": "Unavailable",
        "peak_draw_calls": "Unavailable", "p95_draw_calls": "Unavailable",
        "peak_vertices": "Unavailable", "p95_vertices": "Unavailable",
        "cpu_update_p95_ms": "Unavailable", "cpu_update_peak_ms": "Unavailable",
        "cpu_draw_submit_p95_ms": "Unavailable", "cpu_draw_submit_peak_ms": "Unavailable",
        "effect_load_ms": "Unavailable", "first_play_ms": "Unavailable",
        "budget_rejected_count": "Unavailable",
    },
    "WispCometToon": {
        "status": "PendingRemeasurement", "scale": 0.50,
        "peak_cpu_instances": "Estimated: 21", "p95_cpu_instances": "Unavailable",
        "peak_draw_calls": "Unavailable", "p95_draw_calls": "Unavailable",
        "peak_vertices": "Unavailable", "p95_vertices": "Unavailable",
        "cpu_update_p95_ms": "Unavailable", "cpu_update_peak_ms": "Unavailable",
        "cpu_draw_submit_p95_ms": "Unavailable", "cpu_draw_submit_peak_ms": "Unavailable",
        "effect_load_ms": "Unavailable", "first_play_ms": "Unavailable",
        "budget_rejected_count": "Unavailable",
    },
}
for _baseline in PROFILER_BASELINES.values():
    _pending_remeasurement = _baseline["status"] == "PendingRemeasurement"
    _baseline.update(
        {
            "tested_at": None if _pending_remeasurement else "2026-08-23",
            "build_type": "Debug",
            "renderer_api": "DX12",
            "gpu": "NVIDIA GeForce RTX 3060 Laptop GPU",
            "driver": "32.0.15.9174",
            "resolution": "1280x720",
            "quality_tier": "High",
            "handles": 1,
            "warmup_seconds": "Unavailable" if _pending_remeasurement else 1.0,
            "sample_seconds": "Unavailable" if _pending_remeasurement else 5.0,
            "gpu_render_ms": {"status": "Unavailable", "value": None},
            "measurement_note": (
                "Richness pass has not yet received an isolated profiler baseline; "
                "particle count is recipe-derived and all timing counters remain unavailable."
                if _pending_remeasurement
                else (
                    "Rolling active-lifetime metrics. The CPU-proxy calibration status "
                    "is not applicable because this short projectile expires before its "
                    "two-second particle-count calibration point."
                )
            ),
        }
    )


EFFECTS = (
    {
        "name": "BubbleBounceToon",
        "build_nodes": "bubble",
        "resources": (
            ("official_glow", "Texture/Particle04_clear_soft.png"),
            ("official_mote", "Texture/Particle01.png"),
            ("pierre_soft", "Texture/Particle_Soft.png"),
            ("pierre_ring", "Texture/Shockwave.png"),
            ("pierre_star", "Texture/Star.png"),
        ),
        "licenses": ("effekseer01-official", "pierre02"),
        "estimated_peak_particles": 22,
        "profiler_baseline": PROFILER_BASELINES["BubbleBounceToon"],
        "recommended_runtime": {
            "scale": 0.82,
            "speed": 1.0,
            "orientation": "camera-facing; direction-independent",
            "origin_y_offset": "target center + 0.32 (boss + 0.52)",
            "restart_policy": "single handle follows each 0.18s bounce; no restart needed",
        },
        "layers": [
            "translucent blue body",
            "cyan rim",
            "inner glow",
            "specular star",
            "five bounded orbit motes",
            "two counter-rotating iridescent rings",
            "pearl highlight",
            "four cyan twinkles",
            "six trailing pearls",
        ],
    },
    {
        "name": "PetalStarburstToon",
        "build_nodes": "petal",
        "resources": (
            ("hato_wing", "Texture/aoi-hane.png"),
            ("pierre_feather", "Texture/Feather.png"),
            ("pierre_star", "Texture/Star.png"),
        ),
        "licenses": ("hato01", "pierre02"),
        "estimated_peak_particles": 7,
        "profiler_baseline": PROFILER_BASELINES["PetalStarburstToon"],
        "recommended_runtime": {
            "scale": 0.42,
            "speed": 1.0,
            "orientation": "camera-facing layered volume; direction-independent",
            "origin_y_offset": 0.50,
            "restart_policy": "one persistent handle per projectile; cap remains 24 projectiles",
        },
        "layers": [
            "RGBA pink silk-petal volume",
            "two UV-cropped rose feather sprays",
            "gold star heart",
            "two bounded sparkles",
        ],
    },
    {
        "name": "FireballChibiToon",
        "build_nodes": "fireball",
        "resources": (
            ("pierre_soft", "Texture/Particle_Soft.png"),
            ("pierre_ring", "Texture/Shockwave.png"),
            ("pierre_fire", "Texture/Fire_Single.png"),
            ("pierre_fire_blur", "Texture/Fire_Blur.png"),
            ("pierre_star", "Texture/Star.png"),
            ("pierre_splash", "Texture/Splash.png"),
        ),
        "licenses": ("pierre02",),
        "estimated_peak_particles": 22,
        "profiler_baseline": PROFILER_BASELINES["FireballChibiToon"],
        "recommended_runtime": {
            "scale": 0.56,
            "speed": 1.0,
            "orientation": "camera-facing; move the root along the gameplay trajectory",
            "origin_y_offset": 0.62,
            "restart_policy": "replace native-distance playback with one transformed projectile handle",
        },
        "layers": [
            "gold core",
            "coral outline",
            "two independently moving flame lobes",
            "star glint",
            "five bounded ember motes",
            "white-hot heart",
            "two counter-rotating flame crowns",
            "four orbiting sparks",
            "heat halo",
            "six trailing cinders",
        ],
    },
    {
        "name": "MoonBladeToon",
        "build_nodes": "moonblade",
        "resources": (
            ("pierre_soft", "Texture/Particle_Soft.png"),
            ("pierre_ring", "Texture/Shockwave.png"),
            ("pierre_feather", "Texture/Feather.png"),
            ("pierre_star", "Texture/Star.png"),
            ("official_mote", "Texture/Particle01.png"),
        ),
        "licenses": ("pierre02", "effekseer01-official"),
        "estimated_peak_particles": 19,
        "profiler_baseline": PROFILER_BASELINES["MoonBladeToon"],
        "recommended_runtime": {
            "scale": 0.69, "speed": 1.0,
            "orientation": "camera-facing layered crescent; root follows the piercing trajectory",
            "origin_y_offset": 0.44,
            "restart_policy": "one bounded handle per piercing projectile",
        },
        "layers": ["pearl core", "lavender halo", "dual crescent blades", "star seal",
                   "four orbit stars", "six silver trail motes", "outer pulse"],
    },
    {
        "name": "StarDaggerToon",
        "build_nodes": "dagger",
        "resources": (
            ("pierre_feather", "Texture/Feather.png"),
            ("pierre_star", "Texture/Star.png"),
            ("pierre_soft", "Texture/Particle_Soft.png"),
            ("official_mote", "Texture/Particle01.png"),
            ("pierre_ring", "Texture/Shockwave.png"),
        ),
        "licenses": ("pierre02", "effekseer01-official"),
        "estimated_peak_particles": 19,
        "profiler_baseline": PROFILER_BASELINES["StarDaggerToon"],
        "recommended_runtime": {
            "scale": 0.68, "speed": 1.0,
            "orientation": "camera-facing blade volume; root faces the travel tangent",
            "origin_y_offset": 0.46,
            "restart_policy": "one bounded handle per dagger projectile",
        },
        "layers": ["ivory blade", "sky outline", "gold guard", "blue glow",
                   "dual candy wings", "six spark trail motes", "launch ring"],
    },
    {
        "name": "StarWheelToon",
        "build_nodes": "starwheel",
        "resources": (
            ("pierre_soft", "Texture/Particle_Soft.png"),
            ("pierre_ring", "Texture/Shockwave.png"),
            ("pierre_star", "Texture/Star.png"),
            ("official_mote", "Texture/Particle01.png"),
        ),
        "licenses": ("pierre02", "effekseer01-official"),
        "estimated_peak_particles": 19,
        "profiler_baseline": PROFILER_BASELINES["StarWheelToon"],
        "recommended_runtime": {
            "scale": 0.78, "speed": 1.0,
            "orientation": "camera-facing aura around the native spinning star wheel",
            "origin_y_offset": 0.50,
            "restart_policy": "one bounded handle follows the outbound and return path",
        },
        "layers": ["aqua aura", "gold outer ring", "cyan inner ring", "ivory glint",
                   "four orbit stars", "five candy motes", "five return motes", "rainbow pulse"],
    },
    {
        "name": "WispCometToon",
        "build_nodes": "wisp",
        "resources": (
            ("pierre_soft", "Texture/Particle_Soft.png"),
            ("pierre_fire", "Texture/Fire_Single.png"),
            ("pierre_fire_blur", "Texture/Fire_Blur.png"),
            ("pierre_ring", "Texture/Shockwave.png"),
            ("pierre_star", "Texture/Star.png"),
            ("official_mote", "Texture/Particle01.png"),
            ("pierre_splash", "Texture/Splash.png"),
        ),
        "licenses": ("pierre02", "effekseer01-official"),
        "estimated_peak_particles": 21,
        "profiler_baseline": PROFILER_BASELINES["WispCometToon"],
        "recommended_runtime": {
            "scale": 0.50, "speed": 1.0,
            "orientation": "camera-facing pastel flame around the native comet core",
            "origin_y_offset": 0.52,
            "restart_policy": "one bounded handle per homing projectile",
        },
        "layers": ["pearl heart", "sky flame", "lavender echo", "aqua halo",
                   "star glint", "four choir sparks", "five comet motes", "six pastel trail motes"],
    },
)


def compile_effect(editor: Path, project: Path, output: Path) -> None:
    process = subprocess.run(
        [str(editor), "-cui", "-in", str(project), "-o", str(output)],
        cwd=str(editor.parent),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
        check=False,
    )
    if process.returncode != 0 or not output.is_file():
        raise GenerationError(
            "Effekseer {0} conversion failed ({1}): {2}".format(
                EFFEKSEER_VERSION,
                process.returncode,
                process.stderr.decode("utf-8", errors="replace"),
            )
        )
    data = output.read_bytes()
    if not data.startswith(b"EFKE"):
        raise GenerationError("converted effect is not an editable EFKE container")
    tags = []
    offset = 8
    while offset < len(data):
        if offset + 8 > len(data):
            raise GenerationError("converted EFKE chunk table is truncated")
        tag = data[offset : offset + 4]
        size = int.from_bytes(data[offset + 4 : offset + 8], "little")
        offset += 8 + size
        if offset > len(data):
            raise GenerationError("converted EFKE chunk payload is truncated")
        tags.append(tag)
    if tags != [b"INFO", b"EDIT", b"BIN_"]:
        raise GenerationError("unexpected editable EFKE chunks: {0!r}".format(tags))


def deterministic_zip(entries: Mapping[str, bytes]) -> bytes:
    from io import BytesIO

    stream = BytesIO()
    with zipfile.ZipFile(
        stream, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        for name in sorted(entries, key=str.casefold):
            if name.startswith("/") or ".." in name.split("/") or "\\" in name:
                raise GenerationError("unsafe generated package path: {0}".format(name))
            info = zipfile.ZipInfo(name, FIXED_ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, entries[name])
    return stream.getvalue()


def provenance(
    root: Path,
    spec: Mapping[str, object],
    manifests: Mapping[str, Mapping[str, object]],
    project: bytes,
) -> bytes:
    used_source_ids = sorted(
        {SOURCES[source_id][2] for source_id, _ in spec["resources"]}
        | {"pierre02"}
    )
    value = {
        "schema_version": 1,
        "asset_id": str(spec["name"]).lower(),
        "status": "RUNTIME_VALIDATED_DERIVED",
        "generator": "Tools/VFX/generate_survivor_toon_projectiles_v2.py",
        "tool": {"name": "Effekseer", "version": EFFEKSEER_VERSION},
        "source_intakes": [
            {
                "asset_id": asset_id,
                "author": manifests[asset_id]["author"],
                "source": manifests[asset_id]["source"],
                "archive_sha256": manifests[asset_id]["sha256"],
                "license_identifier": manifests[asset_id]["license_identifier"],
                "license_decision": manifests[asset_id]["license_decision"],
                "license_evidence": str(INTAKES[asset_id]["license"]).replace(
                    "\\", "/"
                ),
            }
            for asset_id in used_source_ids
        ],
        "verified_inputs": [
            {
                "source_id": source_id,
                "path": str(SOURCES[source_id][0]).replace("\\", "/"),
                "sha256": SOURCES[source_id][1],
            }
            for source_id, _ in spec["resources"]
        ],
        "modifications": [
            "Reauthored as a bounded multi-layer CPU effect; original archives and packages are unchanged.",
        ] + (
            ["Expanded for the 2026-08-25 cartoon-richness pass."]
            if spec["build_nodes"] != "petal"
            else []
        ) + [
            "Saved as an editable EFKE container with an EDIT chunk using Effekseer 1.80.6.",
        ],
        "project_sha256": sha256_bytes(project),
        "uses_gpu_particles": False,
        "has_cpu_fallback": True,
        "target_renderer_api": "DX12",
        "supported_renderer_apis": ["DX12"],
        "runtime_validation": {
            "status": "PASS",
            "evidence_type": "runtime-manual",
            "command": "$env:HE_VFX_VALIDATE_PACKAGES='1'; .\\hectl.bat game check",
            "result": "expected=24 passed=24 failed=0",
            "coverage": "load/play/transform/stop/replay",
        },
        "estimated_peak_particles": spec["estimated_peak_particles"],
        "profiler_baseline": spec["profiler_baseline"],
        "vfx_budget_category": "Combat",
        "quality_tier_support": ["Medium", "High"],
        "layers": spec["layers"],
        "recommended_runtime": spec["recommended_runtime"],
    }
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode(
        "utf-8"
    )


def build_effect(
    root: Path,
    staging: Path,
    spec: Mapping[str, object],
    manifests: Mapping[str, Mapping[str, object]],
    base_project: bytes,
) -> Tuple[Dict[str, bytes], Dict[str, object]]:
    if spec["build_nodes"] == "bubble":
        nodes = bubble_nodes()
        end_frame = 60
    elif spec["build_nodes"] == "petal":
        nodes = petal_nodes()
        end_frame = 90
    elif spec["build_nodes"] == "fireball":
        nodes = fireball_nodes()
        end_frame = 90
    elif spec["build_nodes"] == "moonblade":
        nodes = moonblade_nodes()
        end_frame = 60
    elif spec["build_nodes"] == "dagger":
        nodes = dagger_nodes()
        end_frame = 60
    elif spec["build_nodes"] == "starwheel":
        nodes = starwheel_nodes()
        end_frame = 72
    elif spec["build_nodes"] == "wisp":
        nodes = wisp_nodes()
        end_frame = 72
    else:
        raise GenerationError("unknown effect recipe: {0}".format(spec["build_nodes"]))

    project = build_project(base_project, nodes, end_frame)
    effect_name = str(spec["name"])
    effect_stage = staging / effect_name
    effect_stage.mkdir(parents=True, exist_ok=True)
    project_path = effect_stage / (effect_name + ".efkproj")
    project_path.write_bytes(project)

    entries: Dict[str, bytes] = {}
    published: Dict[str, bytes] = {
        effect_name + ".efkproj": project,
    }
    for source_id, archive_path in spec["resources"]:
        data = read_verified(root, source_id)
        entries[archive_path] = data
        published[archive_path] = data
        stage_path = effect_stage / archive_path
        stage_path.parent.mkdir(parents=True, exist_ok=True)
        stage_path.write_bytes(data)

    editor = root / "Engine/Tools/Effekseer/bin/Effekseer.exe"
    if not editor.is_file():
        raise GenerationError("bundled Effekseer editor is missing: {0}".format(editor))
    compiled_path = effect_stage / (effect_name + ".efkefc")
    compile_effect(editor, project_path, compiled_path)
    compiled = compiled_path.read_bytes()
    entries[effect_name + ".efkefc"] = compiled
    published[effect_name + ".efkefc"] = compiled

    package_provenance = provenance(root, spec, manifests, project)
    entries["PROVENANCE.json"] = package_provenance
    published["PROVENANCE.json"] = package_provenance
    for asset_id in spec["licenses"]:
        license_data = (root / INTAKES[asset_id]["license"]).read_bytes()
        license_name = "LICENSES/{0}-CC0-1.0.txt".format(asset_id)
        entries[license_name] = license_data
        published[license_name] = license_data

    package = deterministic_zip(entries)
    node_count = len(nodes)
    texture_count = sum(
        1 for _, archive_path in spec["resources"] if archive_path.lower().endswith(".png")
    )
    model_count = sum(
        1
        for _, archive_path in spec["resources"]
        if archive_path.lower().endswith((".fbx", ".efkmodel"))
    )
    summary: Dict[str, object] = {
        "name": effect_name,
        "output": str(OUTPUT_ROOT / (effect_name + ".efkpkg")).replace("\\", "/"),
        "editable_container": True,
        "cpu_only": True,
        "node_count": node_count,
        "texture_count": texture_count,
        "model_count": model_count,
        "estimated_peak_particles": spec["estimated_peak_particles"],
        "project_sha256": sha256_bytes(project),
        "effect_sha256": sha256_bytes(compiled),
        "package_sha256": sha256_bytes(package),
        "package_size": len(package),
        "recommended_runtime": spec["recommended_runtime"],
    }
    published[effect_name + ".efkpkg"] = package
    return published, summary


def publish_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    if temporary.exists():
        raise GenerationError("refusing stale temporary output: {0}".format(temporary))
    temporary.write_bytes(data)
    temporary.replace(path)


def remove_stale_generated_files(root: Path) -> None:
    derived_root = (root / DERIVED_ROOT).resolve()
    for relative in STALE_DERIVED_FILES:
        target = (derived_root / relative).resolve()
        try:
            target.relative_to(derived_root)
        except ValueError:
            raise GenerationError("stale generated path escaped derived root: {0}".format(target))
        if target.is_file():
            target.unlink()
    model_directory = (
        derived_root / "derived/PetalStarburstToon/Model"
    ).resolve()
    if model_directory.is_dir() and not list(model_directory.iterdir()):
        model_directory.rmdir()


def expected_published_path(effect_name: str, relative: str) -> Path:
    if relative == effect_name + ".efkpkg":
        return OUTPUT_ROOT / relative
    return DERIVED_ROOT / "derived" / effect_name / relative


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="HEngineDev repository root",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--dry-run",
        action="store_true",
        help="verify reviewed sources and print intended outputs without writing",
    )
    mode.add_argument(
        "--check",
        action="store_true",
        help="regenerate in isolation and compare every published byte",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.repo_root.resolve()
    manifests = validate_intakes(root)
    for source_id in SOURCES:
        read_verified(root, source_id)

    if args.dry_run:
        print(
            json.dumps(
                {
                    "status": "PASS",
                    "mode": "dry-run",
                    "license_decisions": {
                        key: {
                            "decision": manifests[key]["license_decision"],
                            "identifier": manifests[key]["license_identifier"],
                        }
                        for key in sorted(manifests)
                    },
                    "outputs": [
                        {
                            "name": spec["name"],
                            "output": str(
                                OUTPUT_ROOT / (str(spec["name"]) + ".efkpkg")
                            ).replace("\\", "/"),
                            "layers": spec["layers"],
                            "estimated_peak_particles": spec[
                                "estimated_peak_particles"
                            ],
                            "recommended_runtime": spec["recommended_runtime"],
                        }
                        for spec in EFFECTS
                    ],
                },
                ensure_ascii=False,
                indent=2,
                sort_keys=True,
            )
        )
        return 0

    base_project = read_verified(root, "base_project")
    summaries: List[Dict[str, object]] = []
    with tempfile.TemporaryDirectory(prefix="he-survivor-toon-v2-") as temporary:
        staging = Path(temporary)
        for spec in EFFECTS:
            published, summary = build_effect(
                root, staging, spec, manifests, base_project
            )
            effect_name = str(spec["name"])
            for relative, data in published.items():
                target = root / expected_published_path(effect_name, relative)
                if args.check:
                    if not target.is_file():
                        raise GenerationError("generated output is missing: {0}".format(target))
                    actual = target.read_bytes()
                    if actual != data:
                        raise GenerationError(
                            "generated output differs from deterministic recipe: {0}".format(
                                target
                            )
                        )
                else:
                    publish_atomic(target, data)
            summaries.append(summary)

    if not args.check:
        remove_stale_generated_files(root)

    print(
        json.dumps(
            {
                "status": "PASS",
                "mode": "check" if args.check else "generate",
                "effekseer_version": EFFEKSEER_VERSION,
                "license": "CC0-1.0",
                "outputs": summaries,
            },
            ensure_ascii=False,
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (
        GenerationError,
        ET.ParseError,
        json.JSONDecodeError,
        OSError,
        subprocess.SubprocessError,
        ValueError,
        zipfile.BadZipFile,
    ) as error:
        print("ERROR: {0}".format(error), file=sys.stderr)
        sys.exit(1)
