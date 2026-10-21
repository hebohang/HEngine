"""Summarize the owned progressive PT smoke, without treating it as editor input latency.

Uses the repository's scene-linear PFM reader. The 256 spp image is a finite,
correlated reference, not ground truth. No assertion requires monotonic error.
"""
import argparse
import csv
import json
import math
from pathlib import Path

from hdr_compare import read_pfm


def percentiles(values):
    values = sorted(float(value) for value in values if math.isfinite(float(value)))
    def at(fraction):
        position = (len(values) - 1) * fraction
        lower = int(position)
        return values[lower] + (values[min(lower + 1, len(values) - 1)] - values[lower]) * (position - lower)
    return {"count": len(values), **({f"p{p}": at(p / 100) for p in (50, 95, 99)} if values else {})}


def rows(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def editor_summary(path):
    data = rows(path)
    gpu = {row["gpuSample"]: row for row in data
           if int(row["gpuSample"]) > int(row.get("gpuWarmupCutoffSample", 0))}
    return {"source": str(path), "dimensions": sorted({f'{row["width"]}x{row["height"]}' for row in data}),
            "cpu_wall_ms": percentiles(row["cpuWallFrameMs"] for row in data),
            "frame_slot_wait_ms": percentiles(row["cpuFrameSlotWaitMs"] for row in data),
            "present_call_ms": percentiles(row["cpuPresentMs"] for row in data),
            "gpu_frame_span_ms_unique_delayed_samples": percentiles(row["gpuFrameSpanMs"] for row in gpu.values())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--editor-csv", type=Path, action="append", default=[])
    args = parser.parse_args()
    reference = read_pfm(args.directory / "256-raw.pfm")
    report = {"schema": "hengine.progressive-pt-evidence.v1",
              "limits": ["256 spp reference is finite and shares samples with lower stages; it is not ground truth",
                         "Offscreen CPU timings exclude editor input and Present",
                         "Camera snapshot completion is not proof of OS input-to-display latency",
                         "Editor CSVs are separate runs; unmatched scenes/dimensions are not a before/after comparison"],
              "stages": [], "editor_runs": [editor_summary(path) for path in args.editor_csv]}
    for spp in (1, 4, 16, 64, 256):
        stage = {"spp": spp}
        for kind in ("raw", "denoised"):
            image = read_pfm(args.directory / f"{spp}-{kind}.pfm")
            if (image.width, image.height) != (reference.width, reference.height):
                raise ValueError("Stage and reference dimensions differ")
            stage[kind] = {"rgb_mean": sum(image.values) / len(image.values),
                           "rmse_vs_finite_256_raw": math.sqrt(sum((a-b)**2 for a, b in zip(image.values, reference.values)) / len(image.values)),
                           "width": image.width, "height": image.height}
        report["stages"].append(stage)
    frames = rows(args.directory / "frames.csv")
    unique_gpu = {row["gpuSample"]: row for row in frames if int(row["gpuSample"]) > 0}
    elapsed = sum(float(row["cpuWallMs"]) for row in frames) / 1000
    report["offscreen"] = {
        "cpu_frame_ms": percentiles(row["cpuWallMs"] for row in frames),
        "gpu_span_ms": percentiles(row["gpuSpanMs"] for row in unique_gpu.values()),
        "trace_ms": percentiles(row["traceMs"] for row in unique_gpu.values()),
        "max_in_flight": max(int(row["inFlight"]) for row in frames),
        "accumulation_and_denoise_elapsed_seconds_excluding_exports": elapsed,
        "completed_spp_per_second_including_denoise_wait_excluding_exports": 256 / elapsed,
        "denoise_cpu_ms_max": max(float(row["denoiseMs"]) for row in frames)}
    motion = rows(args.directory / "motion.csv")
    report["synthetic_camera"] = {
        "snapshot_to_completed_preview_ms": percentiles(row["firstCompletedPreviewMs"] for row in motion),
        "preview_dimensions": sorted({f'{row["width"]}x{row["height"]}' for row in motion}),
        "max_in_flight": max(int(row["inFlight"]) for row in motion),
        "retired_stale_results": max(int(row["staleResults"]) for row in motion)}
    output = args.directory / "summary.json"
    output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(output)


if __name__ == "__main__":
    main()
