"""Offline pipeline A/B evidence analysis; never runs the engine or judges image quality."""
import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path


PAIRS = (("guides", "guides-full", "guides-compact"),
         ("guides-sponza", "guides-sponza-full", "guides-sponza-compact"),
         ("packing", "packing-off", "packing-on"))
EXPECTED = ("cpuWallFrameMs", "cpuRenderOtherMs", "cpuFramePreparationMs",
            "cpuMaterialPackingMs", "materialPackingCalls", "materialPackingCacheHits",
            "materialPackingCacheEnabled", "sceneTableUploadedBytes",
            "indirectCompactionUploadedBytes", "gpuShadowUploadedBytes")


def number(value):
    try:
        result = float(value)
        return result if math.isfinite(result) else None
    except (ValueError, TypeError):
        return None


def distribution(values):
    values = sorted(v for v in values if v is not None)
    if not values:
        return {"available": False, "count": 0}
    def percentile(fraction):
        index = (len(values) - 1) * fraction
        low = int(index)
        return values[low] + (values[min(low + 1, len(values) - 1)] - values[low]) * (index - low)
    return {"available": True, "count": len(values), "avg": statistics.mean(values),
            "median": statistics.median(values), "p95": percentile(.95),
            "p99": percentile(.99), "max": values[-1], "min": values[0]}


def read_csv(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        return list(reader), reader.fieldnames or []


def analyze_run(root, record):
    result = {"record": record, "issues": [], "cpu": {}, "gpu": {}, "gpuPasses": {}}
    if record.get("exitCode") != 0:
        result["issues"].append("Run exitCode absent or nonzero; measurements are incomplete evidence")
    path = root / (record["name"] + ".csv")
    result["csv"] = str(path.resolve())
    if not path.is_file():
        result["issues"].append("CSV unavailable")
        result["available"] = False
        return result
    rows, columns = read_csv(path)
    warmup = number(record.get("warmupFrames"))
    stable = [r for r in rows if number(r.get("frame")) is not None and
              (warmup is None or number(r["frame"]) > warmup)]
    result.update(available=bool(stable), csvRows=len(rows), cpuStableRows=len(stable))
    if warmup is None:
        result["issues"].append("warmupFrames absent; CSV rows assumed already stable per engine writer")
    if number(record.get("measureFrames")) != len(stable):
        result["issues"].append("CPU stable row count differs from requested measureFrames")
    gpu_columns = [c for c in columns if c.startswith("gpu") and c.endswith("Ms")]
    excluded = {"frame", "gpuSample", "gpuWarmupCutoffSample"}
    for column in columns:
        if column not in excluded and column not in gpu_columns:
            result["cpu"][column] = distribution(number(r.get(column)) for r in stable)
    result["missingColumns"] = [c for c in EXPECTED if c not in columns]
    for column in EXPECTED:
        result["cpu"].setdefault(column, {"available": False, "count": 0})
    # CPU rows stay intact. Timestamp samples arrive asynchronously and can repeat.
    cutoffs = {number(r.get("gpuWarmupCutoffSample")) for r in stable}
    cutoffs.discard(None)
    cutoff = max(cutoffs) if cutoffs else None
    result["gpuWarmupCutoffSample"] = cutoff
    if len(cutoffs) > 1:
        result["issues"].append("GPU cutoff changes within CSV; maximum used conservatively")
    if cutoff is None:
        result["issues"].append("GPU cutoff unavailable; no stable GPU evidence accepted")
    gpu_rows = {}
    duplicate_conflicts = 0
    for row in stable:
        sample = number(row.get("gpuSample"))
        if cutoff is None or sample is None or sample <= max(0, cutoff):
            continue
        if sample in gpu_rows:
            if any(row.get(c) != gpu_rows[sample].get(c) for c in gpu_columns):
                duplicate_conflicts += 1
            continue
        gpu_rows[sample] = row
    result["gpuUniqueStableSamples"] = len(gpu_rows)
    result["gpuDuplicateValueConflicts"] = duplicate_conflicts
    for column in gpu_columns:
        result["gpu"][column] = distribution(number(r.get(column)) for r in gpu_rows.values())
    pass_path = Path(str(path) + ".passes.csv")
    if pass_path.is_file():
        pass_rows, _ = read_csv(pass_path)
        passes = defaultdict(dict)
        for row in pass_rows:
            sample = number(row.get("gpuSample"))
            if sample in gpu_rows:
                passes[row["passName"]].setdefault(sample, number(row.get("milliseconds")))
        result["gpuPasses"] = {name: distribution(samples.values()) for name, samples in passes.items()}
    else:
        result["issues"].append("GPU pass CSV unavailable")
    enabled = sorted({number(r.get("materialPackingCacheEnabled")) for r in stable} - {None})
    expected_enabled = 0 if str(record.get("disablePackingCache")) == "1" else 1
    result["packingCacheEvidence"] = {"observedValues": enabled, "requestedEnabled": expected_enabled,
                                      "matchesRequest": enabled == [expected_enabled] if enabled else None}
    sizes = sorted({(number(r.get("width")), number(r.get("height"))) for r in stable
                    if number(r.get("width")) is not None and number(r.get("height")) is not None})
    compact_pass = "RasterNormalMotionGuides" in result["gpuPasses"]
    result["guidesNominalBytes"] = {
        "compactPassObserved": compact_pass,
        "requestedFullSurface": record.get("fullSurface"),
        "dimensions": sizes,
        "avoidedBytesPerPixel": 12 if compact_pass else None,
        "avoidedAttachmentBytes": sizes[0][0] * sizes[0][1] * 12 if compact_pass and len(sizes) == 1 else None,
        "interpretation": "Nominal omitted base/emissive attachment storage only; not measured bandwidth, VRAM savings or timing."}
    return result


def compare_images(left, right):
    if not left.is_file() or not right.is_file():
        return {"available": False, "reason": "Missing capture", "reference": str(left), "candidate": str(right)}
    try:
        from PIL import Image, ImageChops, ImageStat
    except ImportError:
        return {"available": False, "reason": "Pillow unavailable"}
    try:
        with Image.open(left) as a, Image.open(right) as b:
            if a.size != b.size:
                return {"available": False, "reason": "Capture dimensions differ"}
            difference = ImageChops.difference(a.convert("RGB"), b.convert("RGB"))
        histogram = difference.histogram()
        total = sum(histogram)
        over4 = sum(histogram[c * 256 + v] for c in range(3) for v in range(5, 256))
        means = ImageStat.Stat(difference).mean
        return {"available": True, "maeRgb": means, "meanAbsoluteChannelDifference": statistics.mean(means),
                "channelFractionOver4": over4 / total, "width": difference.width, "height": difference.height,
                "interpretation": "Display-encoded RGB difference, 0..255; matching inputs require review; no image quality PASS."}
    except (OSError, ValueError) as error:
        return {"available": False, "reason": str(error)}


def repeated_means(runs, scope):
    grouped = defaultdict(list)
    for run in runs:
        for metric, stats in run.get(scope, {}).items():
            if stats.get("available"):
                grouped[metric].append({"repeat": run["record"].get("repeat"), "mean": stats["avg"]})
    return {metric: {"repeats": means, "meanOfRepeatMeans": statistics.mean(v["mean"] for v in means),
                     "repeatMeanMin": min(v["mean"] for v in means),
                     "repeatMeanMax": max(v["mean"] for v in means), "repeatCount": len(means)}
            for metric, means in grouped.items()}


def consistency(records, key):
    values = sorted({str(r[key]) for r in records if r.get(key) is not None})
    complete = all(r.get(key) is not None for r in records) and bool(records)
    return {"values": values, "complete": complete, "consistent": complete and len(values) == 1}


def analyze(root):
    records = json.loads((root / "runs.json").read_text(encoding="utf-8-sig"))
    if isinstance(records, dict):
        records = [records]
    runs = [analyze_run(root, r) for r in records]
    cases = sorted({r["case"] for r in records})
    result = {"interpretation": "Observed measurements only; no automatic speedup claim or image-quality PASS.",
              "sampling": "CPU: every stable frame row. GPU: one row per gpuSample strictly above maximum gpuWarmupCutoffSample. Percentiles: linear interpolation. Repeat means equally weighted.",
              "binaryConsistency": consistency(records, "binarySha256"), "runs": runs,
              "cases": {}, "pairs": []}
    for case in cases:
        selected = [r for r in runs if r["record"]["case"] == case]
        result["cases"][case] = {scope: repeated_means(selected, scope) for scope in ("cpu", "gpu", "gpuPasses")}
        result["cases"][case]["sceneConsistency"] = consistency([r["record"] for r in selected], "sceneSha256")
    for label, baseline, candidate in PAIRS:
        selected = [r for r in records if r["case"] in (baseline, candidate)]
        pair = {"name": label, "baseline": baseline, "candidate": candidate,
                "binaryConsistency": consistency(selected, "binarySha256"),
                "sceneConsistency": consistency(selected, "sceneSha256"),
                "scenePathConsistency": consistency(selected, "scene"),
                "available": bool(selected), "repeats": []}
        if not selected:
            pair["reason"] = "No runs for this pair"
        for repeat in sorted({r.get("repeat", 0) for r in selected}):
            a = [r for r in runs if r["record"]["case"] == baseline and r["record"].get("repeat", 0) == repeat]
            b = [r for r in runs if r["record"]["case"] == candidate and r["record"].get("repeat", 0) == repeat]
            item = {"repeat": repeat, "available": len(a) == len(b) == 1}
            if not item["available"]:
                item["reason"] = "Missing or duplicate repeat member"
            else:
                a, b = a[0], b[0]
                item["measurementsAvailable"] = a["available"] and b["available"]
                item["deltasCandidateMinusBaseline"] = {}
                for scope in ("cpu", "gpu", "gpuPasses"):
                    metrics = {}
                    for metric in a[scope].keys() & b[scope].keys():
                        x, y = a[scope][metric], b[scope][metric]
                        if x.get("available") and y.get("available"):
                            metrics[metric] = {"meanDifference": y["avg"] - x["avg"],
                                               "meanDifferencePercent": 100 * (y["avg"] / x["avg"] - 1) if x["avg"] else None}
                    item["deltasCandidateMinusBaseline"][scope] = metrics
                item["image"] = compare_images(root / (a["record"]["name"] + ".png"), root / (b["record"]["name"] + ".png"))
            pair["repeats"].append(item)
        result["pairs"].append(pair)
    return result


def markdown(summary):
    lines = ["# Pipeline A/B observations", "", "Observed values only; no automatic speedup or image-quality verdict.", "",
             "CPU includes every stable frame; GPU deduplicates timestamp samples after its warmup cutoff. Repeat means are equally weighted.", "",
             "| Case | Repeat | CPU rows | GPU samples | CPU wall avg ms | Prepare avg ms | Pack avg ms | GPU span avg ms | Cache observed |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---|"]
    def average(run, scope, key):
        value = run.get(scope, {}).get(key, {}).get("avg")
        return "unavailable" if value is None else "{:.4f}".format(value)
    for run in summary["runs"]:
        r = run["record"]
        lines.append("| {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
            r["case"], r.get("repeat"), run.get("cpuStableRows", 0), run.get("gpuUniqueStableSamples", 0),
            average(run, "cpu", "cpuWallFrameMs"), average(run, "cpu", "cpuFramePreparationMs"),
            average(run, "cpu", "cpuMaterialPackingMs"), average(run, "gpu", "gpuFrameSpanMs"),
            run.get("packingCacheEvidence", {}).get("observedValues", "unavailable")))
    lines += ["", "| Case | Metric | Mean of repeat means | Repeat mean range |", "|---|---|---:|---|"]
    for case, scopes in summary["cases"].items():
        for scope, metrics in (("cpu", ("cpuWallFrameMs", "cpuFramePreparationMs", "cpuMaterialPackingMs")),
                               ("gpu", ("gpuFrameSpanMs",))):
            for metric in metrics:
                value = scopes[scope].get(metric)
                if value:
                    lines.append("| {} | {} | {:.4f} | {:.4f} .. {:.4f} |".format(case, metric,
                        value["meanOfRepeatMeans"], value["repeatMeanMin"], value["repeatMeanMax"]))
    lines += ["", "| Pair | Repeat | Image MAE (0..255) | Channels >4 |", "|---|---:|---:|---:|"]
    for pair in summary["pairs"]:
        for repeat in pair["repeats"]:
            image = repeat.get("image", {})
            lines.append("| {} | {} | {} | {} |".format(pair["name"], repeat["repeat"],
                "{:.4f}".format(image["meanAbsoluteChannelDifference"]) if image.get("available") else "unavailable",
                "{:.6%}".format(image["channelFractionOver4"]) if image.get("available") else "unavailable"))
    lines += ["", "Binary consistency: {}. Detailed identities, missing evidence, upload metrics, nominal guide bytes and all distributions are in summary.json.".format(summary["binaryConsistency"]["consistent"])]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="Benchmark directory containing runs.json")
    parser.add_argument("--output-directory", type=Path)
    args = parser.parse_args()
    output = args.output_directory or args.directory
    output.mkdir(parents=True, exist_ok=True)
    summary = analyze(args.directory)
    (output / "summary.json").write_text(json.dumps(summary, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    (output / "summary.md").write_text(markdown(summary), encoding="utf-8")
    print("Wrote {} and {}".format(output / "summary.json", output / "summary.md"))


if __name__ == "__main__":
    main()
