#!/usr/bin/env python3
"""Validate and atomically update an Effekseer intake asset.json manifest."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import re
import sys
import tempfile
import urllib.parse
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any


REQUIRED_FIELDS = {
    "schema_version",
    "asset_id",
    "source",
    "source_url",
    "author",
    "acquired_at",
    "sha256",
    "license_identifier",
    "license_decision",
    "license_evidence",
    "commercial_use",
    "attribution_required",
    "attribution_text",
    "modification_allowed",
    "redistribution_restrictions",
    "engine_restrictions",
    "original_download",
    "minimum_effekseer_version",
    "source_effekseer_version",
    "uses_gpu_particles",
    "supported_renderer_apis",
    "has_cpu_fallback",
    "estimated_peak_particles",
    "vfx_budget_category",
    "quality_tier_support",
    "profiler_baseline",
    "effect_sources",
    "textures",
    "models",
    "materials",
    "audio",
    "inspection",
    "imported_files",
    "final_import_path",
    "modifications",
    "intake_status",
}
ASSET_ID_PATTERN = re.compile(r"^[a-z0-9](?:[a-z0-9._-]{0,62}[a-z0-9])?$")
SEMANTIC_VERSION_PATTERN = re.compile(r"^\d+\.\d+(?:\.\d+)?$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
HISTORICAL_RENDERER_APIS = ("DX11", "DX12", "Vulkan", "OpenGL", "Metal")
SUPPORTED_RENDERER_APIS = ("DX12", "Vulkan")
QUALITY_TIERS = ("Low", "Medium", "High")
BUDGET_CATEGORIES = ("GameplayCritical", "Combat", "Ambient", "UI")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Update and validate an Effekseer asset.json manifest atomically.")
    parser.add_argument("--manifest", required=True, type=Path, help="Existing asset.json path.")
    parser.add_argument("--inspection", type=Path, help="JSON emitted by inspect_effekseer_archive.py.")
    parser.add_argument("--import-file", action="append", default=[], type=Path, help="Imported file to hash; repeatable.")
    parser.add_argument("--final-import-path", help="Approved path under Project/Assets/Effects.")
    parser.add_argument("--modification", action="append", default=[], help="Conversion/edit note; repeatable.")
    parser.add_argument("--minimum-effekseer-version")
    parser.add_argument("--source-effekseer-version")
    parser.add_argument(
        "--uses-gpu-particles",
        choices=("true", "false", "unknown"),
        help="Confirmed GPU-particle usage from Editor or Runtime inspection.",
    )
    parser.add_argument("--supported-renderer-api", action="append", choices=SUPPORTED_RENDERER_APIS, default=[])
    parser.add_argument("--has-cpu-fallback", choices=("true", "false", "unknown"))
    parser.add_argument("--estimated-peak-particles", type=int)
    parser.add_argument("--budget-category", choices=BUDGET_CATEGORIES)
    parser.add_argument("--quality-tier", action="append", choices=QUALITY_TIERS, default=[])
    parser.add_argument("--profiler-baseline", type=Path, help="JSON object containing measured profiler evidence.")
    parser.add_argument("--dry-run", action="store_true", help="Print the updated manifest without writing it.")
    return parser.parse_args()


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_manifest(manifest: dict[str, Any]) -> None:
    missing = sorted(REQUIRED_FIELDS - manifest.keys())
    if missing:
        raise ValueError(f"manifest is missing required fields: {', '.join(missing)}")
    if manifest["schema_version"] != 1:
        raise ValueError("unsupported schema_version; expected 1")
    if not isinstance(manifest["asset_id"], str) or not ASSET_ID_PATTERN.fullmatch(manifest["asset_id"]):
        raise ValueError("invalid asset_id")
    for field in ("source", "source_url", "author", "license_identifier", "license_decision", "intake_status"):
        if not isinstance(manifest[field], str) or not manifest[field].strip():
            raise ValueError(f"{field} must be a non-empty string")
    if manifest["license_decision"] not in {"ACCEPT", "MANUAL_REVIEW", "REJECT"}:
        raise ValueError("invalid license_decision")
    if manifest["attribution_required"] is True and (
        not isinstance(manifest["attribution_text"], str) or not manifest["attribution_text"].strip()
    ):
        raise ValueError("attribution_text is required when attribution_required is true")
    if manifest["sha256"] is not None and (
        not isinstance(manifest["sha256"], str) or not SHA256_PATTERN.fullmatch(manifest["sha256"])
    ):
        raise ValueError("sha256 must be a lowercase SHA-256 hex digest or null")
    for field in ("commercial_use", "attribution_required", "modification_allowed", "uses_gpu_particles", "has_cpu_fallback"):
        if manifest[field] is not None and not isinstance(manifest[field], bool):
            raise ValueError(f"{field} must be boolean or null")
    for field in (
        "supported_renderer_apis",
        "quality_tier_support",
        "effect_sources",
        "textures",
        "models",
        "materials",
        "audio",
        "imported_files",
        "modifications",
    ):
        if not isinstance(manifest[field], list):
            raise ValueError(f"{field} must be an array")
    if any(api not in HISTORICAL_RENDERER_APIS for api in manifest["supported_renderer_apis"]):
        raise ValueError("supported_renderer_apis contains an unknown historical value")
    if any(tier not in QUALITY_TIERS for tier in manifest["quality_tier_support"]):
        raise ValueError("quality_tier_support contains an unsupported value")
    if manifest["vfx_budget_category"] is not None and manifest["vfx_budget_category"] not in BUDGET_CATEGORIES:
        raise ValueError("invalid vfx_budget_category")
    if manifest["estimated_peak_particles"] is not None and (
        not isinstance(manifest["estimated_peak_particles"], int) or manifest["estimated_peak_particles"] < 0
    ):
        raise ValueError("estimated_peak_particles must be a non-negative integer or null")
    for field in ("minimum_effekseer_version", "source_effekseer_version"):
        value = manifest[field]
        if value is not None and (not isinstance(value, str) or not SEMANTIC_VERSION_PATTERN.fullmatch(value)):
            raise ValueError(f"{field} must be a semantic version like 1.80.6 or null")
    if manifest["profiler_baseline"] is not None and not isinstance(manifest["profiler_baseline"], dict):
        raise ValueError("profiler_baseline must be an object or null")
    if manifest["inspection"] is not None and not isinstance(manifest["inspection"], dict):
        raise ValueError("inspection must be an object or null")


def project_effects_path(value: str) -> bool:
    normalized = value.replace("\\", "/")
    path = PurePosixPath(normalized)
    folded = [part.casefold() for part in path.parts]
    target = ["project", "assets", "effects"]
    return any(folded[index : index + 3] == target for index in range(max(0, len(folded) - 2)))


def inspection_summary(path: Path, report: dict[str, Any], manifest: dict[str, Any]) -> dict[str, Any]:
    if report.get("schema_version") != 1:
        raise ValueError("unsupported inspection schema")
    archive = report.get("archive")
    safety = report.get("safety")
    if not isinstance(archive, dict) or not isinstance(safety, dict):
        raise ValueError("inspection is missing archive or safety data")
    inspected_hash = archive.get("sha256")
    if manifest["sha256"] and inspected_hash != manifest["sha256"]:
        raise ValueError("inspection archive SHA-256 does not match asset.json")
    for field in ("effect_sources", "textures", "models", "materials", "audio", "missing_dependencies"):
        if not isinstance(report.get(field), list):
            raise ValueError(f"inspection field {field} must be an array")
    return {
        "report_path": str(path),
        "inspected_at": utc_now(),
        "archive_sha256": inspected_hash,
        "safe": safety.get("safe") is True,
        "main_effect": report.get("main_effect"),
        "missing_dependencies": report["missing_dependencies"],
        "effect_version_detection": report.get("effect_version_detection"),
        "gpu_particles_detection": report.get("gpu_particles_detection"),
        "features": report.get("features"),
        "effect_read_errors": report.get("effect_read_errors", []),
    }


def merge_imported_files(existing: list[Any], import_paths: list[Path]) -> list[dict[str, Any]]:
    records: dict[str, dict[str, Any]] = {}
    for item in existing:
        if not isinstance(item, dict) or not isinstance(item.get("path"), str):
            raise ValueError("each imported_files item must contain a string path")
        records[item["path"].replace("\\", "/").casefold()] = item
    for path in import_paths:
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"import file does not exist or is not a file: {path}")
        display_path = str(path).replace("\\", "/")
        if not project_effects_path(display_path):
            raise ValueError(f"import file must be under Project/Assets/Effects: {path}")
        records[display_path.casefold()] = {
            "path": display_path,
            "sha256": sha256_file(path),
            "size": path.stat().st_size,
        }
    return sorted(records.values(), key=lambda item: item["path"].casefold())


def validate_renderer_evidence(manifest: dict[str, Any], final_path: str | None = None) -> None:
    baseline = manifest.get("profiler_baseline") or {}
    evidence = baseline.get("renderer_evidence", [])
    if not isinstance(evidence, list):
        raise ValueError("profiler_baseline.renderer_evidence must be an array")
    for api in manifest["supported_renderer_apis"]:
        if api not in SUPPORTED_RENDERER_APIS:
            raise ValueError("historical backend cannot be newly claimed or imported: " + api)
        matches = [item for item in evidence if isinstance(item, dict) and item.get("backend") == api]
        if len(matches) != 1 or matches[0].get("status") != "PASS":
            raise ValueError("one actual PASS runtime evidence record required for " + api)
        item = matches[0]
        expected = "gpu" if manifest.get("uses_gpu_particles") is True else "cpu"
        if manifest.get("uses_gpu_particles") is None or item.get("simulation") != expected:
            raise ValueError("runtime evidence must match confirmed CPU/GPU effect usage")
        if item.get("execution_scope") not in ("production", "development_probe"):
            raise ValueError("runtime evidence requires an explicit execution scope")
        if expected == "gpu" and (item.get("gpu_enabled") is not True or
                not isinstance(item.get("gpu_validated"), bool) or
                (item["execution_scope"] == "production" and item["gpu_validated"] is not True)):
            raise ValueError("GPU probe evidence cannot claim validated production support")
        for field in ("executable", "effect", "report"):
            reference = item.get(field, {})
            if not isinstance(reference, dict) or not isinstance(reference.get("path"), str):
                raise ValueError("runtime evidence missing file reference: " + field)
            path = Path(reference["path"])
            claimed = reference.get("sha256", "")
            if not isinstance(claimed, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", claimed) or not path.is_file() or sha256_file(path).lower() != claimed.lower():
                raise ValueError("runtime evidence current file/hash mismatch: " + field)
        if final_path and Path(item["effect"]["path"]).resolve() != Path(final_path).resolve():
            raise ValueError("runtime evidence must cover the actual final package")


def apply_updates(manifest: dict[str, Any], args: argparse.Namespace) -> dict[str, Any]:
    updated = copy.deepcopy(manifest)
    if args.inspection:
        report = load_json(args.inspection)
        if not isinstance(report, dict):
            raise ValueError("inspection JSON root must be an object")
        updated["inspection"] = inspection_summary(args.inspection, report, updated)
        for field in ("effect_sources", "textures", "models", "materials", "audio"):
            updated[field] = report[field]
        version_detection = report.get("effect_version_detection")
        if isinstance(version_detection, dict) and version_detection.get("status") == "Detected":
            detected = version_detection.get("value")
            if isinstance(detected, str) and SEMANTIC_VERSION_PATTERN.fullmatch(detected):
                updated["source_effekseer_version"] = detected
        gpu_detection = report.get("gpu_particles_detection")
        if isinstance(gpu_detection, dict) and gpu_detection.get("status") == "Detected":
            detected_gpu = gpu_detection.get("value")
            if isinstance(detected_gpu, bool):
                updated["uses_gpu_particles"] = detected_gpu
        blocked = updated["inspection"]["safe"] is not True or bool(updated["inspection"]["missing_dependencies"])
        updated["intake_status"] = "INSPECTION_BLOCKED" if blocked else "INSPECTED"

    if args.minimum_effekseer_version:
        if not SEMANTIC_VERSION_PATTERN.fullmatch(args.minimum_effekseer_version):
            raise ValueError("minimum Effekseer version must look like 1.80.6")
        updated["minimum_effekseer_version"] = args.minimum_effekseer_version
    if args.source_effekseer_version:
        if not SEMANTIC_VERSION_PATTERN.fullmatch(args.source_effekseer_version):
            raise ValueError("source Effekseer version must look like 1.80.6")
        updated["source_effekseer_version"] = args.source_effekseer_version
    if args.uses_gpu_particles:
        updated["uses_gpu_particles"] = {
            "true": True,
            "false": False,
            "unknown": None,
        }[args.uses_gpu_particles]
    if args.supported_renderer_api:
        # Renderer evidence is replaced, not merged: once an asset is
        # revalidated on selected current backends, retired API claims must
        # not remain in an importable manifest.
        updated["supported_renderer_apis"] = sorted(set(args.supported_renderer_api), key=str.casefold)
    if args.has_cpu_fallback:
        updated["has_cpu_fallback"] = {"true": True, "false": False, "unknown": None}[args.has_cpu_fallback]
    if args.estimated_peak_particles is not None:
        if args.estimated_peak_particles < 0:
            raise ValueError("estimated peak particles must be non-negative")
        updated["estimated_peak_particles"] = args.estimated_peak_particles
    if args.budget_category:
        updated["vfx_budget_category"] = args.budget_category
    if args.quality_tier:
        updated["quality_tier_support"] = sorted(
            set(updated["quality_tier_support"]) | set(args.quality_tier),
            key=lambda tier: QUALITY_TIERS.index(tier),
        )
    if args.profiler_baseline:
        baseline = load_json(args.profiler_baseline)
        if not isinstance(baseline, dict):
            raise ValueError("profiler baseline JSON root must be an object")
        updated["profiler_baseline"] = baseline

    updated["imported_files"] = merge_imported_files(updated["imported_files"], args.import_file)
    for note in args.modification:
        if not note.strip():
            raise ValueError("modification notes must not be empty")
        updated["modifications"].append({"recorded_at": utc_now(), "description": note.strip()})

    if args.final_import_path:
        if not project_effects_path(args.final_import_path):
            raise ValueError("final import path must be under Project/Assets/Effects")
        if updated["license_decision"] != "ACCEPT":
            raise ValueError("cannot import an asset whose license decision is not ACCEPT")
        if updated["commercial_use"] is not True:
            raise ValueError("cannot import without explicit commercial-use permission")
        if updated["modification_allowed"] is not True:
            raise ValueError("cannot import without explicit modification permission")
        if not updated["license_evidence"]:
            raise ValueError("cannot import without saved license evidence")
        evidence = str(updated["license_evidence"])
        evidence_url = urllib.parse.urlsplit(evidence)
        if evidence_url.scheme:
            if evidence_url.scheme.casefold() != "https" or not evidence_url.hostname:
                raise ValueError("license evidence URL must be public HTTPS")
        else:
            evidence_candidate = args.manifest.parent / evidence
            evidence_path = evidence_candidate.resolve()
            manifest_root = args.manifest.parent.resolve()
            if os.path.commonpath((str(manifest_root), str(evidence_path))) != str(manifest_root):
                raise ValueError("license evidence path escapes the asset inbox")
            if evidence_candidate.is_symlink() or not evidence_path.is_file():
                raise ValueError("saved license evidence file is missing or a link")
        inspection = updated.get("inspection")
        if not isinstance(inspection, dict) or inspection.get("safe") is not True:
            raise ValueError("cannot import before a safe archive inspection")
        if inspection.get("missing_dependencies"):
            raise ValueError("cannot import while dependencies are missing")
        if not updated["imported_files"]:
            raise ValueError("cannot set final import path without hashing at least one --import-file")
        normalized_final = args.final_import_path.replace("\\", "/").casefold()
        if normalized_final not in {
            item["path"].replace("\\", "/").casefold() for item in updated["imported_files"]
        }:
            raise ValueError("final import path must identify one of the hashed --import-file records")
        if updated["uses_gpu_particles"] is None:
            raise ValueError("cannot import until GPU-particle usage is confirmed")
        if not updated["supported_renderer_apis"]:
            raise ValueError("cannot import until renderer API support is confirmed")
        if any(api not in SUPPORTED_RENDERER_APIS for api in updated["supported_renderer_apis"]):
            raise ValueError("imports require currently evidenced DX12 and/or Vulkan support")
        if updated["vfx_budget_category"] is None:
            raise ValueError("cannot import until the VFX budget category is assigned")
        if updated["estimated_peak_particles"] is None:
            raise ValueError("cannot import until peak particles are estimated")
        if not updated["quality_tier_support"]:
            raise ValueError("cannot import until quality-tier support is recorded")
        if updated["profiler_baseline"] is None:
            raise ValueError("cannot import until a profiler baseline is recorded")
        if updated["uses_gpu_particles"] is True:
            if updated["vfx_budget_category"] == "GameplayCritical" and updated["has_cpu_fallback"] is not True:
                raise ValueError("GameplayCritical GPU-particle effects require a confirmed CPU fallback")
        updated["final_import_path"] = args.final_import_path.replace("\\", "/")
        updated["intake_status"] = "IMPORTED"

    if args.supported_renderer_api or args.final_import_path:
        validate_renderer_evidence(updated, args.final_import_path or updated.get("final_import_path"))
    validate_manifest(updated)
    return updated


def write_atomic(path: Path, payload: str) -> None:
    resolved = path.resolve()
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{resolved.name}.", suffix=".tmp", dir=resolved.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(payload)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_name, resolved)
    except Exception:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def main() -> int:
    args = parse_args()
    try:
        if not args.manifest.is_file():
            raise ValueError(f"manifest does not exist or is not a file: {args.manifest}")
        manifest = load_json(args.manifest)
        if not isinstance(manifest, dict):
            raise ValueError("manifest JSON root must be an object")
        validate_manifest(manifest)
        updated = apply_updates(manifest, args)
        payload = json.dumps(updated, ensure_ascii=False, indent=2, sort_keys=True)
        if not args.dry_run:
            write_atomic(args.manifest, payload)
        print(payload)
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(json.dumps({"error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
