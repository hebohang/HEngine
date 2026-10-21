"""Summarize the original PBRTest PT fixture; never treat offscreen FPS as input latency."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path

from hdr_compare import read_pfm
from summarize_progressive_pt import editor_summary, percentiles


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--scene", type=Path, default=Path("Project/Assets/Scenes/PBRTest.he"))
    parser.add_argument("--editor-csv", type=Path)
    parser.add_argument("--preview", action="store_true", help="Write an EV14 ACES/sRGB contact sheet (requires Pillow)")
    args = parser.parse_args()
    reference = read_pfm(args.directory / "256-raw.pfm")
    report = {"schema": "hengine.pbrtest-pt-evidence.v1",
              "scene_sha256": hashlib.sha256(args.scene.read_bytes()).hexdigest(),
              "dimensions": [reference.width, reference.height], "stages": [],
              "limits": ["256 spp is a finite correlated reference, not ground truth",
                         "Offscreen CPU timings exclude OS input, editor panels and Present",
                         "Denoising may introduce bias or remove detail; lower RMSE is not guaranteed",
                         "No matched pre-change PT timing exists: the original scene was rejected"]}
    for spp in (1, 4, 16, 64, 256):
        stage = {"spp": spp}
        for kind in ("raw", "denoised"):
            path = args.directory / f"{spp}-{kind}.pfm"
            image = read_pfm(path)
            if (image.width, image.height) != (reference.width, reference.height):
                raise ValueError("Stage dimensions differ")
            stage[kind] = {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                           "mean_rgb": sum(image.values) / len(image.values),
                           "rmse_vs_finite_256_raw": math.sqrt(sum((a-b)**2 for a, b in
                               zip(image.values, reference.values)) / len(image.values))}
        report["stages"].append(stage)
    with (args.directory / "frames.csv").open(newline="") as stream:
        all_frames = list(csv.DictReader(stream))
    # Only the initial 1 -> 256 progression; later rows exercise mutations/restarts.
    frames = []
    for row in all_frames:
        frames.append(row)
        if int(row["stage"]) == 256 and int(row["minSpp"]) == 256 and int(row["denoisedSpp"]) == 256:
            break
    elapsed = sum(float(row["cpuMs"]) for row in frames) / 1000
    report["offscreen"] = {"cpu_frame_ms": percentiles(row["cpuMs"] for row in frames),
        "elapsed_seconds_excluding_exports": elapsed,
        "completed_spp_per_second_including_denoise_wait": 256 / elapsed,
        "max_in_flight": max(int(row["inFlight"]) for row in all_frames),
        "gpu_distribution": "unavailable: this fixture CSV does not identify unique delayed samples"}
    if args.editor_csv:
        report["editor"] = editor_summary(args.editor_csv)
    output = args.directory / "summary.json"
    output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(output)
    if args.preview:
        from PIL import Image, ImageDraw
        sheet = Image.new("RGB", (1280, 390), (22, 24, 28))
        draw = ImageDraw.Draw(sheet)
        draw.text((12, 9), "Original PBRTest | 640 x 360 | fixed seed | EV14 | ACES / sRGB | scene unchanged", fill="white")
        def display_channel(value):
            x = max(0, value / 16384)
            x = min(1, x * (2.51*x + .03) / (x * (2.43*x + .59) + .14))
            x = 12.92*x if x <= .0031308 else 1.055*x**(1/2.4)-.055
            return round(255*x)
        for row, kind in enumerate(("raw", "denoised")):
            for column, spp in enumerate((1, 4, 16, 64, 256)):
                image = read_pfm(args.directory / f"{spp}-{kind}.pfm")
                tile = Image.frombytes("RGB", (image.width, image.height),
                    bytes(display_channel(value) for value in image.values))
                tile.thumbnail((252, 144), Image.Resampling.LANCZOS)
                x, y = column * 256, 35 + row * 176
                draw.text((x + 4, y), f"{spp} spp | {kind}", fill="white")
                sheet.paste(tile, (x + 2, y + 21))
        sheet.save(args.directory / "pbrtest-stage-preview.jpg", quality=92)


if __name__ == "__main__":
    main()
