#!/usr/bin/env python3
"""Inspect an Effekseer archive without extracting or modifying it."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import posixpath
import re
import stat
import sys
import tarfile
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import BinaryIO, Callable, Iterable


EFFECT_EXTENSIONS = {".efk", ".efkefc", ".efkproj"}
TEXTURE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".dds", ".tga", ".bmp", ".webp"}
MATERIAL_EXTENSIONS = {".efkmat", ".efkmatd", ".mat"}
MODEL_EXTENSIONS = {".efkmodel", ".fbx", ".obj", ".gltf", ".glb"}
AUDIO_EXTENSIONS = {".wav", ".ogg", ".mp3", ".flac"}
DEPENDENCY_EXTENSIONS = TEXTURE_EXTENSIONS | MATERIAL_EXTENSIONS | MODEL_EXTENSIONS | AUDIO_EXTENSIONS
EXECUTABLE_EXTENSIONS = {
    ".app",
    ".bat",
    ".cmd",
    ".com",
    ".cpl",
    ".dll",
    ".exe",
    ".hta",
    ".jar",
    ".js",
    ".lnk",
    ".msi",
    ".ps1",
    ".py",
    ".scr",
    ".sh",
    ".vbe",
    ".vbs",
    ".wsf",
}
KNOWN_SAFE_EXTENSIONS = (
    EFFECT_EXTENSIONS
    | TEXTURE_EXTENSIONS
    | MATERIAL_EXTENSIONS
    | MODEL_EXTENSIONS
    | AUDIO_EXTENSIONS
    | {
        ".efkpkg",
        ".efkcurve",
        ".txt",
        ".md",
        ".json",
        ".xml",
        ".csv",
        ".license",
    }
)
VERSION_PATTERNS = (
    re.compile(r"(?i)effekseer(?:[ _-]*(?:tool|editor))?[ _-]*v(?:ersion)?\s*[:=\"'> ]+v?(\d+\.\d+(?:\.\d+)?)"),
    re.compile(r"(?i)<(?:tool)?version>\s*v?(\d+\.\d+(?:\.\d+)?)\s*</(?:tool)?version>"),
    re.compile(r"(?i)\beffekseer\s+v?(\d+\.\d+(?:\.\d+)?)\b"),
)
TOOL_VERSION_PATTERN = re.compile(
    r"(?is)<(?:[A-Za-z_][\w.-]*:)?ToolVersion\b[^>]*>\s*([^<]{1,64}?)\s*</(?:[A-Za-z_][\w.-]*:)?ToolVersion\s*>"
)
DISTORTION_PATTERN = re.compile(
    r"(?is)<(?:[A-Za-z_][\w.-]*:)?Distortion\b[^>]*>\s*([^<]{1,32}?)\s*</(?:[A-Za-z_][\w.-]*:)?Distortion\s*>"
)
GPU_MARKERS = ("gpuparticle", "gpu_particle", "gpu particle", "gpuemitter", "compute shader")
TRUE_VALUES = {"1", "true", "yes", "on", "enabled"}
FALSE_VALUES = {"0", "false", "no", "off", "disabled"}


@dataclass(frozen=True)
class Entry:
    name: str
    size: int
    compressed_size: int | None
    is_dir: bool
    is_link: bool
    is_special: bool
    encrypted: bool


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Safely inspect ZIP/.efkpkg or TAR Effekseer archives and emit JSON."
    )
    parser.add_argument("archive", type=Path, help="Archive to inspect; it is opened read-only.")
    parser.add_argument("--output", type=Path, help="Optional JSON report path.")
    parser.add_argument("--pretty", action="store_true", help="Indent JSON output.")
    parser.add_argument("--max-archive-bytes", type=int, default=512 * 1024 * 1024)
    parser.add_argument("--max-expanded-bytes", type=int, default=2 * 1024 * 1024 * 1024)
    parser.add_argument("--max-entry-bytes", type=int, default=512 * 1024 * 1024)
    parser.add_argument("--max-entries", type=int, default=20_000)
    parser.add_argument("--max-effect-read-bytes", type=int, default=16 * 1024 * 1024)
    return parser.parse_args()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def normalized_name(name: str) -> tuple[str, list[str]]:
    issues: list[str] = []
    forward = name.replace("\\", "/")
    if "\x00" in forward:
        issues.append("NUL_BYTE")
    if forward.startswith("/") or forward.startswith("//"):
        issues.append("ABSOLUTE_PATH")
    if re.match(r"^[A-Za-z]:", forward):
        issues.append("DRIVE_ABSOLUTE_PATH")
    parts = forward.split("/")
    if ".." in parts:
        issues.append("PARENT_TRAVERSAL")
    normalized = posixpath.normpath(forward)
    if normalized in {"", ".", ".."} or normalized.startswith("../"):
        issues.append("PATH_ESCAPES_ROOT")
    return normalized, issues


def classify_entries(
    entries: list[Entry],
    archive_size: int,
    max_archive_bytes: int,
    max_expanded_bytes: int,
    max_entry_bytes: int,
    max_entries: int,
) -> tuple[dict[str, object], dict[str, str]]:
    dangerous_paths: list[dict[str, object]] = []
    symlinks: list[str] = []
    special_entries: list[str] = []
    encrypted: list[str] = []
    executables: list[str] = []
    suspicious: list[str] = []
    duplicates: list[dict[str, str]] = []
    limit_violations: list[str] = []
    normalized_by_key: dict[str, str] = {}
    normalized_by_original: dict[str, str] = {}
    total_expanded = 0

    if archive_size > max_archive_bytes:
        limit_violations.append(f"archive size {archive_size} exceeds {max_archive_bytes}")
    if len(entries) > max_entries:
        limit_violations.append(f"entry count {len(entries)} exceeds {max_entries}")

    for entry in entries:
        normalized, path_issues = normalized_name(entry.name)
        normalized_by_original[entry.name] = normalized
        if path_issues:
            dangerous_paths.append({"path": entry.name, "issues": path_issues})
        if entry.is_link:
            symlinks.append(entry.name)
        if entry.is_special:
            special_entries.append(entry.name)
        if entry.encrypted:
            encrypted.append(entry.name)
        if entry.is_dir:
            continue

        total_expanded += max(0, entry.size)
        if entry.size > max_entry_bytes:
            limit_violations.append(f"entry {entry.name!r} size {entry.size} exceeds {max_entry_bytes}")
        if entry.compressed_size is not None and entry.compressed_size > 0:
            ratio = entry.size / entry.compressed_size
            if entry.size > 10 * 1024 * 1024 and ratio > 1000:
                limit_violations.append(f"entry {entry.name!r} has suspicious compression ratio {ratio:.1f}")

        key = normalized.casefold()
        prior = normalized_by_key.get(key)
        if prior is not None:
            duplicates.append({"first": prior, "duplicate": entry.name})
        else:
            normalized_by_key[key] = entry.name

        basename = PurePosixPath(normalized).name
        suffix = PurePosixPath(basename).suffix.casefold()
        if suffix in EXECUTABLE_EXTENSIONS:
            executables.append(entry.name)
        elif suffix and suffix not in KNOWN_SAFE_EXTENSIONS:
            suspicious.append(entry.name)
        elif not suffix and not basename.casefold().startswith(("license", "copying", "notice")):
            suspicious.append(entry.name)

    if total_expanded > max_expanded_bytes:
        limit_violations.append(f"expanded size {total_expanded} exceeds {max_expanded_bytes}")

    safety = {
        "safe": not any(
            (
                dangerous_paths,
                symlinks,
                special_entries,
                encrypted,
                executables,
                suspicious,
                duplicates,
                limit_violations,
            )
        ),
        "dangerous_paths": dangerous_paths,
        "symlinks": sorted(symlinks),
        "special_entries": sorted(special_entries),
        "encrypted_entries": sorted(encrypted),
        "executable_or_script_files": sorted(executables),
        "suspicious_extensions": sorted(suspicious),
        "duplicate_normalized_names": duplicates,
        "limit_violations": limit_violations,
        "total_expanded_bytes": total_expanded,
    }
    return safety, normalized_by_original


def effect_texts(data: bytes) -> list[str]:
    texts = [data.decode("utf-8", errors="ignore")]
    if b"\x00" in data:
        texts.append(data.decode("utf-16-le", errors="ignore"))
        texts.append(data.decode("utf-16-be", errors="ignore"))
    ascii_runs = re.findall(rb"[\x20-\x7e]{4,}", data)
    if ascii_runs:
        texts.append("\n".join(run.decode("ascii", errors="ignore") for run in ascii_runs))
    return texts


def textual_effect_text(data: bytes) -> str | None:
    if data.startswith(b"\xef\xbb\xbf"):
        return data.decode("utf-8-sig", errors="replace")
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return data.decode("utf-16", errors="replace")
    lead = data.lstrip()[:1]
    if lead in {b"<", b"{", b"["}:
        return data.decode("utf-8", errors="replace")
    return None


def find_dependencies(texts: Iterable[str]) -> set[str]:
    extension_group = "|".join(re.escape(ext.lstrip(".")) for ext in sorted(DEPENDENCY_EXTENSIONS))
    quoted = re.compile(rf"[\"']([^\"'\r\n]+\.(?:{extension_group}))[\"']", re.IGNORECASE)
    plain = re.compile(rf"([A-Za-z0-9_ .\\/()@+-]{{1,240}}\.(?:{extension_group}))", re.IGNORECASE)
    references: set[str] = set()
    for text in texts:
        for pattern in (quoted, plain):
            for match in pattern.finditer(text):
                candidate = match.group(1).strip().replace("\\", "/")
                candidate = candidate.lstrip("./")
                if candidate:
                    references.add(candidate)
    return references


def detect_version(texts: list[str]) -> dict[str, object]:
    matches: list[str] = []
    for text in texts:
        for pattern in VERSION_PATTERNS:
            matches.extend(pattern.findall(text))
    unique = sorted(set(matches))
    if not unique:
        return {
            "value": None,
            "status": "Unavailable",
            "method": "No unambiguous semantic version marker found",
            "confidence": None,
            "evidence": [],
        }
    if len(unique) > 1:
        return {
            "value": None,
            "status": "Ambiguous",
            "method": "Multiple semantic version markers found; confirm in the official editor",
            "confidence": None,
            "evidence": unique,
        }
    return {
        "value": unique[0],
        "status": "Detected",
        "method": "Semantic version marker in effect payload",
        "confidence": "heuristic",
        "evidence": unique,
    }


def detect_effect_version(texts: list[str], effect_path: str, textual: bool) -> dict[str, object]:
    if not textual:
        return {
            "effect": effect_path,
            "value": None,
            "status": "Unavailable",
            "method": "Opaque/binary effect payload; confirm with the official Effekseer editor",
            "confidence": None,
            "evidence": [],
        }

    tool_versions: set[str] = set()
    for text in texts:
        tool_versions.update(match.strip() for match in TOOL_VERSION_PATTERN.findall(text) if match.strip())
    if len(tool_versions) == 1:
        value = next(iter(tool_versions))
        return {
            "effect": effect_path,
            "value": value,
            "status": "Detected",
            "method": "Exact ToolVersion element in textual effect source",
            "confidence": "exact",
            "evidence": [value],
        }
    if len(tool_versions) > 1:
        evidence = sorted(tool_versions, key=str.casefold)
        return {
            "effect": effect_path,
            "value": None,
            "status": "Ambiguous",
            "method": "Multiple ToolVersion elements found; confirm with the official Effekseer editor",
            "confidence": None,
            "evidence": evidence,
        }

    fallback = detect_version(texts)
    return {"effect": effect_path, **fallback}


def aggregate_effect_versions(detections: list[dict[str, object]]) -> dict[str, object]:
    detected_values = {
        str(detection["value"])
        for detection in detections
        if detection.get("status") == "Detected" and detection.get("value") is not None
    }
    evidence = sorted(
        {
            str(item)
            for detection in detections
            for item in detection.get("evidence", [])
            if item is not None
        },
        key=str.casefold,
    )
    incomplete = any(detection.get("status") != "Detected" for detection in detections)
    if not detected_values:
        return {
            "value": None,
            "status": "Unavailable",
            "method": "No effect source has a confirmed version marker; confirm in the official editor",
            "confidence": None,
            "evidence": evidence,
        }
    if len(detected_values) > 1:
        return {
            "value": None,
            "status": "Ambiguous",
            "method": "Effect sources contain multiple ToolVersion values",
            "confidence": None,
            "evidence": evidence,
        }
    value = next(iter(detected_values))
    if incomplete:
        return {
            "value": None,
            "status": "Partial",
            "method": "A version was detected for only some effect sources; confirm the remainder in the official editor",
            "confidence": None,
            "evidence": evidence,
        }
    return {
        "value": value,
        "status": "Detected",
        "method": "All effect sources report the same ToolVersion",
        "confidence": "exact",
        "evidence": evidence,
    }


def detect_effect_distortion(texts: list[str], effect_path: str, textual: bool) -> dict[str, object]:
    if not textual:
        return {
            "effect": effect_path,
            "value": None,
            "status": "Unavailable",
            "method": "Opaque/binary effect payload; confirm background distortion in the official editor",
            "confidence": None,
            "evidence": [],
        }

    values: set[str] = set()
    for text in texts:
        values.update(match.strip().casefold() for match in DISTORTION_PATTERN.findall(text) if match.strip())
    enabled = sorted(value for value in values if value in TRUE_VALUES)
    disabled = sorted(value for value in values if value in FALSE_VALUES)
    unknown = sorted(values - TRUE_VALUES - FALSE_VALUES)
    if enabled:
        return {
            "effect": effect_path,
            "value": True,
            "status": "Detected",
            "method": "Explicit enabled Distortion element in textual effect source",
            "confidence": "exact",
            "evidence": enabled,
        }
    if unknown:
        return {
            "effect": effect_path,
            "value": None,
            "status": "Unavailable",
            "method": "Unrecognized Distortion element value; confirm in the official Effekseer editor",
            "confidence": None,
            "evidence": unknown,
        }
    if disabled:
        return {
            "effect": effect_path,
            "value": False,
            "status": "Detected",
            "method": "Only explicit disabled Distortion elements found in textual effect source",
            "confidence": "exact",
            "evidence": disabled,
        }
    return {
        "effect": effect_path,
        "value": None,
        "status": "Unavailable",
        "method": "No explicit Distortion element; absence is not treated as proof that distortion is unused",
        "confidence": None,
        "evidence": [],
    }


def aggregate_distortion(detections: list[dict[str, object]]) -> dict[str, object]:
    enabled = [detection["effect"] for detection in detections if detection.get("value") is True]
    if enabled:
        return {
            "value": True,
            "status": "Detected",
            "method": "At least one effect source explicitly enables distortion",
            "confidence": "exact",
            "evidence": enabled,
        }
    if detections and all(detection.get("status") == "Detected" for detection in detections):
        return {
            "value": False,
            "status": "Detected",
            "method": "Every effect source explicitly disables distortion",
            "confidence": "exact",
            "evidence": [],
        }
    return {
        "value": None,
        "status": "Unavailable",
        "method": "Distortion could not be ruled out for every effect source",
        "confidence": None,
        "evidence": [],
    }


def detect_gpu(texts: list[str], all_effects_textual: bool) -> dict[str, object]:
    lowered = "\n".join(texts).casefold()
    evidence = sorted({marker for marker in GPU_MARKERS if marker in lowered})
    if evidence:
        return {
            "value": True,
            "status": "Detected",
            "method": "GPU-particle marker in effect payload",
            "confidence": "heuristic",
            "evidence": evidence,
        }
    return {
        "value": None,
        "status": "Unavailable",
        "method": (
            "No known GPU-particle marker found; marker absence is not treated as proof of CPU-only behavior"
            if all_effects_textual
            else "Opaque/binary effect payload has no detectable GPU-particle marker"
        ),
        "confidence": None,
        "evidence": [],
    }


def resolve_dependency(reference: str, effect_path: str, available: set[str]) -> tuple[str | None, str | None]:
    normalized_reference, issues = normalized_name(reference)
    if issues:
        return None, f"unsafe dependency reference {reference!r}: {','.join(issues)}"
    candidates = [normalized_reference]
    parent = str(PurePosixPath(effect_path).parent)
    if parent not in {"", "."}:
        candidates.insert(0, posixpath.normpath(posixpath.join(parent, normalized_reference)))
    for candidate in candidates:
        if candidate.casefold() in available:
            return candidate, None
    return None, reference


def inspect_open_archive(
    archive_path: Path,
    archive_format: str,
    entries: list[Entry],
    open_entry: Callable[[str], BinaryIO],
    args: argparse.Namespace,
) -> dict[str, object]:
    safety, normalized_by_original = classify_entries(
        entries,
        archive_path.stat().st_size,
        args.max_archive_bytes,
        args.max_expanded_bytes,
        args.max_entry_bytes,
        args.max_entries,
    )
    files = [entry for entry in entries if not entry.is_dir]
    by_extension: dict[str, list[str]] = {
        "effect_sources": [],
        "textures": [],
        "materials": [],
        "models": [],
        "audio": [],
    }
    categories = (
        ("effect_sources", EFFECT_EXTENSIONS),
        ("textures", TEXTURE_EXTENSIONS),
        ("materials", MATERIAL_EXTENSIONS),
        ("models", MODEL_EXTENSIONS),
        ("audio", AUDIO_EXTENSIONS),
    )
    for entry in files:
        suffix = PurePosixPath(normalized_by_original[entry.name]).suffix.casefold()
        for category, extensions in categories:
            if suffix in extensions:
                by_extension[category].append(entry.name)
                break
    for paths in by_extension.values():
        paths.sort(key=str.casefold)

    effects = by_extension["effect_sources"]
    main_candidates = sorted(
        effects,
        key=lambda value: (
            len(PurePosixPath(value.replace("\\", "/")).parts),
            0 if value.casefold().endswith(".efkefc") else 1,
            value.casefold(),
        ),
    )
    main_effect = main_candidates[0] if main_candidates else None

    all_texts: list[str] = []
    all_effects_textual = bool(effects)
    dependency_records: list[dict[str, object]] = []
    read_errors: list[dict[str, str]] = []
    version_detections: list[dict[str, object]] = []
    distortion_detections: list[dict[str, object]] = []
    inspection_warnings: list[str] = []
    available = {
        normalized_by_original[entry.name].casefold()
        for entry in files
        if not normalized_name(entry.name)[1]
    }
    for effect in effects:
        entry = next(item for item in files if item.name == effect)
        if entry.size > args.max_effect_read_bytes:
            all_effects_textual = False
            read_errors.append({"path": effect, "error": "effect exceeds read limit"})
            version_detections.append(detect_effect_version([], effect, False))
            distortion_detections.append(detect_effect_distortion([], effect, False))
            continue
        try:
            with open_entry(effect) as stream:
                data = stream.read(args.max_effect_read_bytes + 1)
            if len(data) > args.max_effect_read_bytes:
                all_effects_textual = False
                read_errors.append({"path": effect, "error": "effect exceeds read limit"})
                version_detections.append(detect_effect_version([], effect, False))
                distortion_detections.append(detect_effect_distortion([], effect, False))
                continue
        except (KeyError, OSError, RuntimeError, tarfile.TarError, zipfile.BadZipFile) as error:
            all_effects_textual = False
            read_errors.append({"path": effect, "error": str(error)})
            version_detections.append(detect_effect_version([], effect, False))
            distortion_detections.append(detect_effect_distortion([], effect, False))
            continue
        texts = effect_texts(data)
        all_texts.extend(texts)
        semantic_text = textual_effect_text(data)
        textual = semantic_text is not None
        semantic_texts = [semantic_text] if semantic_text is not None else []
        all_effects_textual = all_effects_textual and textual
        version_detections.append(detect_effect_version(semantic_texts, effect, textual))
        distortion_detections.append(detect_effect_distortion(semantic_texts, effect, textual))
        if effect.casefold().endswith(".efkproj") and not textual:
            inspection_warnings.append(
                f"{effect}: legacy .efkproj is not recognizable text/XML; dependency, version, and distortion checks are unavailable"
            )
        for reference in sorted(find_dependencies(texts), key=str.casefold):
            resolved, problem = resolve_dependency(reference, normalized_by_original[effect], available)
            dependency_records.append(
                {
                    "effect": effect,
                    "reference": reference,
                    "resolved_path": resolved,
                    "status": "FOUND" if resolved else "MISSING_OR_UNSAFE",
                    "problem": problem,
                }
            )

    missing_dependencies = [record for record in dependency_records if record["status"] != "FOUND"]
    version = aggregate_effect_versions(version_detections)
    gpu = detect_gpu(all_texts, all_effects_textual)
    distortion = aggregate_distortion(distortion_detections)
    lowered = "\n".join(all_texts).casefold()
    if not effects:
        dependency_detection = {
            "status": "Unavailable",
            "method": "No Effekseer effect source was found",
            "confidence": None,
        }
        inspection_warnings.append("No .efk, .efkefc, or legacy .efkproj effect source was found")
    elif read_errors or not all_effects_textual:
        dependency_detection = {
            "status": "Partial",
            "method": "Known dependency extensions were scanned, but one or more effect payloads are opaque or unreadable",
            "confidence": "heuristic",
        }
        inspection_warnings.append(
            "Dependency enumeration is partial; an empty missing_dependencies array does not prove that every opaque dependency exists"
        )
    else:
        dependency_detection = {
            "status": "Heuristic",
            "method": "Fully read textual effects were scanned for known Effekseer dependency extensions",
            "confidence": "heuristic",
        }
        inspection_warnings.append(
            "Text/XML dependency enumeration covers known extensions but still requires official-editor preview before import"
        )

    requires_depth = any(
        marker in lowered for marker in ("softparticle", "soft particle", "depthtexture", "depth texture")
    )
    if not requires_depth:
        requires_depth = None

    return {
        "schema_version": 1,
        "archive": {
            "path": str(archive_path.resolve()),
            "filename": archive_path.name,
            "format": archive_format,
            "size": archive_path.stat().st_size,
            "sha256": sha256_file(archive_path),
            "entry_count": len(entries),
        },
        "safety": safety,
        "contents": [
            {
                "path": entry.name,
                "size": entry.size,
                "compressed_size": entry.compressed_size,
                "is_directory": entry.is_dir,
                "is_link": entry.is_link,
                "is_special": entry.is_special,
            }
            for entry in entries
        ],
        "main_effect": main_effect,
        "main_effect_candidates": main_candidates,
        **by_extension,
        "dependencies": dependency_records,
        "missing_dependencies": missing_dependencies,
        "dependency_detection": dependency_detection,
        "effect_read_errors": read_errors,
        "effect_version_detection": version,
        "effect_version_detections": version_detections,
        "gpu_particles_detection": gpu,
        "distortion_detection": distortion,
        "distortion_detections": distortion_detections,
        "inspection_warnings": inspection_warnings,
        "features": {
            "requires_depth_or_soft_particles": requires_depth,
            "uses_distortion": distortion["value"],
            "contains_audio": bool(by_extension["audio"]),
        },
    }


def inspect_archive(path: Path, args: argparse.Namespace) -> dict[str, object]:
    if not path.is_file():
        raise ValueError(f"archive does not exist or is not a file: {path}")
    if path.stat().st_size > args.max_archive_bytes:
        raise ValueError(f"archive size {path.stat().st_size} exceeds {args.max_archive_bytes}")

    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path, "r") as archive:
            infos = archive.infolist()
            entries = []
            for info in infos:
                mode = (info.external_attr >> 16) & 0xFFFF
                entries.append(
                    Entry(
                        name=info.filename,
                        size=info.file_size,
                        compressed_size=info.compress_size,
                        is_dir=info.is_dir(),
                        is_link=stat.S_ISLNK(mode),
                        is_special=stat.S_IFMT(mode)
                        not in {0, stat.S_IFREG, stat.S_IFDIR, stat.S_IFLNK},
                        encrypted=bool(info.flag_bits & 0x1),
                    )
                )
            return inspect_open_archive(path, "zip", entries, archive.open, args)

    if tarfile.is_tarfile(path):
        with tarfile.open(path, "r:*") as archive:
            members = archive.getmembers()
            entries = [
                Entry(
                    name=member.name,
                    size=member.size,
                    compressed_size=None,
                    is_dir=member.isdir(),
                    is_link=member.issym() or member.islnk(),
                    is_special=not (member.isreg() or member.isdir() or member.issym() or member.islnk()),
                    encrypted=False,
                )
                for member in members
            ]

            def open_tar_entry(name: str) -> BinaryIO:
                stream = archive.extractfile(name)
                if stream is None:
                    raise OSError(f"cannot read archive entry: {name}")
                return stream

            return inspect_open_archive(path, "tar", entries, open_tar_entry, args)

    raise ValueError("unsupported archive format; expected ZIP/.efkpkg or TAR")


def write_json_atomic(path: Path, payload: str, protected_path: Path) -> None:
    resolved = path.resolve()
    if resolved == protected_path.resolve():
        raise ValueError("output path must not overwrite the inspected archive")
    resolved.parent.mkdir(parents=True, exist_ok=True)
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
        report = inspect_archive(args.archive, args)
        payload = json.dumps(report, ensure_ascii=False, indent=2 if args.pretty else None, sort_keys=True)
        if args.output:
            write_json_atomic(args.output, payload, args.archive)
        print(payload)
        return 0 if report["safety"]["safe"] else 2
    except (OSError, ValueError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(json.dumps({"error": str(error), "archive": str(args.archive)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
