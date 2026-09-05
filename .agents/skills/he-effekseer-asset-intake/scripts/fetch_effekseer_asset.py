#!/usr/bin/env python3
"""Download an explicitly licensed Effekseer archive into an isolated inbox."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import tarfile
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


MAX_DOWNLOAD_BYTES = 512 * 1024 * 1024
ASSET_ID_PATTERN = re.compile(r"^[a-z0-9](?:[a-z0-9._-]{0,62}[a-z0-9])?$")
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


@dataclass(frozen=True)
class LicenseDecision:
    decision: str
    commercial_use: bool | None
    attribution_required: bool | None
    modification_allowed: bool | None
    redistribution_restrictions: str | None
    engine_restrictions: str | None
    reason: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Safely fetch an accepted Effekseer archive into Project/SourceAssets/EffekseerInbox."
    )
    parser.add_argument("--url", required=True, help="Direct HTTPS download URL.")
    parser.add_argument("--source", required=True, help="Public landing/source URL or source label.")
    parser.add_argument("--license", dest="license_identifier", help="Exact license identifier.")
    parser.add_argument("--author", required=True, help="Confirmed author or publisher.")
    parser.add_argument("--asset-id", required=True, help="Stable lowercase ID using letters, digits, dot, dash, underscore.")
    parser.add_argument(
        "--destination",
        type=Path,
        default=Path("Project/SourceAssets/EffekseerInbox"),
        help="Inbox root; the asset ID is appended (default: Project/SourceAssets/EffekseerInbox).",
    )
    parser.add_argument(
        "--license-file",
        type=Path,
        help="Complete custom license text; required with CUSTOM-COMMERCIAL-MODIFICATION-ALLOWED.",
    )
    parser.add_argument("--dry-run", action="store_true", help="Print the plan without network or filesystem writes.")
    return parser.parse_args()


def sanitize_public_url(value: str) -> str:
    parts = urllib.parse.urlsplit(value)
    if not parts.scheme:
        return value.strip()
    host = parts.hostname or ""
    if parts.port:
        host = f"{host}:{parts.port}"
    return urllib.parse.urlunsplit((parts.scheme.lower(), host, parts.path, "", ""))


def validate_download_url(value: str) -> urllib.parse.SplitResult:
    parts = urllib.parse.urlsplit(value)
    if parts.scheme.lower() != "https":
        raise ValueError("download URL must use HTTPS")
    if not parts.hostname:
        raise ValueError("download URL must include a hostname")
    if parts.username or parts.password:
        raise ValueError("download URL must not embed credentials")
    return parts


def safe_filename(parts: urllib.parse.SplitResult, asset_id: str) -> str:
    name = Path(urllib.parse.unquote(parts.path)).name.strip().rstrip(". ")
    name = re.sub(r"[<>:\"/\\|?*\x00-\x1f]", "_", name)
    if not name:
        name = f"{asset_id}-download.bin"
    reserved_stem = Path(name).stem.casefold().rstrip(". ")
    if reserved_stem in {
        "con",
        "prn",
        "aux",
        "nul",
        "com1",
        "com2",
        "com3",
        "com4",
        "com5",
        "com6",
        "com7",
        "com8",
        "com9",
        "lpt1",
        "lpt2",
        "lpt3",
        "lpt4",
        "lpt5",
        "lpt6",
        "lpt7",
        "lpt8",
        "lpt9",
    }:
        raise ValueError(f"unsafe Windows filename: {name}")
    if Path(name).suffix.casefold() in EXECUTABLE_EXTENSIONS:
        raise ValueError(f"executable or script download is forbidden: {name}")
    return name


def review_license(identifier: str | None, author: str, license_file: Path | None) -> LicenseDecision:
    if not identifier or not identifier.strip():
        return LicenseDecision("REJECT", None, None, None, None, None, "missing license")
    if not author.strip() or author.strip().casefold() in {"unknown", "unconfirmed", "n/a"}:
        return LicenseDecision("REJECT", None, None, None, None, None, "author/source is not confirmed")

    compact = re.sub(r"[ _]", "-", identifier.strip().upper())
    if any(token in compact for token in ("NONCOMMERCIAL", "NON-COMMERCIAL", "-NC", "PERSONAL-USE")):
        return LicenseDecision("REJECT", False, None, None, None, None, "non-commercial/personal-use restriction")
    if compact in {"CC0", "CC0-1.0", "CC-ZERO", "CREATIVE-COMMONS-ZERO"}:
        return LicenseDecision("ACCEPT", True, False, True, "No additional CC0 restriction", None, "explicit CC0 dedication")
    if compact in {"CC-BY", "CC-BY-3.0", "CC-BY-4.0"}:
        return LicenseDecision(
            "ACCEPT",
            True,
            True,
            True,
            "Preserve author, source, license, and modification attribution",
            None,
            "recognized CC-BY license with confirmed author",
        )
    if compact == "CUSTOM-COMMERCIAL-MODIFICATION-ALLOWED":
        if license_file is None:
            return LicenseDecision(
                "MANUAL_REVIEW",
                None,
                None,
                None,
                None,
                None,
                "custom license requires --license-file with complete reviewed terms",
            )
        if license_file.is_symlink() or not license_file.is_file():
            return LicenseDecision("REJECT", None, None, None, None, None, "custom license file is missing or a link")
        if license_file.stat().st_size <= 0 or license_file.stat().st_size > 1024 * 1024:
            return LicenseDecision("REJECT", None, None, None, None, None, "custom license file size is invalid")
        if license_file.suffix.casefold() not in {"", ".txt", ".md", ".license"}:
            return LicenseDecision("REJECT", None, None, None, None, None, "custom license file must be plain text")
        try:
            terms = license_file.read_bytes()
            terms.decode("utf-8")
        except (OSError, UnicodeDecodeError):
            return LicenseDecision("REJECT", None, None, None, None, None, "custom license file must be readable UTF-8 text")
        if b"\x00" in terms:
            return LicenseDecision("REJECT", None, None, None, None, None, "custom license file contains binary data")
        return LicenseDecision(
            "ACCEPT",
            True,
            None,
            True,
            None,
            None,
            "reviewer declares complete custom terms permit commercial use and modification; retain those terms",
        )
    if "BY-SA" in compact or "ROYALTY-FREE" in compact or "ROYALTYFREE" in compact:
        return LicenseDecision("MANUAL_REVIEW", None, None, None, None, None, "license requires manual term review")
    if any(engine in compact for engine in ("RPG-MAKER-ONLY", "UNITY-ONLY", "UNREAL-ONLY")):
        return LicenseDecision("REJECT", None, None, None, None, identifier, "engine-restricted license")
    return LicenseDecision("MANUAL_REVIEW", None, None, None, None, None, "unrecognized or ambiguous license")


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def build_attribution(asset_id: str, author: str, source: str, identifier: str, required: bool | None) -> str | None:
    if required is not True:
        return None
    return f"{asset_id} by {author}; source: {source}; licensed under {identifier}."


def initial_manifest(
    args: argparse.Namespace,
    decision: LicenseDecision,
    public_download_url: str,
    filename: str | None,
    digest: str | None,
    acquired_at: str | None,
    license_evidence: str | None,
) -> dict[str, object]:
    source = sanitize_public_url(args.source)
    identifier = args.license_identifier or ""
    return {
        "schema_version": 1,
        "asset_id": args.asset_id,
        "source": source,
        "source_url": public_download_url,
        "author": args.author.strip(),
        "acquired_at": acquired_at,
        "sha256": digest,
        "license_identifier": identifier,
        "license_decision": decision.decision,
        "license_evidence": license_evidence,
        "commercial_use": decision.commercial_use,
        "attribution_required": decision.attribution_required,
        "attribution_text": build_attribution(
            args.asset_id, args.author.strip(), source, identifier, decision.attribution_required
        ),
        "modification_allowed": decision.modification_allowed,
        "redistribution_restrictions": decision.redistribution_restrictions,
        "engine_restrictions": decision.engine_restrictions,
        "original_download": filename,
        "minimum_effekseer_version": None,
        "source_effekseer_version": None,
        "uses_gpu_particles": None,
        "supported_renderer_apis": [],
        "has_cpu_fallback": None,
        "estimated_peak_particles": None,
        "vfx_budget_category": None,
        "quality_tier_support": [],
        "profiler_baseline": None,
        "effect_sources": [],
        "textures": [],
        "models": [],
        "materials": [],
        "audio": [],
        "inspection": None,
        "imported_files": [],
        "final_import_path": None,
        "modifications": [],
        "intake_status": "DOWNLOADED_PENDING_INSPECTION" if filename else "PLANNED",
    }


def stream_download(url: str, destination: Path) -> tuple[str, int, str]:
    request = urllib.request.Request(url, headers={"User-Agent": "HEngine-Effekseer-Asset-Intake/1"})
    digest = hashlib.sha256()
    total = 0
    with urllib.request.urlopen(request, timeout=30) as response:  # noqa: S310 - HTTPS validated before and after redirect.
        final_parts = validate_download_url(response.geturl())
        content_type = response.headers.get_content_type().casefold()
        if content_type in {"text/html", "application/xhtml+xml", "application/json"}:
            raise ValueError(f"unexpected response content type: {content_type}")
        declared_length = response.headers.get("Content-Length")
        declared: int | None = None
        if declared_length:
            try:
                declared = int(declared_length)
            except ValueError as error:
                raise ValueError("invalid Content-Length") from error
            if declared < 0 or declared > MAX_DOWNLOAD_BYTES:
                raise ValueError(f"declared download size {declared} is outside the allowed range")
        with destination.open("xb") as stream:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                total += len(chunk)
                if total > MAX_DOWNLOAD_BYTES:
                    raise ValueError(f"download exceeds {MAX_DOWNLOAD_BYTES} bytes")
                digest.update(chunk)
                stream.write(chunk)
            stream.flush()
            os.fsync(stream.fileno())
    if total == 0:
        raise ValueError("downloaded file is empty")
    if declared is not None and total != declared:
        raise ValueError(f"download length {total} does not match declared Content-Length {declared}")
    return digest.hexdigest(), total, sanitize_public_url(urllib.parse.urlunsplit(final_parts))


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_archive(path: Path) -> None:
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path, "r") as archive:
            if not archive.infolist():
                raise ValueError("downloaded ZIP archive is empty")
        return
    if tarfile.is_tarfile(path):
        with tarfile.open(path, "r:*") as archive:
            if not archive.getmembers():
                raise ValueError("downloaded TAR archive is empty")
        return
    raise ValueError("downloaded content is not a recognized ZIP/.efkpkg or TAR archive")


def license_text(args: argparse.Namespace, decision: LicenseDecision, public_url: str) -> str:
    source = sanitize_public_url(args.source)
    attribution = build_attribution(
        args.asset_id,
        args.author.strip(),
        source,
        args.license_identifier or "",
        decision.attribution_required,
    )
    lines = [
        f"Asset ID: {args.asset_id}",
        f"Author: {args.author.strip()}",
        f"Source: {source}",
        f"Download source: {public_url}",
        f"Declared license: {args.license_identifier}",
        f"Intake decision: {decision.decision}",
        f"Decision basis: {decision.reason}",
    ]
    compact = re.sub(r"[ _]", "-", (args.license_identifier or "").strip().upper())
    license_reference = {
        "CC0": "https://creativecommons.org/publicdomain/zero/1.0/legalcode",
        "CC0-1.0": "https://creativecommons.org/publicdomain/zero/1.0/legalcode",
        "CC-BY": "https://creativecommons.org/licenses/by/4.0/legalcode",
        "CC-BY-3.0": "https://creativecommons.org/licenses/by/3.0/legalcode",
        "CC-BY-4.0": "https://creativecommons.org/licenses/by/4.0/legalcode",
    }.get(compact)
    if license_reference:
        lines.append(f"License reference: {license_reference}")
    if attribution:
        lines.append(f"Attribution: {attribution}")
    lines.extend(
        [
            "",
            "Retain the complete license text supplied by the publisher alongside this record when available.",
            "This provenance record does not replace the publisher's license terms.",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    staging: Path | None = None
    try:
        parts = validate_download_url(args.url)
        if not ASSET_ID_PATTERN.fullmatch(args.asset_id):
            raise ValueError("asset ID must be 1-64 lowercase letters/digits with internal dot, dash, or underscore")
        filename = safe_filename(parts, args.asset_id)
        decision = review_license(args.license_identifier, args.author, args.license_file)
        destination = args.destination.resolve()
        asset_root = (destination / args.asset_id).resolve()
        if asset_root.parent != destination:
            raise ValueError("asset destination escapes the inbox root")
        public_url = sanitize_public_url(args.url)
        plan = {
            "action": "dry-run" if args.dry_run else "download",
            "url": public_url,
            "source": sanitize_public_url(args.source),
            "license": args.license_identifier,
            "license_decision": decision.decision,
            "license_reason": decision.reason,
            "target": str(asset_root),
            "filename": filename,
        }
        print(json.dumps(plan, ensure_ascii=False, indent=2))

        if decision.decision != "ACCEPT":
            if args.dry_run:
                return 2
            raise ValueError(f"formal download refused: license decision is {decision.decision} ({decision.reason})")
        if args.dry_run:
            return 0
        if asset_root.exists():
            raise ValueError(f"asset inbox already exists; refusing to overwrite: {asset_root}")

        destination.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix=f".{args.asset_id}-", dir=destination))
        staged_archive = staging / filename
        streamed_digest, size, final_url = stream_download(args.url, staged_archive)
        validate_archive(staged_archive)
        verified_digest = sha256_file(staged_archive)
        if verified_digest != streamed_digest:
            raise ValueError("SHA-256 changed between download and verification")

        acquired_at = utc_now()
        custom_license_name: str | None = None
        if args.license_file is not None:
            custom_license_name = "LICENSE-source" + (args.license_file.suffix or ".txt")
            shutil.copyfile(args.license_file, staging / custom_license_name)
        manifest = initial_manifest(
            args,
            decision,
            final_url,
            filename,
            verified_digest,
            acquired_at,
            custom_license_name or "LICENSE.txt",
        )
        (staging / "LICENSE.txt").write_text(license_text(args, decision, final_url), encoding="utf-8")
        (staging / "asset.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        os.replace(staging, asset_root)
        staging = None
        print(
            json.dumps(
                {
                    "status": "DOWNLOADED_PENDING_INSPECTION",
                    "asset_root": str(asset_root),
                    "archive": filename,
                    "size": size,
                    "sha256": verified_digest,
                },
                ensure_ascii=False,
                indent=2,
            )
        )
        return 0
    except (OSError, ValueError, urllib.error.URLError, zipfile.BadZipFile, tarfile.TarError) as error:
        print(json.dumps({"error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 1
    finally:
        if staging is not None and staging.exists():
            shutil.rmtree(staging, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
