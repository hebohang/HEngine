"""Paired SSGI diagnostics. Requires Pillow and NumPy; never treats PNGs as HDR."""
import argparse
import csv
import hashlib
import json
import re
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def statistics(values):
    a = np.asarray(values, dtype=np.float64)
    if not len(a):
        return {"samples": 0}
    if not np.all(np.isfinite(a)):
        raise ValueError("Non-finite statistic input")
    return {"samples": len(a), "average": float(a.mean()), "minimum": float(a.min()),
            "p95": float(np.percentile(a, 95)), "p99": float(np.percentile(a, 99)),
            "maximum": float(a.max())}


def validate_settings(prefix):
    snapshot = read_json(str(prefix) + ".csv.settings.json")
    renderer = snapshot["renderer"]
    if renderer["phase"] != "shutdown":
        raise ValueError("Expected shutdown settings snapshot: " + str(prefix))
    effective = snapshot["effective"]
    if effective["Rendering.Path"] != "Deferred":
        raise ValueError("This comparison requires Deferred")
    input_path = Path(str(prefix) + ".json")
    if input_path.exists():
        for key, expected in read_json(input_path).items():
            actual = effective.get(key, snapshot.get("debug", {}).get(key))
            equal = actual == expected if isinstance(expected, (str, bool)) else (
                actual is not None and abs(float(actual)-float(expected)) <= max(1e-6, abs(float(expected))*1e-5))
            if not equal:
                raise ValueError("Requested/effective mismatch {}: {} != {}".format(key, actual, expected))
    return snapshot


def analyze_run(prefix):
    prefix = Path(prefix)
    snapshot = validate_settings(prefix)
    validation_logs = []
    for suffix in ('.stdout.log', '.stderr.log'):
        path = Path(str(prefix)+suffix)
        if path.exists():
            validation_logs.append(str(path))
            if re.search(r'\[(?:error|critical)\]|Validation Error|NVRHI[^\n]*(?:ERROR|FATAL)',
                         path.read_text(encoding='utf-8-sig', errors='replace'), re.IGNORECASE):
                raise ValueError('Runtime/validation error in ' + str(path))
    with Path(str(prefix)+".csv").open(newline="", encoding="utf-8-sig") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        raise ValueError("No measured CPU frames")
    selected = {}
    for row in rows:
        sample = int(row["gpuSample"])
        if sample > int(row["gpuWarmupCutoffSample"]) and float(row["gpuFrameSpanMs"]) > 0:
            selected.setdefault(sample, row)
        if int(row["width"]) != snapshot["renderer"]["width"] or int(row["height"]) != snapshot["renderer"]["height"]:
            raise ValueError("Render dimensions changed during measurement")
    if not selected:
        raise ValueError("No unique GPU samples remain after warmup")
    passes, sums, seen = {}, {}, set()
    with Path(str(prefix)+".csv.passes.csv").open(newline="", encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            sample, name = int(row["gpuSample"]), row["passName"]
            if sample not in selected or name == "FrameGpuSpan":
                continue
            if (sample, name) in seen:
                raise ValueError("Duplicate GPU pass sample")
            seen.add((sample, name))
            duration = float(row["milliseconds"])
            if duration < 0 or not np.isfinite(duration):
                raise ValueError("Invalid GPU duration")
            passes.setdefault(name, []).append(duration)
            if name.startswith(("SSGI", "SVGF", "GlobalIllumination", "RasterSurfaceGuides", "DepthPyramid",
                                "DiffuseDemodulation", "TemporalReprojection", "HistoryFix", "MomentsAndVariance",
                                "ATrousFilter", "BilateralUpsample", "DiffuseRemodulation", "GIComposite")):
                sums[sample] = sums.get(sample, 0.0) + duration
    image = Path(str(prefix)+".png")
    result = {"prefix": str(prefix), "settings": snapshot,
              "cpuWallMs": statistics([float(r["cpuWallFrameMs"]) for r in rows]),
              "gpuFrameMs": statistics([float(r["gpuFrameSpanMs"]) for r in selected.values()]),
              "gpuSampleIds": sorted(selected), "passesMs": {n: statistics(v) for n, v in sorted(passes.items())},
              "giAndSharedPreparationMs": statistics(list(sums.values())),
              "transientReservedPeakBytes": max(int(r["transientReservedBytes"]) for r in rows),
              "validationLogs": validation_logs,
              "gpuWarmupPolicy": "Unique gpuSample > each row's gpuWarmupCutoffSample; same IDs select pass samples",
              "imageSha256": hashlib.sha256(image.read_bytes()).hexdigest() if image.exists() else None}
    return result


def image_comparison(off_prefix, on_prefix, output, dimensions):
    off_path, on_path = Path(str(off_prefix)+".png"), Path(str(on_prefix)+".png")
    if not off_path.exists() or not on_path.exists():
        return {"status": "NO_CAPTURES"}
    off, on = Image.open(off_path).convert("RGB"), Image.open(on_path).convert("RGB")
    if off.size != on.size:
        raise ValueError("Capture sizes differ")
    if off.size != dimensions:
        raise ValueError("Capture size differs from actual renderer dimensions")
    a, b = np.asarray(off, dtype=np.float32)/255.0, np.asarray(on, dtype=np.float32)/255.0
    la, lb = a @ np.array([.2126,.7152,.0722]), b @ np.array([.2126,.7152,.0722])
    # Erode dark areas to reject antialiased shadow boundaries. Black sky remains
    # included: without a depth capture this is explicitly a darkness proxy.
    dark = la <= 8.0/255.0
    eroded = dark.copy()
    for y in (-1, 0, 1):
        for x in (-1, 0, 1):
            eroded &= np.roll(np.roll(dark, y, axis=0), x, axis=1)
    margin = max(2, int(min(off.size)*.02))
    eroded[:margin] = False; eroded[-margin:] = False
    eroded[:, :margin] = False; eroded[:, -margin:] = False
    delta = lb-la
    gain = np.maximum(b-a, 0)
    direct = la >= .15
    count = int(eroded.sum())
    result = {"status": "MEASURED_POST_TONEMAP_PROXY", "dimensions": list(off.size),
              "darkThresholdSrgbLuma": 8.0/255.0, "brighteningThresholdSrgbLuma": 3.0/255.0,
              "darkPixels": count, "darkPixelsBrightened": int((eroded & (delta >= 3.0/255.0)).sum()),
              "darkFractionBrightened": float((delta[eroded] >= 3.0/255.0).mean()) if count else None,
              "darkLumaOff": float(la[eroded].mean()) if count else None,
              "darkLumaOn": float(lb[eroded].mean()) if count else None,
              "darkLumaDelta": statistics(delta[eroded]),
              "directPixels": int(direct.sum()), "directLumaDelta": statistics(delta[direct]),
              "changedPixelFraction": float(np.any(abs(a-b) > 1.0/255.0, axis=2).mean()),
              "interpretation": "Fixed-exposure, tone-mapped sRGB difference; dark mask is not a geometry/shadow mask and includes black sky. This does not by itself prove GI correctness or absence of leaks."}
    # One compact image per pair, maximum 1280px edge; original captures stay on disk.
    panel_width = 420
    panel_height = max(1, round(off.height*panel_width/off.width))
    sheet = Image.new("RGB", (panel_width*3, panel_height+24), (20,20,20))
    images = [off, on, Image.fromarray(np.uint8(np.clip(gain*4, 0, 1)*255))]
    draw = ImageDraw.Draw(sheet)
    for i, (picture, label) in enumerate(zip(images, ("GI Off", "SSGI", "Positive difference x4"))):
        sheet.paste(picture.resize((panel_width,panel_height), Image.Resampling.LANCZOS), (i*panel_width,24))
        draw.text((i*panel_width+6,5), label, fill="white")
    preview = Path(str(output)+".preview.jpg")
    sheet.save(preview, quality=88)
    result["preview"] = str(preview)
    return result


def compare(off_prefix, on_prefix, output, manifest=None):
    off, on = analyze_run(off_prefix), analyze_run(on_prefix)
    dimensions = tuple(off['settings']['renderer'][key] for key in ('width', 'height'))
    if dimensions != tuple(on['settings']['renderer'][key] for key in ('width', 'height')):
        raise ValueError('Off/On renderer dimensions differ')
    if manifest is not None:
        requested_dimensions = tuple(int(part) for part in manifest['resolution'].split('x'))
        if dimensions != requested_dimensions:
            raise ValueError('Actual dimensions differ from requested resolution')
        for run in (off, on):
            if run['cpuWallMs']['samples'] != manifest['measureFrames']:
                raise ValueError('Measured CPU frame count differs from requested count')
            if run['settings']['effective']['Rendering.Backend'] != manifest['graphicsApi']:
                raise ValueError('Actual graphics backend differs from requested backend')
    left, right = off["settings"]["effective"], on["settings"]["effective"]
    allowed = {"Rendering.GI.Technique"}
    differing = [key for key in set(left)|set(right) if key not in allowed and left.get(key) != right.get(key)]
    if differing:
        raise ValueError("Off/On controls differ: " + ", ".join(sorted(differing)))
    if left["Rendering.GI.Technique"] != "Off" or right["Rendering.GI.Technique"] != "ScreenSpaceGI":
        raise ValueError("Expected Off / ScreenSpaceGI pair")
    result = {"off": off, "on": on, "identicalEffectiveControlsExceptGiTechnique": True,
              "gpuFrameMeanDeltaMs": on["gpuFrameMs"]["average"]-off["gpuFrameMs"]["average"],
              "images": image_comparison(off_prefix, on_prefix, output, dimensions)}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--off", type=Path)
    parser.add_argument("--on", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = {"evidenceType": "runtime-manual", "status": "MEASURED", "pairs": {},
              "interpretation": "Short paired diagnostics; no thermal/sustained-performance or visual-quality PASS inferred. CPU wall includes GPU backpressure; GPU span excludes Present."}
    if args.manifest:
        manifest = read_json(args.manifest)
        report['manifest'] = str(args.manifest)
        report['lightingCase'] = manifest.get('lightingCase', 'AuthoredDirect')
        report['intelCloseView'] = manifest.get('intelCloseView', False)
        report['sceneCases'] = manifest['scenes']
        output = args.output or args.manifest.parent/"report.json"
        groups = {}
        for run in manifest["runs"]:
            if run["status"] != "CAPTURED":
                raise ValueError("Incomplete run: " + run["prefix"])
            groups.setdefault(run["pair"], {})[run["mode"]] = run
        for label, pair in groups.items():
            if pair["off"]["sceneSha256"] != pair["on"]["sceneSha256"]:
                raise ValueError("Paired scene hashes differ")
            if pair["off"]["executableSha256"] != pair["on"]["executableSha256"]:
                raise ValueError("Executable changed inside a pair")
            if pair['off'].get('sourceHashes') != pair['on'].get('sourceHashes'):
                raise ValueError('Shader/renderer source changed inside a pair')
            if manifest["movingCamera"]:
                cameras = [read_json(r["prefix"]+".camera/camera.runtime.json") for r in (pair["off"], pair["on"])]
                if any(c["status"] != "PASS_FIXED_TICK_INPUT" or c["ticks"] != manifest["warmupFrames"]+manifest["measureFrames"] for c in cameras):
                    raise ValueError("Moving camera did not complete exact frame sequence")
                if cameras[0]["poseSequenceSha256"] != cameras[1]["poseSequenceSha256"]:
                    raise ValueError("Paired camera sequences differ")
            report["pairs"][label] = compare(pair["off"]["prefix"], pair["on"]["prefix"], output.parent/label, manifest)
    else:
        if not args.off or not args.on or not args.output:
            parser.error("Use --manifest or --off PREFIX --on PREFIX --output report.json")
        output = args.output
        report["pairs"]["pair"] = compare(args.off, args.on, output.with_suffix(""))
        report["sceneIdentity"] = "Caller must establish same scene/camera; no manifest supplied."
    output.write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    for label, pair in report["pairs"].items():
        print("{}: GPU Off {:.3f} ms, On {:.3f} ms, delta {:+.3f} ms; image {}".format(
            label, pair["off"]["gpuFrameMs"]["average"], pair["on"]["gpuFrameMs"]["average"],
            pair["gpuFrameMeanDeltaMs"], pair["images"]["status"]))
    print("SSGI report: " + str(output))


if __name__ == "__main__":
    main()
