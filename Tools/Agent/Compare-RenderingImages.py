"""Compare completed, equal-size native captures without printing image data.

Metrics are display-encoded RGB differences, not a perceptual quality verdict.
Use identical camera, simulation frame, exposure, AA history and scene inputs.
"""
import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageChops, ImageStat


def percentile(histogram, fraction):
    threshold = sum(histogram) * fraction
    count = 0
    for value, frequency in enumerate(histogram):
        count += frequency
        if count >= threshold:
            return value
    return 255


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    with Image.open(args.reference) as source, Image.open(args.candidate) as target:
        if source.size != target.size:
            raise ValueError("Capture dimensions differ; do not rescale a native comparison")
        reference, candidate = source.convert("RGB"), target.convert("RGB")
    difference = ImageChops.difference(reference, candidate)
    statistics = ImageStat.Stat(difference)
    histogram = difference.histogram()
    combined = [sum(histogram[channel * 256 + value] for channel in range(3))
                for value in range(256)]
    total = sum(combined)
    red, green, blue = difference.split()
    maximum_channel = ImageChops.lighter(ImageChops.lighter(red, green), blue)
    visible_mask = maximum_channel.point(lambda value: 255 if value > 4 else 0)
    changed_pixels = visible_mask.histogram()[255]
    result = {
        "reference": str(args.reference.resolve()),
        "candidate": str(args.candidate.resolve()),
        "referenceSha256": file_hash(args.reference),
        "candidateSha256": file_hash(args.candidate),
        "width": reference.width, "height": reference.height,
        "units": "absolute display-encoded RGB code-value difference, range 0..255",
        "maeRgb": statistics.mean, "rmsRgb": statistics.rms,
        "p95ChannelDifference": percentile(combined, .95),
        "p99ChannelDifference": percentile(combined, .99),
        "maximumChannelDifference": max(high for _, high in statistics.extrema),
        "exactChannelFraction": combined[0] / total,
        "channelFractionOver1": sum(combined[2:]) / total,
        "channelFractionOver4": sum(combined[5:]) / total,
        "pixelCountOver4": changed_pixels,
        "pixelFractionOver4": changed_pixels / (reference.width * reference.height),
        "pixelBoundsOver4": visible_mask.getbbox(),
        "identicalRgb": combined[0] == total,
        "interpretation": "Numerical evidence only; matching frames and visual/motion review remain required.",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
