---
name: he-effekseer-asset-intake
description: Discover, license-review, safely download, inspect, register, preview, convert, and import Effekseer effects for HEngine. Use for Effekseer asset sourcing, .zip/.efkpkg archive intake, .efk/.efkefc compatibility or GPU-particle checks, provenance and SHA-256 manifests, CC0/CC-BY review, quarantined Project/SourceAssets/EffekseerInbox workflows, or importing effects into Project/Assets/Effects.
---

# HEngine Effekseer Asset Intake

Use a provenance-first quarantine workflow. Treat every downloaded archive as untrusted and every unstated license as unusable.

## Read the applicable references

- Read [references/sources.md](references/sources.md) before discovering or ranking candidates.
- Read [references/license-policy.md](references/license-policy.md) before making any license decision or downloading.
- Read [references/manifest-schema.md](references/manifest-schema.md) before creating or updating `asset.json`.

## Follow the intake workflow

1. Identify a concrete gameplay/VFX gap and the renderer, quality tier, budget category, and Effekseer version constraints. Prefer effects that improve gameplay readability over spectacle.
2. Discover and audit candidates only by default. Do not download unless the user explicitly requests it or the active task already authorizes it.
3. Record the public source page, author, exact license evidence, format, expected dependencies, and the gameplay gap for every candidate. Never infer rights from a filename, search tag, "free," or "royalty free."
4. Apply `references/license-policy.md`. Stop on `REJECT`; keep `MANUAL_REVIEW` candidates out of runtime assets and do not download protected/purchased content automatically.
5. For an authorized, automatically acceptable asset, run a dry run first:

   ```powershell
   python scripts/fetch_effekseer_asset.py --url <direct-download-url> --source <public-source-url> --license CC0-1.0 --author <author> --asset-id <asset-id> --destination Project/SourceAssets/EffekseerInbox --dry-run
   ```

6. Review the printed URL, license, target, and filename. Repeat without `--dry-run` only when they are correct. Keep the original download and generated `asset.json` under `Project/SourceAssets/EffekseerInbox/<asset-id>/`; never download directly into `Project/Assets/Effects`.
7. Inspect without extracting or modifying the original:

   ```powershell
   python scripts/inspect_effekseer_archive.py Project/SourceAssets/EffekseerInbox/<asset-id>/<archive> --output Project/SourceAssets/EffekseerInbox/<asset-id>/inspection.json --pretty
   ```

8. Reject dangerous paths, links, executable/script content, duplicate normalized names, oversized content, or content inconsistent with the source description. Resolve every missing dependency before preview.
9. Treat version and GPU-particle results as evidence-bearing detections. If the script reports `null`, `Unavailable`, or low-confidence heuristics, inspect with the official Effekseer 1.80.6 Editor; do not convert uncertainty into a precise claim.
10. Preview in Effekseer 1.80.6 from a copy. Preserve the original `.efk`/`.efkefc` and archive. Record irreversible format upgrades and all edits. Do not overwrite a 1.7 source without explicit confirmation.
11. Check renderer capability, CPU fallback, coordinate system, orientation, scale, color space, materials, depth/soft particles, distortion/background copy, sound, lifetime/stop behavior, LOD, overdraw, and the HEngine VFX budget. HEngine currently accepts only the DX12 renderer; Vulkan is not implemented, and retired DX11/OpenGL evidence must be revalidated before import. Gameplay-critical effects must have a CPU fallback.
12. Preview in HEngine before packaging. Capture measured profiler data; mark unavailable counters as unavailable and estimates as estimated.
13. Package only the approved copy as `.efkpkg`, import it to a new path under `Project/Assets/Effects`, and preserve the prior effect for comparison/rollback. Never attach gameplay damage, collision, targeting, or hit logic to decorative particles.
14. Save the measured profiler baseline as a JSON object. Update the manifest atomically, assign a budget/quality tier, confirm renderer and CPU fallback behavior, and hash every imported file:

   ```powershell
   python scripts/update_asset_manifest.py --manifest Project/SourceAssets/EffekseerInbox/<asset-id>/asset.json --inspection Project/SourceAssets/EffekseerInbox/<asset-id>/inspection.json --import-file Project/Assets/Effects/<effect>.efkpkg --final-import-path Project/Assets/Effects/<effect>.efkpkg --minimum-effekseer-version 1.80.6 --supported-renderer-api DX12 --has-cpu-fallback true --estimated-peak-particles <count> --budget-category Combat --quality-tier High --profiler-baseline <baseline.json> --modification "Packaged with Effekseer 1.80.6" --dry-run
   ```

15. Review the dry-run JSON, then repeat without `--dry-run`. Report the source, license decision/evidence, SHA-256 values, Effekseer compatibility, GPU/CPU fallback status, budget result, preview result, final path, and modifications.

## Use the scripts safely

- `scripts/fetch_effekseer_asset.py` accepts only an explicit automatically acceptable license, refuses credentials and executable-looking downloads, limits size, verifies the staged file before atomic publication, and writes no files during `--dry-run`. A reviewed custom license also requires `--license-file <complete-terms>`.
- `scripts/inspect_effekseer_archive.py` supports ZIP-compatible archives (including `.efkpkg`) and TAR archives. It never extracts or executes content. A detection of no GPU marker is not proof for opaque binary formats.
- `scripts/update_asset_manifest.py` validates required fields, consumes inspection JSON, hashes imports, and writes through an atomic replacement. Use `--dry-run` before mutation.

Do not bypass login, purchase, captcha, rate limits, or access controls; simulate purchases; execute archive programs/scripts; collect unrelated personal data; or leave partially downloaded content marked complete. For itch.io, BOOTH, and other gated sources, register only the candidate and let the user obtain it.
