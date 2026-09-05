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
| `supported_renderer_apis` | array | Confirmed HEngine API names, never guessed from effect name. The current importer accepts only `DX12`; retain retired API names only as quarantined historical evidence until the asset is revalidated. |
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
