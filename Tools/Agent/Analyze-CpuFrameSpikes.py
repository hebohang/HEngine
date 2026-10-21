"""Join HEngine RenderStats CPU spikes with optional runtime timing log lines.

RuntimeSystemTiming and ApplicationLayerTiming use the simulation frame printed
in RenderStats. RenderWorldExtractTiming uses the render-world frame, which is
one ahead of the simulation frame in RenderSystem3D.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import statistics
from pathlib import Path


LINE_TYPES = {
    "RuntimeSystemTiming": "systems",
    "ApplicationLayerTiming": "layers",
    "RuntimeRenderTiming": "renderSystem",
    "RenderWorldExtractTiming": "extract",
    "ScriptSystemTiming": "scripts",
}
TIMING_LINE = re.compile(r"\[(?P<type>" + "|".join(LINE_TYPES) +
                         r")\] frame=(?P<frame>\d+) (?P<details>.*)")
NUMBER = re.compile(r"(?P<name>[A-Za-z][A-Za-z0-9]*)=(?P<value>-?\d+(?:\.\d+)?)")
SYSTEM = re.compile(r"class HEngine::(?P<name>[A-Za-z0-9_]+)=(?P<value>\d+(?:\.\d+)?)")


def parse_log(path: Path) -> dict[str, dict[int, dict[str, float]]]:
    rows: dict[str, dict[int, dict[str, float]]] = {kind: {} for kind in LINE_TYPES.values()}
    with path.open(encoding="utf-8", errors="replace") as source:
        for line in source:
            match = TIMING_LINE.search(line)
            if not match:
                continue
            kind = LINE_TYPES[match.group("type")]
            details = match.group("details")
            values = {item.group("name"): float(item.group("value"))
                      for item in NUMBER.finditer(details)}
            if kind == "systems":
                values.update({item.group("name"): float(item.group("value"))
                               for item in SYSTEM.finditer(details)})
            rows[kind][int(match.group("frame"))] = values
    return rows


def analyze(stats_path: Path, log_path: Path, threshold: float, extract_offset: int) -> dict:
    log = parse_log(log_path)
    with stats_path.open(newline="", encoding="utf-8-sig") as source:
        reader = csv.DictReader(source)
        required = {"frame", "cpuWallFrameMs", "updateMs", "renderSubmitPresentMs",
                    "gpuFrameSpanMs", "gpuVisibleObjects"}
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise ValueError("RenderStats CSV is missing required frame timing columns")
        frames = list(reader)
    if not frames:
        raise ValueError("RenderStats CSV has no measured frames")

    cpu = [float(row["cpuWallFrameMs"]) for row in frames]
    gpu = [float(row["gpuFrameSpanMs"]) for row in frames]
    slow = []
    for row in frames:
        wall = float(row["cpuWallFrameMs"])
        if wall <= threshold:
            continue
        frame = int(row["frame"])
        item = {
            "frame": frame,
            "cpuWallMs": round(wall, 3),
            "updateMs": round(float(row["updateMs"]), 3),
            "renderSubmitPresentMs": round(float(row["renderSubmitPresentMs"]), 3),
            "gpuFrameSpanMs": round(float(row["gpuFrameSpanMs"]), 3),
            "gpuVisibleObjects": int(row["gpuVisibleObjects"]),
            "sceneTableUploadedBytes": int(row.get("sceneTableUploadedBytes") or 0),
        }
        for kind in ("systems", "layers", "renderSystem", "extract", "scripts"):
            matched_frame = frame + extract_offset if kind == "extract" else frame
            if matched_frame in log[kind]:
                item[kind] = log[kind][matched_frame]
        slow.append(item)

    return {
        "renderStatsPath": str(stats_path),
        "runtimeLogPath": str(log_path),
        "cpuThresholdMs": threshold,
        "renderWorldFrameOffset": extract_offset,
        "measuredFrames": len(frames),
        "cpuMedianMs": round(statistics.median(cpu), 3),
        "cpuP95Ms": round(sorted(cpu)[int((len(cpu) - 1) * 0.95)], 3),
        "cpuMaxMs": round(max(cpu), 3),
        "gpuMaxMs": round(max(gpu), 3),
        "slowCpuFrames": slow,
        "note": "Timing correlation identifies costly phases, not a root cause or an external power event.",
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--render-stats", type=Path, required=True)
    parser.add_argument("--runtime-log", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--threshold-ms", type=float, default=33.333)
    parser.add_argument("--extract-frame-offset", type=int, default=1)
    arguments = parser.parse_args()
    report = analyze(arguments.render_stats, arguments.runtime_log,
                     arguments.threshold_ms, arguments.extract_frame_offset)
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in
                      ("measuredFrames", "cpuMedianMs", "cpuP95Ms", "cpuMaxMs", "gpuMaxMs")},
                     separators=(",", ":")))
    print(f"slowCpuFrames={len(report['slowCpuFrames'])} output={arguments.output}")


if __name__ == "__main__":
    main()
