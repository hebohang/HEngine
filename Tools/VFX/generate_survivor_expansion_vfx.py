#!/usr/bin/env python3
"""Generate the local-original Starbloom Survivor expansion VFX collection."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import ModuleType
from typing import Iterable, Mapping, Sequence
import xml.etree.ElementTree as ET
import zipfile


EFFEKSEER_VERSION = "1.80.6"
FIXED_ZIP_TIME = (2026, 8, 27, 0, 0, 0)
INTAKE_ROOT = Path(
    "Project/SourceAssets/EffekseerInbox/survivor-expansion-vfx"
)
RUNTIME_ROOT = Path("Project/Assets/Effects/Survivor")
TOOL_INSPECTION_ROOT = INTAKE_ROOT / "tool-inspection"
PROFILER_BASELINE_PATH = INTAKE_ROOT / "profiler-baseline.json"
REGRESSION_MANIFEST_PATH = INTAKE_ROOT / "regression-manifest.json"
ASSET_MANIFEST_PATH = INTAKE_ROOT / "asset.json"
AGGREGATE_INSPECTION_PATH = INTAKE_ROOT / "inspection.json"
BASE_PROJECT_PATH = Path(
    "Project/SourceAssets/EffekseerInbox/pierre02/source/"
    "Pierre02/FireBall.efkproj"
)
BASE_PROJECT_SHA256 = (
    "258d3d953740a9c7739edebc0f6992a972185a41630fd4ba5d53978c4becb490"
)
PIERRE_LICENSE_PATH = Path(
    "Project/SourceAssets/EffekseerInbox/pierre02/LICENSE.txt"
)
PIERRE_LICENSE_SHA256 = (
    "d3f89b5877210437dcde35fd97214dccf2918547d925488883209a1ea00af22c"
)


class GenerationError(RuntimeError):
    pass


class EffectSpec:
    def __init__(
        self,
        name: str,
        asset_path: str,
        family: str,
        recipe: str,
        colors: tuple[tuple[int, int, int], ...],
        authored_radius: float,
        logic_radius_contract: str,
        budget_category: str,
        quality_tiers: tuple[str, ...],
    ) -> None:
        self.name = name
        self.asset_path = asset_path
        self.family = family
        self.recipe = recipe
        self.colors = colors
        self.authored_radius = authored_radius
        self.logic_radius_contract = logic_radius_contract
        self.budget_category = budget_category
        self.quality_tiers = quality_tiers


def load_module(path: Path, name: str) -> ModuleType:
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise GenerationError("cannot load helper: {0}".format(path))
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


def load_helpers(root: Path) -> tuple[ModuleType, ModuleType]:
    toon = load_module(
        root / "Tools/VFX/generate_survivor_toon_projectiles_v2.py",
        "he_survivor_expansion_vfx_toon_helpers",
    )
    verdant = load_module(
        root / "Project/Tools/GenerateVerdantRift.py",
        "he_survivor_expansion_vfx_png_helpers",
    )
    toon.FIXED_ZIP_TIME = FIXED_ZIP_TIME
    return toon, verdant


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def verified_bytes(path: Path, expected_sha256: str) -> bytes:
    if not path.is_file():
        raise GenerationError("verified input is missing: {0}".format(path))
    payload = path.read_bytes()
    actual = sha256_bytes(payload)
    if actual != expected_sha256:
        raise GenerationError(
            "verified input hash mismatch: {0}; expected={1}, actual={2}".format(
                path, expected_sha256, actual
            )
        )
    return payload


def validate_accepted_base(root: Path) -> tuple[bytes, bytes, Mapping[str, object]]:
    manifest_path = root / "Project/SourceAssets/EffekseerInbox/pierre02/asset.json"
    if not manifest_path.is_file():
        raise GenerationError("accepted Pierre02 intake manifest is missing")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    required = {
        "asset_id": "pierre02",
        "license_identifier": "CC0-1.0",
        "license_decision": "ACCEPT",
        "commercial_use": True,
        "modification_allowed": True,
    }
    for key, value in required.items():
        if manifest.get(key) != value:
            raise GenerationError(
                "Pierre02 intake is not accepted: {0}={1!r}".format(
                    key, manifest.get(key)
                )
            )
    base_project = verified_bytes(root / BASE_PROJECT_PATH, BASE_PROJECT_SHA256)
    license_payload = verified_bytes(root / PIERRE_LICENSE_PATH, PIERRE_LICENSE_SHA256)
    return base_project, license_payload, manifest


def clamp_byte(value: float) -> int:
    return max(0, min(255, int(round(value))))


def build_source_textures(verdant: ModuleType) -> dict[str, bytes]:
    size = 96

    def coordinates(x: int, y: int) -> tuple[float, float, float, float]:
        nx = (x + 0.5) / size * 2.0 - 1.0
        ny = (y + 0.5) / size * 2.0 - 1.0
        return nx, ny, math.sqrt(nx * nx + ny * ny), math.atan2(ny, nx)

    def disc(x: int, y: int) -> tuple[int, int, int, int]:
        _, _, radius, _ = coordinates(x, y)
        falloff = max(0.0, 1.0 - max(0.0, radius - 0.18) / 0.82)
        alpha = clamp_byte(falloff ** 1.7 * 255.0)
        return 255, 255, 255, alpha if radius <= 1.0 else 0

    def ring(x: int, y: int) -> tuple[int, int, int, int]:
        _, _, radius, _ = coordinates(x, y)
        distance = abs(radius - 0.70)
        alpha = clamp_byte(max(0.0, 1.0 - distance / 0.13) ** 1.5 * 255.0)
        return 255, 255, 255, alpha

    def star(x: int, y: int) -> tuple[int, int, int, int]:
        _, _, radius, angle = coordinates(x, y)
        wave = 0.5 + 0.5 * math.cos(angle * 5.0)
        boundary = 0.34 + 0.46 * wave
        edge = boundary - radius
        alpha = clamp_byte(max(0.0, min(1.0, edge * 18.0 + 0.5)) * 255.0)
        return 255, 255, 255, alpha

    def streak(x: int, y: int) -> tuple[int, int, int, int]:
        nx, ny, _, _ = coordinates(x, y)
        metric = math.sqrt((nx / 0.94) ** 2 + (ny / 0.22) ** 2)
        alpha = clamp_byte(max(0.0, 1.0 - metric) ** 0.8 * 255.0)
        return 255, 255, 255, alpha

    texture_functions = {
        "Texture/Disc.png": disc,
        "Texture/Ring.png": ring,
        "Texture/Star.png": star,
        "Texture/Streak.png": streak,
    }
    payloads = {
        name: verdant.build_rgba_png(size, size, pixel_function)
        for name, pixel_function in texture_functions.items()
    }
    if len({sha256_bytes(payload) for payload in payloads.values()}) != len(payloads):
        raise GenerationError("project-owned procedural textures are duplicated")
    return payloads


PALETTES = {
    "thunder": ((255, 213, 76), (100, 211, 245), (255, 249, 204)),
    "ring": ((83, 219, 224), (167, 244, 232), (255, 255, 255)),
    "fireball": ((255, 102, 65), (255, 183, 67), (255, 244, 136)),
    "moonblade": ((163, 132, 255), (111, 203, 240), (255, 246, 197)),
    "bubble": ((82, 203, 238), (166, 241, 239), (246, 156, 219)),
    "tornado": ((105, 217, 194), (189, 241, 223), (178, 149, 255)),
    "starfall": ((255, 212, 92), (255, 244, 166), (255, 127, 119)),
    "aura": ((99, 215, 211), (181, 154, 255), (255, 244, 163)),
    "dagger": ((246, 242, 229), (115, 190, 238), (255, 212, 92)),
    "starwheel": ((255, 214, 86), (108, 219, 213), (255, 246, 199)),
    "petalstorm": ((244, 137, 189), (255, 183, 159), (255, 215, 92)),
    "comet": ((94, 183, 241), (113, 224, 207), (255, 242, 157)),
    "honeyneedle": ((255, 211, 71), (78, 61, 83), (112, 222, 211)),
    "sporecap": ((230, 132, 199), (117, 211, 149), (255, 238, 164)),
    "phantombell": ((142, 119, 237), (221, 230, 255), (103, 216, 222)),
    "coralanchor": ((255, 117, 100), (91, 204, 210), (255, 207, 96)),
    "prismorbit": ((104, 218, 209), (173, 139, 244), (255, 214, 84)),
    "acornmortar": ((165, 99, 55), (230, 166, 60), (119, 208, 132)),
    "frostorb": ((150, 229, 247), (102, 190, 238), (255, 255, 255)),
    "gravityyoyo": ((119, 83, 209), (244, 153, 204), (255, 210, 73)),
    "rainbowrail": ((100, 190, 239), (255, 127, 145), (255, 216, 76)),
}


IMPACT_SPECS = (
    ("Fire", "fireball", "warm core and ember edge"),
    ("WaterBubble", "bubble", "rounded cyan splash boundary"),
    ("MoonArcane", "moonblade", "lavender crescent and star flash"),
    ("Physical", "dagger", "short ivory-gold directional slash"),
    ("NaturePoison", "sporecap", "green-pink spore and petal burst"),
    ("IceElectric", "thunder", "ice-blue crack with yellow spark"),
)


BODY_WEAPONS = {
    "honeyneedle": "HoneyNeedle",
    "sporecap": "Sporecap",
    "phantombell": "PhantomBell",
    "coralanchor": "CoralAnchor",
    "prismorbit": "PrismOrbit",
    "acornmortar": "AcornMortar",
    "frostorb": "FrostOrb",
    "gravityyoyo": "GravityYoyo",
    "rainbowrail": "RainbowRail",
}


COMMON_SPECS = (
    ("EnemyProjectileOrange", "projectile", ((255, 138, 62), (255, 211, 91), (255, 244, 203)), 0.34,
     "Visible envelope is bounded to the enemy projectile collision core."),
    ("EnemyZoneTelegraph", "telegraph", ((255, 126, 74), (255, 196, 85), (255, 238, 180)), 1.00,
     "Authored unit ring outer edge is scaled 1:1 to the gameplay zone radius."),
    ("BossAreaTelegraph", "telegraph", ((255, 78, 95), (255, 176, 70), (255, 242, 169)), 1.00,
     "Authored unit double-ring outer edge is scaled 1:1 to the Boss attack radius."),
    ("BossChest", "reveal", ((255, 208, 74), (119, 220, 211), (255, 244, 197)), 0.72,
     "Presentation-only chest reveal stays inside its interaction marker."),
    ("EvolutionReveal", "reveal", ((167, 128, 255), (100, 216, 216), (255, 216, 82)), 0.90,
     "Presentation-only evolution burst is centered on the reward UI/world marker."),
    ("MonsterDeath", "burst", ((142, 119, 225), (111, 211, 194), (255, 192, 118)), 0.68,
     "Death burst remains inside the retired monster footprint and owns no damage."),
    ("HealLevelUp", "reveal", ((99, 216, 157), (104, 221, 220), (255, 235, 137)), 0.78,
     "Player-centered reward pulse is cosmetic and bounded below nearby danger telegraphs."),
)


def load_weapon_catalog(root: Path) -> list[Mapping[str, object]]:
    scripts_path = str(root / "Project/Scripts")
    if scripts_path not in sys.path:
        sys.path.insert(0, scripts_path)
    import SurvivorCatalogItems as items

    weapons = list(items.WEAPONS)
    if len(weapons) != 21:
        raise GenerationError("Survivor Catalog must contain exactly 21 weapons")
    ids = [str(weapon["id"]) for weapon in weapons]
    if ids != list(PALETTES):
        raise GenerationError(
            "VFX palette order does not match current weapon Catalog: {0}".format(ids)
        )
    return weapons


def build_specs(root: Path) -> tuple[EffectSpec, ...]:
    weapons = load_weapon_catalog(root)
    by_id = {str(weapon["id"]): weapon for weapon in weapons}
    specs: list[EffectSpec] = []

    expected_impacts = {
        "Assets/Effects/Survivor/Impacts/{0}.efkpkg".format(name)
        for name, _, _ in IMPACT_SPECS
    }
    catalog_impacts = {str(weapon["impactVfx"]) for weapon in weapons}
    if catalog_impacts != expected_impacts:
        raise GenerationError(
            "impact outputs do not match Catalog: {0}".format(
                sorted(catalog_impacts ^ expected_impacts)
            )
        )
    for name, palette_id, contract in IMPACT_SPECS:
        specs.append(EffectSpec(
            name,
            "Assets/Effects/Survivor/Impacts/{0}.efkpkg".format(name),
            "Impacts",
            "impact",
            PALETTES[palette_id],
            0.58,
            contract + " Visual envelope never exceeds the owning hit confirmation radius.",
            "Combat",
            ("Low", "Medium", "High"),
        ))

    for weapon_id, effect_name in BODY_WEAPONS.items():
        weapon = by_id[weapon_id]
        expected_path = "Assets/Effects/Survivor/Weapons/{0}.efkpkg".format(
            effect_name
        )
        if weapon["bodyVfx"] != expected_path:
            raise GenerationError(
                "{0} bodyVfx mismatch: {1}".format(weapon_id, weapon["bodyVfx"])
            )
        logic_radius = float(weapon["area"])
        authored_radius = min(0.90, max(0.30, logic_radius * 0.50))
        specs.append(EffectSpec(
            effect_name,
            expected_path,
            "Weapons",
            "body",
            PALETTES[weapon_id],
            authored_radius,
            (
                "Body/trail envelope is {0:.3f} authored units and stays within "
                "the Catalog area/hit radius {1:.3f}; gameplay owns collision."
            ).format(authored_radius, logic_radius),
            "Combat",
            ("Low", "Medium", "High"),
        ))

    for weapon in weapons:
        weapon_id = str(weapon["id"])
        expected_path = "Assets/Effects/Survivor/Casts/{0}.efkpkg".format(
            weapon_id
        )
        if weapon["castVfx"] != expected_path:
            raise GenerationError(
                "{0} castVfx mismatch: {1}".format(weapon_id, weapon["castVfx"])
            )
        specs.append(EffectSpec(
            weapon_id,
            expected_path,
            "Casts",
            "cast",
            PALETTES[weapon_id],
            0.38,
            "Origin-only cast flash has no gameplay radius and remains below projectile/telegraph priority.",
            "Combat",
            ("Low", "Medium", "High"),
        ))

    for name, recipe, colors, radius, contract in COMMON_SPECS:
        specs.append(EffectSpec(
            name,
            "Assets/Effects/Survivor/Common/{0}.efkpkg".format(name),
            "Common",
            recipe,
            colors,
            radius,
            contract,
            "GameplayCritical",
            ("Low", "Medium", "High"),
        ))

    paths = [spec.asset_path for spec in specs]
    if len(specs) != 43 or len(set(paths)) != 43:
        raise GenerationError("expected 43 unique expansion VFX package paths")
    return tuple(specs)


def colored(rgb: tuple[int, int, int], alpha: int) -> tuple[int, int, int, int]:
    return rgb[0], rgb[1], rgb[2], alpha


def impact_nodes(toon: ModuleType, spec: EffectSpec) -> list[ET.Element]:
    diameter = spec.authored_radius * 2.0
    primary, secondary, accent = spec.colors
    return [
        toon.make_sprite(
            spec.name + "Core", "Texture/Disc.png", colored(primary, 220),
            (diameter * 0.18,) * 3, (diameter * 0.72,) * 3, 18,
            fade_in=1, fade_out=8,
        ),
        toon.make_sprite(
            spec.name + "Boundary", "Texture/Ring.png", colored(secondary, 185),
            (diameter * 0.20,) * 3, (diameter,) * 3, 24,
            fade_in=1, fade_out=12,
        ),
        toon.make_sprite(
            spec.name + "Stars", "Texture/Star.png", colored(accent, 230),
            (diameter * 0.12,) * 3, (diameter * 0.30,) * 3, 22,
            max_generation=3, generation_time=2.0,
            location_min=(-spec.authored_radius * 0.20,) * 3,
            location_max=(spec.authored_radius * 0.20,) * 3,
            velocity_min=(-0.035, 0.025, -0.035),
            velocity_max=(0.035, 0.070, 0.035),
            rotation_velocity_min=(0.0, 0.0, -5.0),
            rotation_velocity_max=(0.0, 0.0, 5.0),
            fade_out=10,
        ),
        toon.make_sprite(
            spec.name + "Directional", "Texture/Streak.png", colored(primary, 175),
            (diameter * 0.12, diameter * 0.05, diameter * 0.12),
            (diameter * 0.68, diameter * 0.12, diameter * 0.68), 16,
            max_generation=2, generation_time=3.0,
            rotation_min=(0.0, 0.0, -90.0),
            rotation_max=(0.0, 0.0, 90.0),
            fade_out=8,
        ),
    ]


def cast_nodes(toon: ModuleType, spec: EffectSpec) -> list[ET.Element]:
    diameter = spec.authored_radius * 2.0
    primary, secondary, accent = spec.colors
    return [
        toon.make_sprite(
            spec.name + "CastRing", "Texture/Ring.png", colored(primary, 170),
            (diameter * 0.18,) * 3, (diameter,) * 3, 20,
            fade_in=1, fade_out=10,
        ),
        toon.make_sprite(
            spec.name + "CastStars", "Texture/Star.png", colored(accent, 225),
            (diameter * 0.16,) * 3, (diameter * 0.35,) * 3, 20,
            max_generation=2, generation_time=3.0,
            location_min=(-0.10, 0.02, -0.10),
            location_max=(0.10, 0.16, 0.10),
            fade_out=9,
        ),
        toon.make_sprite(
            spec.name + "CastStreaks", "Texture/Streak.png", colored(secondary, 185),
            (diameter * 0.18, diameter * 0.05, diameter * 0.18),
            (diameter * 0.72, diameter * 0.10, diameter * 0.72), 18,
            max_generation=2, generation_time=3.0,
            rotation_min=(0.0, 0.0, -70.0),
            rotation_max=(0.0, 0.0, 70.0),
            fade_out=8,
        ),
    ]


def body_nodes(toon: ModuleType, spec: EffectSpec) -> list[ET.Element]:
    diameter = spec.authored_radius * 2.0
    primary, secondary, accent = spec.colors
    return [
        toon.make_sprite(
            spec.name + "BodyCore", "Texture/Disc.png", colored(primary, 190),
            (diameter * 0.58,) * 3, (diameter * 0.52,) * 3, 54,
            fade_in=2, fade_out=12,
        ),
        toon.make_sprite(
            spec.name + "BodyRing", "Texture/Ring.png", colored(secondary, 155),
            (diameter * 0.62,) * 3, (diameter * 0.92,) * 3, 54,
            rotation_velocity=(0.0, 0.0, 2.5), fade_in=2, fade_out=12,
        ),
        toon.make_sprite(
            spec.name + "BodyMotes", "Texture/Star.png", colored(accent, 215),
            (diameter * 0.10,) * 3, (diameter * 0.20,) * 3, 38,
            max_generation=3, generation_time=7.0,
            location_min=(-spec.authored_radius * 0.42,) * 3,
            location_max=(spec.authored_radius * 0.42,) * 3,
            fade_out=12,
        ),
        toon.make_sprite(
            spec.name + "BodyTrail", "Texture/Streak.png", colored(primary, 145),
            (diameter * 0.18, diameter * 0.06, diameter * 0.18),
            (diameter * 0.58, diameter * 0.10, diameter * 0.58), 28,
            max_generation=4, generation_time=6.0,
            location_min=(-spec.authored_radius * 0.28, -0.03, -spec.authored_radius * 0.28),
            location_max=(spec.authored_radius * 0.28, 0.03, spec.authored_radius * 0.28),
            velocity=(0.0, 0.025, 0.0), fade_out=10,
        ),
    ]


def telegraph_nodes(toon: ModuleType, spec: EffectSpec) -> list[ET.Element]:
    diameter = spec.authored_radius * 2.0
    primary, secondary, accent = spec.colors
    return [
        toon.make_sprite(
            spec.name + "OuterBoundary", "Texture/Ring.png", colored(primary, 150),
            (diameter * 0.98, diameter * 0.98, 1.0),
            (diameter, diameter, 1.0), 72,
            rotation=(90.0, 0.0, 0.0), billboard=2,
            fade_in=8, fade_out=8,
        ),
        toon.make_sprite(
            spec.name + "InnerPulse", "Texture/Ring.png", colored(secondary, 105),
            (diameter * 0.45, diameter * 0.45, 1.0),
            (diameter * 0.86, diameter * 0.86, 1.0), 72,
            rotation=(90.0, 0.0, 0.0), billboard=2,
            fade_in=8, fade_out=8,
        ),
        toon.make_sprite(
            spec.name + "EdgeTicks", "Texture/Streak.png", colored(accent, 155),
            (diameter * 0.16, diameter * 0.035, 1.0),
            (diameter * 0.28, diameter * 0.05, 1.0), 60,
            max_generation=4, generation_time=12.0,
            location_min=(-spec.authored_radius * 0.72, 0.01, -spec.authored_radius * 0.72),
            location_max=(spec.authored_radius * 0.72, 0.01, spec.authored_radius * 0.72),
            rotation=(90.0, 0.0, 0.0),
            rotation_min=(90.0, 0.0, -180.0),
            rotation_max=(90.0, 0.0, 180.0),
            billboard=2, fade_in=6, fade_out=8,
        ),
        toon.make_sprite(
            spec.name + "CenterMark", "Texture/Star.png", colored(accent, 90),
            (diameter * 0.16, diameter * 0.16, 1.0),
            (diameter * 0.22, diameter * 0.22, 1.0), 72,
            rotation=(90.0, 0.0, 0.0), billboard=2,
            fade_in=8, fade_out=8,
        ),
    ]


def burst_nodes(toon: ModuleType, spec: EffectSpec) -> list[ET.Element]:
    diameter = spec.authored_radius * 2.0
    primary, secondary, accent = spec.colors
    return [
        toon.make_sprite(
            spec.name + "SoftPop", "Texture/Disc.png", colored(primary, 175),
            (diameter * 0.22,) * 3, (diameter * 0.78,) * 3, 28,
            fade_out=14,
        ),
        toon.make_sprite(
            spec.name + "BreakRing", "Texture/Ring.png", colored(secondary, 160),
            (diameter * 0.30,) * 3, (diameter,) * 3, 30,
            fade_out=14,
        ),
        toon.make_sprite(
            spec.name + "Fragments", "Texture/Star.png", colored(accent, 210),
            (diameter * 0.10,) * 3, (diameter * 0.22,) * 3, 30,
            max_generation=4, generation_time=2.0,
            location_min=(-0.12, 0.00, -0.12),
            location_max=(0.12, 0.20, 0.12),
            velocity_min=(-0.040, 0.025, -0.040),
            velocity_max=(0.040, 0.090, 0.040),
            fade_out=15,
        ),
        toon.make_sprite(
            spec.name + "ExitStreaks", "Texture/Streak.png", colored(primary, 125),
            (diameter * 0.14, diameter * 0.04, diameter * 0.14),
            (diameter * 0.54, diameter * 0.08, diameter * 0.54), 24,
            max_generation=3, generation_time=3.0,
            rotation_min=(0.0, 0.0, -180.0),
            rotation_max=(0.0, 0.0, 180.0), fade_out=12,
        ),
    ]


def reveal_nodes(toon: ModuleType, spec: EffectSpec) -> list[ET.Element]:
    nodes = burst_nodes(toon, spec)
    diameter = spec.authored_radius * 2.0
    nodes.append(toon.make_sprite(
        spec.name + "RevealCrown", "Texture/Star.png", colored(spec.colors[2], 230),
        (diameter * 0.15,) * 3, (diameter * 0.42,) * 3, 42,
        max_generation=3, generation_time=6.0,
        location_min=(-0.20, 0.20, -0.20),
        location_max=(0.20, 0.50, 0.20),
        velocity=(0.0, 0.025, 0.0), fade_out=16,
    ))
    return nodes


def projectile_nodes(toon: ModuleType, spec: EffectSpec) -> list[ET.Element]:
    return body_nodes(toon, spec)


RECIPE_BUILDERS = {
    "impact": impact_nodes,
    "cast": cast_nodes,
    "body": body_nodes,
    "telegraph": telegraph_nodes,
    "burst": burst_nodes,
    "reveal": reveal_nodes,
    "projectile": projectile_nodes,
}


def build_nodes(toon: ModuleType, spec: EffectSpec) -> tuple[list[ET.Element], int, int]:
    try:
        nodes = RECIPE_BUILDERS[spec.recipe](toon, spec)
    except KeyError as error:
        raise GenerationError("unknown recipe: {0}".format(spec.recipe)) from error
    names: list[str] = []
    textures: set[str] = set()
    peak_particles = 0
    for node in nodes:
        name_node = node.find("Name")
        texture_node = node.find("RendererCommonValues/ColorTexture")
        generation_node = node.find("CommonValues/MaxGeneration/Value")
        if name_node is None or not name_node.text:
            raise GenerationError("generated node has no stable name")
        if texture_node is None or not texture_node.text:
            raise GenerationError("generated node has no texture")
        if generation_node is None or not generation_node.text:
            raise GenerationError("generated node has no generation budget")
        names.append(name_node.text)
        textures.add(texture_node.text)
        peak_particles += int(generation_node.text)
    if len(nodes) < 3 or len(set(names)) != len(names) or len(textures) < 2:
        raise GenerationError(
            "{0} must have >=3 unique nodes and >=2 textures".format(spec.name)
        )
    return nodes, peak_particles, 96 if spec.recipe == "telegraph" else 72


PROJECT_LICENSE_TEXT = b"""HEngine project-owned Starbloom Survivor expansion VFX\n\nThe procedural textures, effect recipes, color choices, timing, scale contracts,\nand provenance records generated by Tools/VFX/generate_survivor_expansion_vfx.py\nare original project-owned work created for HEngineDev. They may be used,\nmodified, cooked, and redistributed as part of Starbloom Survivor.\n\nNo downloaded texture, model, audio, marketplace asset, or legacy\nMANUAL_REVIEW effect is used by this collection.\n"""


def licenses_markdown() -> bytes:
    return (
        "# Survivor expansion VFX licenses\n\n"
        "All visible textures and all authored node recipes are deterministic "
        "HEngine project-owned work; see `source/HEngine-Project-Owned.txt`.\n\n"
        "The empty project structure used for Effekseer XML serialization is "
        "derived from the already reviewed `pierre02` official Sample Effects "
        "intake: `license_decision: ACCEPT`, `license_identifier: CC0-1.0`. "
        "The complete retained license is copied to every derived package and "
        "remains at `../pierre02/LICENSE.txt`.\n\n"
        "No legacy `MANUAL_REVIEW` runtime package is an input.\n"
    ).encode("utf-8")


def source_readme(texture_payloads: Mapping[str, bytes]) -> bytes:
    rows = [
        "| File | SHA-256 | Purpose |",
        "| --- | --- | --- |",
    ]
    purpose = {
        "Texture/Disc.png": "soft bounded core",
        "Texture/Ring.png": "low-fill radius boundary",
        "Texture/Star.png": "readable accent/glint",
        "Texture/Streak.png": "directional trail/tick",
    }
    for name, payload in sorted(texture_payloads.items()):
        rows.append(
            "| `source/{0}` | `{1}` | {2} |".format(
                name, sha256_bytes(payload).upper(), purpose[name]
            )
        )
    return (
        "# Survivor expansion VFX source\n\n"
        "These 96x96 RGBA textures are generated pixel-by-pixel by "
        "`Tools/VFX/generate_survivor_expansion_vfx.py`. They are white "
        "alpha masks tinted by Effekseer nodes, use no external image input, "
        "and are intentionally simple for low overdraw.\n\n"
        + "\n".join(rows)
        + "\n"
    ).encode("utf-8")


def source_reference_payload(pierre_manifest: Mapping[str, object]) -> bytes:
    value = {
        "schema_version": 1,
        "visible_asset_source": "HEngine project-owned procedural textures and node recipes",
        "base_project_structure": {
            "path": BASE_PROJECT_PATH.as_posix(),
            "sha256": BASE_PROJECT_SHA256,
            "intake_asset_id": "pierre02",
            "intake_license_identifier": pierre_manifest["license_identifier"],
            "intake_license_decision": pierre_manifest["license_decision"],
            "license_evidence": PIERRE_LICENSE_PATH.as_posix(),
            "license_sha256": PIERRE_LICENSE_SHA256,
        },
        "excluded_sources": [
            "All legacy MANUAL_REVIEW Effekseer packages",
            "All downloaded marketplace/community content",
        ],
    }
    return json_bytes(value)


def json_bytes(value: object) -> bytes:
    return (
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")


def effect_relative_stem(spec: EffectSpec) -> Path:
    prefix = "Assets/Effects/Survivor/"
    if not spec.asset_path.startswith(prefix) or not spec.asset_path.endswith(".efkpkg"):
        raise GenerationError("unsafe runtime effect path: {0}".format(spec.asset_path))
    relative = Path(spec.asset_path[len(prefix):]).with_suffix("")
    if relative.is_absolute() or ".." in relative.parts or "\\" in spec.asset_path:
        raise GenerationError("runtime effect path escaped Survivor root")
    return relative


def derived_directory(spec: EffectSpec) -> Path:
    return INTAKE_ROOT / "derived" / effect_relative_stem(spec)


def recipe_inspection_path(spec: EffectSpec) -> Path:
    return INTAKE_ROOT / "recipe-inspection" / effect_relative_stem(spec).with_suffix(
        ".json"
    )


def tool_inspection_path(spec: EffectSpec) -> Path:
    return TOOL_INSPECTION_ROOT / effect_relative_stem(spec).with_suffix(".json")


def package_provenance(
    spec: EffectSpec,
    project: bytes,
    texture_payloads: Mapping[str, bytes],
    peak_particles: int,
    node_count: int,
) -> bytes:
    value = {
        "schema_version": 1,
        "asset_id": "survivor-expansion-vfx-" + "-".join(
            part.lower() for part in effect_relative_stem(spec).parts
        ),
        "author": "HEngine project",
        "generator": "Tools/VFX/generate_survivor_expansion_vfx.py",
        "tool": {"name": "Effekseer", "version": EFFEKSEER_VERSION},
        "source": {
            "kind": "local-original-derivative",
            "visible_assets": "project-owned procedural masks",
            "base_project_path": BASE_PROJECT_PATH.as_posix(),
            "base_project_sha256": BASE_PROJECT_SHA256,
            "base_project_license": "CC0-1.0 / ACCEPT / pierre02",
        },
        "license_identifier": "HENGINE-PROJECT-OWNED+CC0-1.0",
        "license_decision": "ACCEPT",
        "project_sha256": sha256_bytes(project),
        "textures": [
            {"path": name, "sha256": sha256_bytes(payload)}
            for name, payload in sorted(texture_payloads.items())
        ],
        "node_count": node_count,
        "estimated_peak_particles": peak_particles,
        "uses_gpu_particles": False,
        "has_cpu_fallback": True,
        "uses_distortion": False,
        "supported_renderer_apis": ["DX12"],
        "budget_category": spec.budget_category,
        "quality_tier_support": list(spec.quality_tiers),
        "authored_radius": spec.authored_radius,
        "logic_radius_contract": spec.logic_radius_contract,
        "gameplay_contract": "Particles are presentation-only; gameplay owns damage, collision, targeting and timing.",
        "modifications": [
            "Replaced all base-project nodes with bounded CPU sprite nodes.",
            "Used at least two distinct project-owned alpha-mask textures.",
            "Compiled as an editable EFKE container with Effekseer 1.80.6.",
        ],
    }
    return json_bytes(value)


def validate_project_recipe(
    project: bytes,
    spec: EffectSpec,
    expected_node_count: int,
) -> None:
    if b"GpuParticle" in project or b"GPU_Particle" in project:
        raise GenerationError("generated project unexpectedly contains GPU particles")
    root = ET.fromstring(project)
    nodes = root.findall("Root/Children/Node")
    if len(nodes) != expected_node_count or len(nodes) < 3:
        raise GenerationError("generated project node count drifted")
    textures = {
        node.findtext("RendererCommonValues/ColorTexture", "") for node in nodes
    }
    textures.discard("")
    if len(textures) < 2:
        raise GenerationError("generated project uses fewer than two textures")
    if spec.recipe == "telegraph" and "Texture/Disc.png" in textures:
        raise GenerationError("gameplay telegraph must not use an occluding fill disc")


def compile_and_package_effect(
    root: Path,
    toon: ModuleType,
    staging: Path,
    spec: EffectSpec,
    base_project: bytes,
    pierre_license: bytes,
    source_textures: Mapping[str, bytes],
) -> tuple[dict[Path, bytes], dict[str, object]]:
    nodes, peak_particles, end_frame = build_nodes(toon, spec)
    project = toon.build_project(base_project, nodes, end_frame)
    validate_project_recipe(project, spec, len(nodes))
    used_textures = sorted({
        node.findtext("RendererCommonValues/ColorTexture", "") for node in nodes
    })
    if any(texture not in source_textures for texture in used_textures):
        raise GenerationError("generated effect references an unknown texture")
    effect_name = Path(spec.asset_path).stem
    stage = staging / effect_relative_stem(spec)
    stage.mkdir(parents=True, exist_ok=True)
    project_path = stage / (effect_name + ".efkproj")
    project_path.write_bytes(project)
    for texture in used_textures:
        texture_path = stage / texture
        texture_path.parent.mkdir(parents=True, exist_ok=True)
        texture_path.write_bytes(source_textures[texture])

    editor = root / "Engine/Tools/Effekseer/bin/Effekseer.exe"
    if not editor.is_file():
        raise GenerationError("bundled Effekseer editor is missing")
    compiled_path = stage / (effect_name + ".efkefc")
    toon.compile_effect(editor, project_path, compiled_path)
    compiled = compiled_path.read_bytes()

    texture_subset = {name: source_textures[name] for name in used_textures}
    provenance = package_provenance(
        spec, project, texture_subset, peak_particles, len(nodes)
    )
    package_entries: dict[str, bytes] = {
        effect_name + ".efkefc": compiled,
        "PROVENANCE.json": provenance,
        "LICENSES/HEngine-Project-Owned.txt": PROJECT_LICENSE_TEXT,
        "LICENSES/pierre02-CC0-1.0.txt": pierre_license,
    }
    package_entries.update(texture_subset)
    package = toon.deterministic_zip(package_entries)

    runtime_project_path = Path("Project") / Path(spec.asset_path)
    effect_derived_root = derived_directory(spec)
    outputs: dict[Path, bytes] = {
        runtime_project_path: package,
        effect_derived_root / (effect_name + ".efkproj"): project,
        effect_derived_root / (effect_name + ".efkefc"): compiled,
        effect_derived_root / "PROVENANCE.json": provenance,
        effect_derived_root / "LICENSES/HEngine-Project-Owned.txt": PROJECT_LICENSE_TEXT,
        effect_derived_root / "LICENSES/pierre02-CC0-1.0.txt": pierre_license,
    }
    for texture, payload in texture_subset.items():
        outputs[effect_derived_root / texture] = payload

    recipe_inspection = {
        "schema_version": 1,
        "effect": effect_name,
        "family": spec.family,
        "runtime_path": spec.asset_path,
        "package_sha256": sha256_bytes(package),
        "package_size": len(package),
        "package_entries": sorted(package_entries),
        "editable_efke": True,
        "effekseer_version": EFFEKSEER_VERSION,
        "runtime_binary_generation": 1810,
        "node_count": len(nodes),
        "texture_count": len(used_textures),
        "textures": used_textures,
        "estimated_peak_particles": peak_particles,
        "uses_gpu_particles": {
            "value": False,
            "status": "ExactGeneratedRecipe",
        },
        "uses_distortion": {
            "value": False,
            "status": "ExactGeneratedRecipe",
        },
        "has_cpu_fallback": True,
        "budget_category": spec.budget_category,
        "quality_tier_support": list(spec.quality_tiers),
        "authored_radius": spec.authored_radius,
        "logic_radius_contract": spec.logic_radius_contract,
        "low_occlusion": (
            "No fill disc; boundary-only ring/ticks"
            if spec.recipe == "telegraph"
            else "Bounded additive masks with alpha <= 230"
        ),
        "missing_dependencies": [],
    }
    inspection_payload = json_bytes(recipe_inspection)
    outputs[recipe_inspection_path(spec)] = inspection_payload
    summary = dict(recipe_inspection)
    summary["project_sha256"] = sha256_bytes(project)
    summary["compiled_sha256"] = sha256_bytes(compiled)
    summary["derived_root"] = effect_derived_root.as_posix()
    return outputs, summary


def default_profiler_baseline() -> dict[str, object]:
    unavailable = {"status": "Unavailable", "value": None}
    return {
        "schema_version": 1,
        "status": "PendingRuntimeMeasurement",
        "tested_at": None,
        "build_type": "Unavailable",
        "renderer_api": "DX12",
        "gpu": "Unavailable",
        "driver": "Unavailable",
        "resolution": "Unavailable",
        "quality_tier": "High",
        "warmup_policy": "Unavailable",
        "sample_duration_seconds": None,
        "runtime_regression": {
            "status": "Pending",
            "expected": 43,
            "passed": 0,
            "failed": 0,
            "command": (
                "$env:HE_VFX_VALIDATE_PACKAGES='1'; "
                "$env:HE_VFX_VALIDATION_MANIFEST='SourceAssets/EffekseerInbox/"
                "survivor-expansion-vfx/regression-manifest.json'; "
                "./hectl.bat game check"
            ),
            "log": None,
        },
        "showcase_samples": [],
        "metrics": {
            "cpu_update_ms": dict(unavailable),
            "cpu_draw_submit_ms": dict(unavailable),
            "gpu_simulation_ms": dict(unavailable),
            "gpu_render_ms": dict(unavailable),
            "total_frame_ms": {"status": "UnavailableUseTracy", "value": None},
            "draw_calls": dict(unavailable),
            "vertex_count": dict(unavailable),
            "active_cpu_instances": dict(unavailable),
            "particle_buffer_used": dict(unavailable),
            "particle_buffer_capacity": dict(unavailable),
        },
        "limitations": [
            "Detailed GPU and total-frame counters are not inferred when unavailable.",
        ],
    }


def load_profiler_baseline(root: Path) -> tuple[dict[str, object], bool]:
    path = root / PROFILER_BASELINE_PATH
    if not path.is_file():
        return default_profiler_baseline(), True
    baseline = json.loads(path.read_text(encoding="utf-8"))
    if baseline.get("schema_version") != 1:
        raise GenerationError("profiler baseline schema is invalid")
    if baseline.get("renderer_api") != "DX12":
        raise GenerationError("profiler baseline must describe DX12")
    return baseline, False


def runtime_regression_passed(baseline: Mapping[str, object]) -> bool:
    regression = baseline.get("runtime_regression")
    if not isinstance(regression, dict):
        return False
    return (
        regression.get("status") == "PASS"
        and regression.get("expected") == 43
        and regression.get("passed") == 43
        and regression.get("failed") == 0
    )


def showcase_passed(baseline: Mapping[str, object]) -> bool:
    samples = baseline.get("showcase_samples")
    return (
        isinstance(samples, list)
        and len(samples) >= 4
        and all(isinstance(sample, dict) and sample.get("status") == "PASS" for sample in samples)
    )


def inspect_tool_reports(
    root: Path,
    specs: Sequence[EffectSpec],
    summaries: Mapping[str, Mapping[str, object]],
) -> tuple[dict[str, object], bool]:
    reports: list[dict[str, object]] = []
    all_safe = True
    for spec in specs:
        relative = tool_inspection_path(spec)
        path = root / relative
        result: dict[str, object] = {
            "effect": spec.name,
            "runtime_path": spec.asset_path,
            "report": relative.as_posix(),
            "status": "Pending",
            "safe": False,
            "scanner_gpu_detection": "Unavailable",
            "scanner_version_detection": "Unavailable",
        }
        if not path.is_file():
            all_safe = False
            reports.append(result)
            continue
        report = json.loads(path.read_text(encoding="utf-8"))
        expected_sha = summaries[spec.asset_path]["package_sha256"]
        archive = report.get("archive", {})
        safety = report.get("safety", {})
        missing_dependencies = report.get("missing_dependencies", [])
        textures = report.get("textures", [])
        safe = (
            archive.get("sha256") == expected_sha
            and safety.get("safe") is True
            and missing_dependencies == []
            and isinstance(textures, list)
            and len(textures) >= 2
        )
        result.update({
            "status": "PASS" if safe else "FAIL",
            "safe": safe,
            "archive_sha256": archive.get("sha256"),
            "missing_dependencies": missing_dependencies,
            "texture_count": len(textures) if isinstance(textures, list) else None,
            "scanner_gpu_detection": report.get(
                "gpu_particles_detection", {}
            ).get("status", "Unavailable"),
            "scanner_version_detection": report.get(
                "effect_version_detection", {}
            ).get("status", "Unavailable"),
        })
        if not safe:
            all_safe = False
        reports.append(result)
    return {
        "status": "PASS" if all_safe else "PendingOrFailed",
        "expected": len(specs),
        "present": sum(report["status"] != "Pending" for report in reports),
        "passed": sum(report["status"] == "PASS" for report in reports),
        "reports": reports,
        "note": (
            "Opaque scanner GPU/version Unavailable values are preserved. "
            "Exact CPU-only and 1.80.6 evidence comes from the generated XML, "
            "bundled compiler, EFKE chunk validation, and HEngine runtime regression."
        ),
    }, all_safe


def build_aggregate_inspection(
    tool_summary: Mapping[str, object],
    baseline: Mapping[str, object],
    specs: Sequence[EffectSpec],
    summaries: Mapping[str, Mapping[str, object]],
) -> dict[str, object]:
    runtime_pass = runtime_regression_passed(baseline)
    return {
        "schema_version": 1,
        "asset_id": "survivor-expansion-vfx",
        "package_count": len(specs),
        "archive_safety": tool_summary,
        "recipe_validation": {
            "status": "PASS",
            "effekseer_version": EFFEKSEER_VERSION,
            "editable_efke": True,
            "runtime_binary_generation": 1810,
            "uses_gpu_particles": {
                "value": False,
                "status": "ExactGeneratedRecipe",
            },
            "has_cpu_fallback": True,
            "uses_distortion": {
                "value": False,
                "status": "ExactGeneratedRecipe",
            },
            "minimum_nodes": min(int(summary["node_count"]) for summary in summaries.values()),
            "minimum_textures": min(int(summary["texture_count"]) for summary in summaries.values()),
            "all_logic_radius_contracts_recorded": all(
                bool(spec.logic_radius_contract) for spec in specs
            ),
        },
        "runtime_validation": baseline.get("runtime_regression"),
        "showcase_samples": baseline.get("showcase_samples", []),
        "supported_renderer_apis": ["DX12"] if runtime_pass else [],
        "status": (
            "PASS"
            if tool_summary.get("status") == "PASS" and runtime_pass
            else "PENDING_RUNTIME_OR_TOOL_REVIEW"
        ),
    }


def build_asset_manifest(
    specs: Sequence[EffectSpec],
    summaries: Mapping[str, Mapping[str, object]],
    aggregate_inspection: Mapping[str, object],
    baseline: Mapping[str, object],
) -> dict[str, object]:
    runtime_pass = runtime_regression_passed(baseline)
    ready = (
        aggregate_inspection.get("archive_safety", {}).get("status") == "PASS"
        and runtime_pass
        and showcase_passed(baseline)
    )
    imported_files = [
        {
            "path": "Project/" + spec.asset_path,
            "sha256": summaries[spec.asset_path]["package_sha256"],
            "size": summaries[spec.asset_path]["package_size"],
        }
        for spec in specs
    ]
    effect_sources: list[str] = []
    for spec in specs:
        effect_name = Path(spec.asset_path).stem
        root = derived_directory(spec)
        effect_sources.extend((
            (root / (effect_name + ".efkproj")).as_posix(),
            (root / (effect_name + ".efkefc")).as_posix(),
        ))
    return {
        "schema_version": 1,
        "asset_id": "survivor-expansion-vfx",
        "source": "HEngine project-owned deterministic VFX generator with ACCEPT/CC0 Pierre02 XML base structure",
        "source_url": "local://Tools/VFX/generate_survivor_expansion_vfx.py",
        "author": "HEngine project",
        "acquired_at": None,
        "sha256": None,
        "license_identifier": "HENGINE-PROJECT-OWNED+CC0-1.0",
        "license_decision": "ACCEPT",
        "license_evidence": (INTAKE_ROOT / "LICENSES.md").as_posix(),
        "commercial_use": True,
        "attribution_required": False,
        "attribution_text": None,
        "modification_allowed": True,
        "redistribution_restrictions": "Project-owned work plus CC0 base imposes no additional restriction",
        "engine_restrictions": None,
        "original_download": None,
        "minimum_effekseer_version": EFFEKSEER_VERSION,
        "source_effekseer_version": EFFEKSEER_VERSION,
        "uses_gpu_particles": False,
        "supported_renderer_apis": ["DX12"] if runtime_pass else [],
        "has_cpu_fallback": True,
        "estimated_peak_particles": max(
            int(summary["estimated_peak_particles"]) for summary in summaries.values()
        ),
        "vfx_budget_category": "GameplayCritical",
        "quality_tier_support": ["Low", "Medium", "High"],
        "profiler_baseline": baseline,
        "effect_sources": effect_sources,
        "textures": [
            (INTAKE_ROOT / "source" / texture).as_posix()
            for texture in ("Texture/Disc.png", "Texture/Ring.png", "Texture/Star.png", "Texture/Streak.png")
        ],
        "models": [],
        "materials": [],
        "audio": [],
        "inspection": {
            "safe": aggregate_inspection.get("archive_safety", {}).get("status") == "PASS",
            "report_path": AGGREGATE_INSPECTION_PATH.as_posix(),
            "package_count": len(specs),
            "status": aggregate_inspection.get("status"),
        },
        "imported_files": imported_files,
        "final_import_path": (
            "Project/Assets/Effects/Survivor/Common/BossAreaTelegraph.efkpkg"
            if ready else None
        ),
        "modifications": [
            {
                "recorded_at": "2026-08-27T00:00:00Z",
                "description": (
                    "Generated 43 bounded multi-node CPU effects with project-owned "
                    "procedural masks and Effekseer 1.80.6."
                ),
            }
        ],
        "intake_status": "IMPORTED" if ready else "GENERATED_PENDING_RUNTIME_REVIEW",
    }


def assert_forward_slashes(value: object, location: str = "root") -> None:
    if isinstance(value, str):
        if "\\" in value:
            raise GenerationError("backslash in {0}: {1}".format(location, value))
    elif isinstance(value, dict):
        for key, child in value.items():
            assert_forward_slashes(child, location + "." + str(key))
    elif isinstance(value, (list, tuple)):
        for index, child in enumerate(value):
            assert_forward_slashes(child, "{0}[{1}]".format(location, index))


def build_collection(
    root: Path,
    staging: Path,
) -> tuple[dict[Path, bytes], dict[str, object], bool]:
    toon, verdant = load_helpers(root)
    base_project, pierre_license, pierre_manifest = validate_accepted_base(root)
    texture_payloads = build_source_textures(verdant)
    specs = build_specs(root)
    outputs: dict[Path, bytes] = {}
    summaries_by_path: dict[str, dict[str, object]] = {}

    outputs[INTAKE_ROOT / "LICENSES.md"] = licenses_markdown()
    outputs[INTAKE_ROOT / "source/README.md"] = source_readme(texture_payloads)
    outputs[INTAKE_ROOT / "source/HEngine-Project-Owned.txt"] = PROJECT_LICENSE_TEXT
    outputs[INTAKE_ROOT / "source/BASE_PROJECT_REFERENCE.json"] = (
        source_reference_payload(pierre_manifest)
    )
    for texture_name, payload in texture_payloads.items():
        outputs[INTAKE_ROOT / "source" / texture_name] = payload

    for spec in specs:
        effect_outputs, summary = compile_and_package_effect(
            root,
            toon,
            staging,
            spec,
            base_project,
            pierre_license,
            texture_payloads,
        )
        overlap = set(outputs) & set(effect_outputs)
        if overlap:
            raise GenerationError("generated output path collision: {0}".format(overlap))
        outputs.update(effect_outputs)
        summaries_by_path[spec.asset_path] = summary

    regression_manifest = {
        "SchemaVersion": 1,
        "Packages": [spec.asset_path for spec in specs],
    }
    outputs[REGRESSION_MANIFEST_PATH] = json_bytes(regression_manifest)

    baseline, baseline_missing = load_profiler_baseline(root)
    tool_summary, _ = inspect_tool_reports(root, specs, summaries_by_path)
    aggregate_inspection = build_aggregate_inspection(
        tool_summary, baseline, specs, summaries_by_path
    )
    asset_manifest = build_asset_manifest(
        specs, summaries_by_path, aggregate_inspection, baseline
    )
    assert_forward_slashes(aggregate_inspection, "inspection")
    assert_forward_slashes(asset_manifest, "asset")
    outputs[AGGREGATE_INSPECTION_PATH] = json_bytes(aggregate_inspection)
    outputs[ASSET_MANIFEST_PATH] = json_bytes(asset_manifest)

    generation_report = {
        "schema_version": 1,
        "generator": "Tools/VFX/generate_survivor_expansion_vfx.py",
        "effekseer_version": EFFEKSEER_VERSION,
        "package_count": len(specs),
        "families": {
            family: sum(spec.family == family for spec in specs)
            for family in ("Impacts", "Weapons", "Casts", "Common")
        },
        "outputs": [summaries_by_path[spec.asset_path] for spec in specs],
    }
    outputs[INTAKE_ROOT / "generation-report.json"] = json_bytes(generation_report)

    report = {
        "package_count": len(specs),
        "families": generation_report["families"],
        "license_decision": "ACCEPT",
        "license_identifier": "HENGINE-PROJECT-OWNED+CC0-1.0",
        "minimum_nodes": min(
            int(summary["node_count"]) for summary in summaries_by_path.values()
        ),
        "minimum_textures": min(
            int(summary["texture_count"]) for summary in summaries_by_path.values()
        ),
        "archive_tool_reports": tool_summary["present"],
        "archive_tool_passed": tool_summary["passed"],
        "runtime_regression": baseline.get("runtime_regression"),
        "intake_status": asset_manifest["intake_status"],
        "outputs": [
            {
                "path": spec.asset_path,
                "sha256": summaries_by_path[spec.asset_path]["package_sha256"],
                "nodes": summaries_by_path[spec.asset_path]["node_count"],
                "textures": summaries_by_path[spec.asset_path]["texture_count"],
                "estimated_peak_particles": summaries_by_path[spec.asset_path][
                    "estimated_peak_particles"
                ],
            }
            for spec in specs
        ],
    }
    return outputs, report, baseline_missing


def publish_atomic(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    if temporary.exists():
        raise GenerationError("refusing stale temporary output: {0}".format(temporary))
    try:
        temporary.write_bytes(payload)
        os.replace(str(temporary), str(path))
    finally:
        if temporary.exists():
            temporary.unlink()


def compare_outputs(root: Path, outputs: Mapping[Path, bytes]) -> None:
    missing: list[str] = []
    changed: list[str] = []
    for relative, expected in sorted(outputs.items(), key=lambda item: item[0].as_posix()):
        path = root / relative
        if not path.is_file():
            missing.append(relative.as_posix())
        elif path.read_bytes() != expected:
            changed.append(relative.as_posix())
    if missing or changed:
        raise GenerationError(
            "generated expansion VFX are stale; missing={0}, changed={1}".format(
                missing, changed
            )
        )


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
        help="compile in temporary quarantine and report intended outputs without writing",
    )
    mode.add_argument(
        "--check",
        action="store_true",
        help="rebuild in temporary quarantine and compare all generator-owned bytes",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    root = arguments.repo_root.resolve()
    if not (root / "Project/Project.heproject").is_file():
        raise GenerationError("not an HEngineDev repository root: {0}".format(root))
    with tempfile.TemporaryDirectory(prefix="he-survivor-expansion-vfx-") as temporary:
        outputs, report, baseline_missing = build_collection(root, Path(temporary))

    if arguments.dry_run:
        mode = "dry-run"
    elif arguments.check:
        compare_outputs(root, outputs)
        mode = "check"
    else:
        for relative, payload in sorted(outputs.items(), key=lambda item: item[0].as_posix()):
            publish_atomic(root / relative, payload)
        if baseline_missing:
            publish_atomic(
                root / PROFILER_BASELINE_PATH,
                json_bytes(default_profiler_baseline()),
            )
        mode = "generate"
    report["mode"] = mode
    report["result"] = "PASS"
    report["effekseer_version"] = EFFEKSEER_VERSION
    print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
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
        raise SystemExit(1)
