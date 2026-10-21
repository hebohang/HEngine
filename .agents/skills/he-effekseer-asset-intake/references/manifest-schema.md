# `asset.json` schema

Use UTF-8 JSON with `schema_version` set to `1`. Keep unknown facts as `null`, empty arrays, or explicit `Unavailable`; never invent a value. `fetch_effekseer_asset.py` creates the initial record and `update_asset_manifest.py` validates and updates it atomically.

## Required top-level fields

| Field | Type | Meaning |
| --- | --- | --- |
| `schema_version` | integer | Schema version, currently `1`. |
| `asset_id` | string | Stable lowercase intake ID. |
| `source` | string | Public landing/source page or source label supplied by the reviewer. |
| `source_url` | string | Sanitized direct download URL with credentials, query, and fragment removed. |
| `author` | string | Confirmed creator/publisher. |
| `acquired_at` | string/null | UTC RFC 3339 timestamp after successful download. |
| `sha256` | string/null | SHA-256 of the untouched original archive. |
| `license_identifier` | string | Exact declared identifier. |
| `license_decision` | string | `ACCEPT`, `MANUAL_REVIEW`, or `REJECT`. |
| `license_evidence` | string/null | Saved license/evidence path or stable URL. |
| `commercial_use` | boolean/null | Explicit commercial-use permission. |
| `attribution_required` | boolean/null | Whether attribution is required. |
| `attribution_text` | string/null | Exact required attribution. |
| `modification_allowed` | boolean/null | Explicit modification permission. |
| `redistribution_restrictions` | string/null | Relevant redistribution conditions. |
| `engine_restrictions` | string/null | Engine/platform restrictions. |
| `original_download` | string/null | Filename of the untouched download within the inbox item. |
| `minimum_effekseer_version` | string/null | Minimum confirmed compatible version. |
| `source_effekseer_version` | string/null | Detected or author-declared source version. |
| `uses_gpu_particles` | boolean/null | `null` when detection is inconclusive. |
| `supported_renderer_apis` | array | Confirmed HEngine API names, never guessed from effect name. Current choices are `DX12` and `Vulkan`, each backed by current per-effect runtime evidence; retain retired API names only as quarantined history. GPU development-probe evidence does not publish production GPU capability. |
| `has_cpu_fallback` | boolean/null | Confirmed CPU fallback status. |
| `estimated_peak_particles` | integer/null | Estimate, explicitly identified as such. |
| `vfx_budget_category` | string/null | `GameplayCritical`, `Combat`, `Ambient`, or `UI`. |
| `quality_tier_support` | array | Confirmed subset of `Low`, `Medium`, and `High`. |
| `profiler_baseline` | object/null | Measured configuration and counters; preserve Unavailable/Estimated labels. |
| `effect_sources` | array | `.efk`/`.efkefc` paths found in the archive. |
| `textures` | array | Texture paths. |
| `models` | array | Model paths. |
| `materials` | array | Material paths. |
| `audio` | array | Audio paths. |
| `inspection` | object/null | Safety, dependency, version, and capability summary. |
| `imported_files` | array | Objects with `path`, `sha256`, and byte `size`. |
| `final_import_path` | string/null | Approved path under `Project/Assets/Effects`. |
| `modifications` | array | Timestamped descriptions of conversions/edits. |
| `intake_status` | string | Lifecycle status such as `DOWNLOADED_PENDING_INSPECTION`, `INSPECTED`, or `IMPORTED`. |

## Profiler baseline

Include the build/configuration, renderer API, GPU/driver, resolution, quality tier, sample duration, warm-up policy, and measured P50/P95/P99 values where available. Relevant measurements include CPU Update, CPU Draw Submit, GPU Simulation, GPU Render, total CPU/GPU frame, draw calls, vertices, particle-buffer use/capacity, and peak particles. Mark derived values `Estimated` and inaccessible counters `Unavailable`.

## Integrity rules

- Keep `sha256` tied to the untouched original archive.
- Hash every final imported file independently in `imported_files`.
- Do not set `uses_gpu_particles: false` when an opaque format merely lacks a detectable marker; use `null`.
- Do not set renderer support or CPU fallback without tool/editor/runtime evidence.
- Preserve previous modifications and imported-file records when updating.
- Set `final_import_path` only after HEngine preview, license approval, dependency completion, renderer/GPU checks, and a recorded budget/profiler review. It must match a hashed `imported_files` record.

## Renderer evidence for support claims

Before adding `--supported-renderer-api` or setting `--final-import-path`, supply `profiler_baseline.renderer_evidence`, one object per claimed API:

- `backend`: `DX12` or `Vulkan`; `status`: `PASS` only after reviewing actual runtime/capture evidence for this package.
- `simulation`: `cpu` or `gpu`, matching confirmed `uses_gpu_particles`.
- `execution_scope`: `production` or `development_probe`; for GPU include actual booleans `gpu_enabled` and `gpu_validated`. A successful probe with `gpu_validated=false` must use `development_probe`; it does not prove normal Game/Editor/PACK_GAME default enablement.
- `executable`, `effect`, `report`: each is `{ "path": "local path", "sha256": "64 hex characters" }`. Run from the project root for relative paths. The script verifies all three current files against their hashes; `effect` must be the final hashed package when publishing. `report` points to retained actual runtime evidence, including native exit, renderer identity, captures and stop result. `executable` pins the current tested build.

The validator checks linkage and scope consistency, not whether a manually labelled PASS is truthful. Read the referenced report and its native/capture evidence; never fabricate PASS or infer support from archive inspection or `vfx.describe` alone. Keep exact build/configuration, scene/quality and runtime timestamps in the baseline/report. Revalidate changed package/build hashes. Existing old manifests may be inspected without rewriting their historical support claims.

For parameter changes, consult current `vfx.describe` and `vfx.control` status: speed/scale/seed are distinct from color/intensity support; custom materials can reject global color, and dynamic inputs have no effect without authored equations. A CPU effect is not automatically a CPU fallback for a different GPU effect. Match the gameplay shape and actual Low-quality fallback preview before recording `has_cpu_fallback=true`.

## Update command

The following example publishes a completed import from the repository root; inspection-only updates omit import and support claims. Replace placeholders and example values with measured/confirmed results; `--supported-renderer-api` is repeatable only for APIs independently supported by the retained evidence. Do not set fallback or quality support from this example alone.

```powershell
python .agents/skills/he-effekseer-asset-intake/scripts/update_asset_manifest.py --manifest Project/SourceAssets/EffekseerInbox/<asset-id>/asset.json --inspection Project/SourceAssets/EffekseerInbox/<asset-id>/inspection.json --import-file Project/Assets/Effects/<effect>.efkpkg --final-import-path Project/Assets/Effects/<effect>.efkpkg --minimum-effekseer-version 1.80.6 --supported-renderer-api <DX12-or-Vulkan> --has-cpu-fallback true --estimated-peak-particles <count> --budget-category Combat --quality-tier High --profiler-baseline <baseline.json> --modification "Packaged with Effekseer 1.80.6" --dry-run
```

Review the dry-run JSON before repeating without `--dry-run`. The script consumes the inspection, validates support evidence, hashes imports and writes through an atomic replacement.
