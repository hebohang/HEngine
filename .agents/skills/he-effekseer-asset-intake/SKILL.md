---
name: he-effekseer-asset-intake
description: Discover, inspect, or import external Effekseer assets for HEngine, including license and compatibility review.
---

# HEngine Effekseer Asset Intake

Treat downloaded archives as untrusted and unstated licenses as unusable. Keep provenance and originals in quarantine; perform only the stages needed for the requested result.

## Select the completion stage

| Request | Completion boundary |
| --- | --- |
| Discover/rank | Return enough suitable candidates with source and license evidence; use [sources](references/sources.md). Do not download by default. |
| Static inspection | Report archive safety, dependencies, format/version and GPU detections, including unknowns. Stop without launching an editor or engine unless confirmation is needed for the requested result. |
| Preview/convert | Work on a copy in the tool needed to answer the request; deliver preview/conversion evidence without publishing runtime assets. |
| Import | Finish license, dependency, HEngine preview, support and budget checks, then publish the approved package and [manifest](references/manifest-schema.md). |

Read [license-policy](references/license-policy.md) before a license decision or download, and [manifest-schema](references/manifest-schema.md) before writing `asset.json`. Existing task authorization is sufficient; advancing stages must serve the request, not this table alone.

## Discovery, download and static inspection

- Use the requested gameplay purpose and known renderer, quality, budget and version constraints to rank candidates. Record the public page, author, exact license evidence, format and expected dependencies; never infer rights from a filename, tag, "free" or "royalty free."
- Download only when explicitly requested or already authorized by the active task. Stop acquisition/import for `REJECT`; keep `MANUAL_REVIEW` candidates out of runtime assets. Gated/purchased items remain candidate records for the user to obtain.
- Use `scripts/fetch_effekseer_asset.py` with an explicitly acceptable license and `--dry-run`; review the URL, license, target and filename, then repeat without the flag when correct. The script refuses credentials/executable-looking downloads, limits size and verifies staging before atomic publication. Reviewed custom licenses also need `--license-file <complete-terms>`. Commands are in [sources](references/sources.md#download-and-inspection-commands).
- Keep downloads and `asset.json` under `Project/SourceAssets/EffekseerInbox/<asset-id>/`, never directly in `Project/Assets/Effects`. Inspect ZIP/`.efkpkg` or TAR using `scripts/inspect_effekseer_archive.py`; it does not extract or execute content. Preserve the original archive and `.efk`/`.efkefc`.
- Reject dangerous paths, links, executable/script content, duplicate normalized names, oversized content or contents inconsistent with the source. Resolve missing dependencies before preview.
- Detection results are evidence, not guarantees: absent GPU markers do not prove an opaque file is CPU-only. Keep `null`, `Unavailable` and low-confidence results explicit. Use the official Effekseer 1.80.6 Editor only when the task requires confirmation or an unknown blocks the requested preview/import stage.

## Preview and import

- Preview or convert editable copies with official Effekseer 1.80.6 tools as needed. Keep legacy originals even if loading fails; do not patch opaque runtime bytes or assume equal version numbers imply compatibility. Record all edits and irreversible upgrades; overwriting a 1.7 original still requires explicit confirmation.
- Before final import, preview a scratch `.efkpkg` in HEngine using owned `hectl game/editor check`, `capture` or an Agent session stopped in `finally`; never `open`. Check the current configuration/ownership contract before using `Tools/VFX/preview.py`.
- Validate only the selected or affected backends and quality tiers; each claimed support combination needs evidence. Check relevant coordinate/orientation/scale, color/material, depth/soft-particle, distortion/background-copy, sound, lifetime/stop and LOD behavior, plus overdraw and the VFX budget. Gameplay-critical effects require a CPU fallback.
- Query the current candidate with `vfx.describe` / `vfx.stats`, or embedded `HEngine.VFX.Describe()` / `GetStats()` without an Agent mailbox, and inspect per-effect status/support reasons after load/play. Separate CPU support, actual GPU enablement and production validation: `gpu.validated=false` stays unverified after a successful development probe. Backend names alone prove no support; retired DX11/OpenGL evidence is historical.
- Pin executable/backend/package/scene/quality and retain native stdout/stderr, captures and measured profiler data. Mark inaccessible counters `Unavailable` and estimates `Estimated`; follow the [renderer-evidence contract](references/manifest-schema.md#renderer-evidence-for-support-claims).
- Publish only the approved `.efkpkg` to a new path under `Project/Assets/Effects`, preserving prior effects for rollback. Decorative particles must not own gameplay damage, collision, targeting or hit logic.
- Update the manifest atomically with the measured profiler baseline, budget/quality tier, confirmed support/fallback and hashes of every imported file. `scripts/update_asset_manifest.py` validates hash-linked runtime evidence for each new support claim; use its [dry-run example](references/manifest-schema.md#update-command), inspect the JSON, then publish.

Report the completed stage and its applicable source/license evidence, hashes, compatibility, support/fallback, budget, preview and output paths. Unperformed stages stay unverified rather than triggering extra work.

Do not bypass login, payment, captcha, rate limits or access controls; simulate purchases; execute archive programs/scripts; collect unrelated personal data; or mark partial downloads complete.
