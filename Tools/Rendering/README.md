# Linear HDR comparison tool

`hdr_compare.py` compares pre-tonemap, scene-linear RGB captures without adding a
Runtime or CMake dependency. Its authoritative outputs are a JSON report and a
linear floating-point PFM error image. Pillow is optional and is used only for
mask image loading and the display-only PNG heatmap.

## Basic comparison

```powershell
python Tools/Rendering/hdr_compare.py `
  --reference Artifacts/Cornell/reference.pfm `
  --candidate Artifacts/Cornell/realtime.pfm `
  --output-dir Artifacts/Cornell/comparison
```

The report includes RGB NRMSE, RGB relative L1, Rec.709 linear-luminance error,
and positive-RGB chromaticity error. Divisions by zero are represented as JSON
`null` when the numerator is nonzero; an all-zero error over an all-zero
reference is reported as zero.

## Masks, temporal frames, and gates

Masks may be PFM, raw-float plus sidecar, or an image format supported by the
installed Pillow. Values greater than `--mask-threshold` are selected. Named ROI
and material masks are intersected with the interior mask. When `--edge-mask` is
present, its silhouettes are dilated by two pixels by default. Every named ROI,
material, bleeding, and leak result contains both a `global` scope and an
`interior` scope so near-wall/contour errors are never silently discarded.

```powershell
python Tools/Rendering/hdr_compare.py `
  --reference Artifacts/Cornell/parity.pfm `
  --candidate Artifacts/Cornell/frame_120.pfm `
  --output-dir Artifacts/Cornell/quality `
  --edge-mask Artifacts/Cornell/edge_mask.png `
  --roi-mask red_bleed=Artifacts/Cornell/red_bleed.png `
  --roi-mask green_bleed=Artifacts/Cornell/green_bleed.png `
  --roi-mask occluded=Artifacts/Cornell/occluded.png `
  --material-mask white=Artifacts/Cornell/material_white.png `
  --red-bleeding-roi red_bleed `
  --green-bleeding-roi green_bleed `
  --occluded-roi occluded `
  --temporal-glob "Artifacts/Cornell/static/frame_*.pfm" `
  --temporal-roi white `
  --disocclusion-roi occluded `
  --disocclusion-start 12 `
  --disocclusion-threshold 0.05 `
  --disocclusion-consecutive 3 `
  --max-disocclusion-recovery-frames 8 `
  --max-nrmse 0.10 `
  --max-leak-ratio 0.005 `
  --max-temporal-stddev 0.01
```

Temporal glob results use natural filename order (`frame_2` before `frame_10`)
and frames are processed one at a time; the tool does not retain a full sequence
in memory. If every temporal frame has a distinct truth image, provide
matching `--temporal-reference-frame` arguments or
`--temporal-reference-glob`. Otherwise the main reference is used for every
frame. Temporal stability is the population standard deviation of the
candidate-minus-reference luminance residual, normalized by mean absolute truth
luminance. This removes motion/light changes represented by per-frame truth;
with a fixed truth it measures static accumulation noise. Disocclusion recovery
uses the per-frame luminance-relative-L1 curve. The report distinguishes the
first good frame from the later frame that confirms the requested consecutive
run. `--max-disocclusion-recovery-frames` makes non-recovery a failed gate.
That gate requires an explicit `--disocclusion-roi`; whole-frame fallback is
rejected because unchanged pixels can otherwise hide slow local recovery.

The occluded-leak gate uses positive **ROI-mean** luminance excess divided by
the reference ROI energy. The report also retains per-pixel half-wave positive
excess as a conservative diagnostic, but does not gate on it: independent
unbiased Monte-Carlo images produce a positive half-wave value from noise alone.
For tight limits, use a geometrically fully-occluded ROI and enough independent
samples to bound the remaining ROI-mean uncertainty.

Requested thresholds produce process exit code `3` on failure or unavailable
data. Each threshold check records its exact scope. HDR-FLIP adapter v1 gates are
always global even when `--threshold-scope interior` selects the default scope
for NRMSE, relative L1, and leak metrics. Input/format errors use exit code `2`;
success uses `0`.

`--threshold-mask NAME` applies NRMSE/relative-L1 gates to the named material or
ROI mask (and then to `--threshold-scope`). Cornell uses
`--threshold-mask non_emissive --threshold-scope interior` so the bright area
emitter cannot make a wall/surface-quality gate pass by dominating RMS energy.

## Raw-float sidecar

For `capture.raw`, place metadata at `capture.raw.json` (preferred) or
`capture.json`, or pass the sidecar explicitly for the main reference/candidate.

```json
{
  "width": 1920,
  "height": 1080,
  "channels": 4,
  "dtype": "float16",
  "endianness": "little",
  "layout": "HWC",
  "row_order": "top-down",
  "channel_order": "RGBA",
  "color_space": "scene-linear",
  "color_primaries": "Rec.709/sRGB"
}
```

Supported payloads are tightly packed float16 or float32 HWC data with RGB,
BGR, RGBA, BGRA, or one-channel ordering. PFM uses the original Debevec HDR
variant's left-to-right, bottom-to-top raster order and endianness sign
convention. The scale magnitude is applied to samples on read.

All current metric definitions require scene-linear Rec.709/sRGB RGB. PFM has no
color metadata, so that primaries contract is an explicit CLI default and is
recorded in the report. Convert ACEScg, XYZ, or other source spaces before using
this version of the tool; raw sidecars with other/missing primaries are rejected.

## HDR-FLIP adapter

The repository currently has no approved HDR-FLIP dependency. The tool does not
rename another perceptual metric to HDR-FLIP. Without an adapter report, JSON
contains `available: false` and a reason. An independently approved runner can
provide `--hdr-flip-report` using this narrow interchange format:

The adapter requires content binding and all parameters that materially identify
an HDR-FLIP run:

```json
{
  "schema": "hdr-flip-adapter.v1",
  "mean": 0.025,
  "p95": 0.09,
  "tool": "approved runner name",
  "version": "runner version",
  "reference_sha256": "64 lowercase hex characters",
  "candidate_sha256": "64 lowercase hex characters",
  "reference_decoded_rgb_sha256": "canonical decoded RGB digest",
  "candidate_decoded_rgb_sha256": "canonical decoded RGB digest",
  "width": 1920,
  "height": 1080,
  "color_space": "scene-linear-rgb",
  "color_primaries": "Rec.709/sRGB",
  "scope": "global",
  "parameters": {
    "pixels_per_degree": 67.0,
    "tone_mapper": "ACES",
    "start_exposure": -12.5,
    "stop_exposure": 1.0,
    "exposure_count": 14,
    "exposure_policy": "automatic",
    "p95_method": "nearest-rank-unweighted-pixels"
  }
}
```

Both source-file hashes and canonical decoded scene-linear RGB hashes must match
the current inputs. The latter bind raw sidecar interpretation (dtype,
endianness, row/channel order) as well as pixel values and declared primaries.
Adapter v1 accepts only an unmasked global scope so a stale, masked, or unrelated
report cannot satisfy the current HDR-FLIP gates. This imports already computed
values and provenance; it does not execute or vendor the external implementation.

An adapter value is report-only unless `--hdr-flip-policy` supplies an approved
policy whose tool/version, PPD, tone mapper, exposure policy, and P95 definition
match. A minimal automatic-exposure policy is:

```json
{
  "schema": "hengine.hdr-flip-policy.v1",
  "tool": "approved runner name",
  "version": "runner version",
  "pixels_per_degree": 67.0,
  "tone_mapper": "ACES",
  "exposure_policy": "automatic",
  "p95_method": "nearest-rank-unweighted-pixels"
}
```

A `fixed` exposure policy must additionally declare `start_exposure`,
`stop_exposure`, and `exposure_count`. `--max-hdr-flip-mean` and
`--max-hdr-flip-p95` fail as unavailable without a matching policy.

## Self-test

```powershell
python -m unittest discover -s Tools/Rendering/tests -p "test_*.py" -v
```
