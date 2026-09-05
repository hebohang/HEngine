#!/usr/bin/env python3
"""Compare pre-tonemap, scene-linear HDR captures.

The core implementation intentionally uses only the Python standard library.
Pillow is detected at runtime and is used only for reading mask images and for
writing a viewable PNG heatmap. The authoritative heatmap is always a linear
floating-point PFM.
"""

from __future__ import print_function

import argparse
import array
import glob
import hashlib
import json
import math
import os
import re
import struct
import sys
from pathlib import Path


SCHEMA = "hengine.hdr-comparison.v1"
EPSILON = 1.0e-12
LUMA_WEIGHTS = (0.2126, 0.7152, 0.0722)


class ComparisonError(RuntimeError):
    pass


class FloatImage(object):
    def __init__(self, width, height, channels, values, source=None):
        self.width = int(width)
        self.height = int(height)
        self.channels = int(channels)
        try:
            self.values = (
                values
                if isinstance(values, array.array) and values.typecode == "f"
                else array.array("f", values)
            )
        except (TypeError, ValueError, OverflowError) as error:
            raise ComparisonError("Image values cannot be represented as float32: {0}".format(error))
        self.source = str(source) if source is not None else None
        expected = self.width * self.height * self.channels
        if self.width <= 0 or self.height <= 0:
            raise ComparisonError("Image dimensions must be positive")
        if self.channels not in (1, 3, 4):
            raise ComparisonError("Only 1, 3, or 4 channel float images are supported")
        if len(self.values) != expected:
            raise ComparisonError(
                "Image payload has {0} values; expected {1}".format(len(self.values), expected)
            )
        for value in self.values:
            if not math.isfinite(value):
                raise ComparisonError("Image contains NaN or infinity: {0}".format(self.source or "memory"))

    @property
    def pixel_count(self):
        return self.width * self.height


def _read_pfm_line(stream, label):
    while True:
        raw = stream.readline()
        if not raw:
            raise ComparisonError("Unexpected end of PFM while reading {0}".format(label))
        line = raw.strip()
        if line and not line.startswith(b"#"):
            return line


def read_pfm(path):
    path = Path(path)
    with path.open("rb") as stream:
        magic = _read_pfm_line(stream, "magic")
        if magic == b"PF":
            channels = 3
        elif magic == b"Pf":
            channels = 1
        else:
            raise ComparisonError("Unsupported PFM magic in {0}: {1!r}".format(path, magic))

        dimensions = _read_pfm_line(stream, "dimensions").split()
        if len(dimensions) != 2:
            raise ComparisonError("PFM dimensions must contain width and height: {0}".format(path))
        try:
            width, height = int(dimensions[0]), int(dimensions[1])
            scale = float(_read_pfm_line(stream, "scale"))
        except ValueError as error:
            raise ComparisonError("Invalid PFM header in {0}: {1}".format(path, error))
        if width <= 0 or height <= 0 or scale == 0.0 or not math.isfinite(scale):
            raise ComparisonError("Invalid PFM dimensions or scale: {0}".format(path))

        payload = stream.read()
        expected_bytes = width * height * channels * 4
        if len(payload) != expected_bytes:
            raise ComparisonError(
                "PFM payload size mismatch in {0}: got {1}, expected {2}".format(
                    path, len(payload), expected_bytes
                )
            )

    values = array.array("f")
    values.frombytes(payload)
    file_byte_order = "little" if scale < 0.0 else "big"
    if file_byte_order != sys.byteorder:
        values.byteswap()
    magnitude = abs(scale)
    if magnitude != 1.0:
        for index in range(len(values)):
            values[index] *= magnitude

    row_stride = width * channels
    top_down = array.array("f")
    for y in range(height - 1, -1, -1):
        start = y * row_stride
        top_down.extend(values[start : start + row_stride])
    return FloatImage(width, height, channels, top_down, path)


def write_pfm(path, image):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    channels = image.channels
    if channels not in (1, 3):
        raise ComparisonError("PFM writer supports only 1 or 3 channel images")
    magic = b"Pf" if channels == 1 else b"PF"
    row_stride = image.width * channels
    bottom_up = array.array("f")
    for y in range(image.height - 1, -1, -1):
        start = y * row_stride
        bottom_up.extend(image.values[start : start + row_stride])
    if sys.byteorder != "little":
        bottom_up.byteswap()
    with path.open("wb") as stream:
        stream.write(magic + b"\n")
        stream.write("{0} {1}\n".format(image.width, image.height).encode("ascii"))
        stream.write(b"-1.0\n")
        stream.write(bottom_up.tobytes())


def _default_sidecar_path(path):
    candidates = [
        path.with_suffix(path.suffix + ".json"),
        path.with_suffix(".json"),
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise ComparisonError(
        "Raw float input requires a JSON sidecar; tried: {0}".format(
            ", ".join(str(candidate) for candidate in candidates)
        )
    )


def _effective_sidecar_path(path, explicit_sidecar=None):
    path = Path(path)
    if path.suffix.lower() not in (".raw", ".bin", ".f16", ".f32"):
        return Path(explicit_sidecar) if explicit_sidecar else None
    return Path(explicit_sidecar) if explicit_sidecar else _default_sidecar_path(path)


def _normalize_raw_dtype(metadata):
    dtype = str(metadata.get("dtype", "")).lower()
    byte_order = str(metadata.get("endianness", "little")).lower()
    if dtype.startswith("<"):
        byte_order = "little"
        dtype = dtype[1:]
    elif dtype.startswith(">"):
        byte_order = "big"
        dtype = dtype[1:]
    aliases = {
        "f4": "float32",
        "float": "float32",
        "float32": "float32",
        "f2": "float16",
        "half": "float16",
        "float16": "float16",
    }
    if dtype not in aliases:
        raise ComparisonError("Raw sidecar dtype must be float16 or float32")
    if byte_order not in ("little", "big"):
        raise ComparisonError("Raw sidecar endianness must be 'little' or 'big'")
    return aliases[dtype], byte_order


def read_raw_float(path, sidecar_path=None):
    path = Path(path)
    sidecar_path = Path(sidecar_path) if sidecar_path else _default_sidecar_path(path)
    try:
        with sidecar_path.open("r", encoding="utf-8") as stream:
            metadata = json.load(stream)
    except (OSError, ValueError) as error:
        raise ComparisonError("Unable to read raw sidecar {0}: {1}".format(sidecar_path, error))
    if not isinstance(metadata, dict):
        raise ComparisonError("Raw sidecar root must be a JSON object: {0}".format(sidecar_path))

    required = (
        "width",
        "height",
        "channels",
        "dtype",
        "color_space",
        "color_primaries",
    )
    missing = [field for field in required if field not in metadata]
    if missing:
        raise ComparisonError("Raw sidecar is missing fields: {0}".format(", ".join(missing)))
    integer_fields = {
        "width": metadata["width"],
        "height": metadata["height"],
        "channels": metadata["channels"],
        "offset_bytes": metadata.get("offset_bytes", 0),
    }
    for field, value in integer_fields.items():
        if isinstance(value, bool) or not isinstance(value, int):
            raise ComparisonError("Raw sidecar {0} must be an integer".format(field))
    width = integer_fields["width"]
    height = integer_fields["height"]
    channels = integer_fields["channels"]
    offset = integer_fields["offset_bytes"]
    if width <= 0 or height <= 0 or channels not in (1, 3, 4):
        raise ComparisonError("Raw sidecar dimensions/channels are out of range")
    if str(metadata["color_space"]).lower() not in ("linear", "scene-linear", "scene_linear"):
        raise ComparisonError("Raw input must declare color_space as scene-linear")
    if str(metadata["color_primaries"]) != "Rec.709/sRGB":
        raise ComparisonError(
            "Raw input color_primaries must be 'Rec.709/sRGB'; other primaries require an explicit conversion"
        )
    if str(metadata.get("layout", "HWC")).upper() != "HWC":
        raise ComparisonError("Only HWC raw-float layout is supported")
    row_order = str(metadata.get("row_order", "top-down")).lower()
    if row_order not in ("top-down", "bottom-up"):
        raise ComparisonError("row_order must be 'top-down' or 'bottom-up'")
    if offset < 0:
        raise ComparisonError("offset_bytes cannot be negative")
    dtype, byte_order = _normalize_raw_dtype(metadata)
    item_size = 4 if dtype == "float32" else 2
    value_count = width * height * channels
    expected_bytes = value_count * item_size
    with path.open("rb") as stream:
        stream.seek(offset)
        payload = stream.read()
    if len(payload) != expected_bytes:
        raise ComparisonError(
            "Raw payload size mismatch in {0}: got {1}, expected {2}".format(
                path, len(payload), expected_bytes
            )
        )

    if dtype == "float32":
        values = array.array("f")
        values.frombytes(payload)
        if byte_order != sys.byteorder:
            values.byteswap()
    else:
        prefix = "<" if byte_order == "little" else ">"
        values = array.array("f", (item[0] for item in struct.iter_unpack(prefix + "e", payload)))

    channel_order = str(metadata.get("channel_order", {1: "R", 3: "RGB", 4: "RGBA"}.get(channels, ""))).upper()
    accepted_orders = {1: ("R",), 3: ("RGB", "BGR"), 4: ("RGBA", "BGRA")}
    if channel_order not in accepted_orders.get(channels, ()):
        raise ComparisonError("Raw sidecar channel_order does not match its channel count")
    if channel_order in ("BGR", "BGRA"):
        for pixel in range(width * height):
            base = pixel * channels
            values[base], values[base + 2] = values[base + 2], values[base]

    if row_order == "bottom-up":
        row_stride = width * channels
        top_down = array.array("f")
        for y in range(height - 1, -1, -1):
            start = y * row_stride
            top_down.extend(values[start : start + row_stride])
        values = top_down
    return FloatImage(width, height, channels, values, path)


def load_float_image(path, sidecar_path=None):
    path = Path(path)
    suffix = path.suffix.lower()
    if suffix == ".pfm":
        if sidecar_path:
            raise ComparisonError("PFM input does not use a sidecar: {0}".format(path))
        return read_pfm(path)
    if suffix in (".raw", ".bin", ".f16", ".f32"):
        return read_raw_float(path, sidecar_path)
    raise ComparisonError(
        "HDR input must be .pfm or raw-float (.raw/.bin/.f16/.f32) with JSON sidecar: {0}".format(path)
    )


def _require_same_extent(reference, candidate, label="candidate"):
    if reference.width != candidate.width or reference.height != candidate.height:
        raise ComparisonError(
            "{0} dimensions {1}x{2} do not match reference {3}x{4}".format(
                label,
                candidate.width,
                candidate.height,
                reference.width,
                reference.height,
            )
        )
    if reference.channels < 3 or candidate.channels < 3:
        raise ComparisonError("Reference and candidate radiance images must have at least RGB channels")


def load_mask(path, width, height, threshold=0.5):
    path = Path(path)
    suffix = path.suffix.lower()
    if suffix in (".pfm", ".raw", ".bin", ".f16", ".f32"):
        image = load_float_image(path)
        if image.width != width or image.height != height:
            raise ComparisonError("Mask dimensions do not match the HDR image: {0}".format(path))
        result = bytearray(image.pixel_count)
        for pixel in range(image.pixel_count):
            base = pixel * image.channels
            value = image.values[base]
            if image.channels >= 3:
                value = max(value, image.values[base + 1], image.values[base + 2])
            result[pixel] = 1 if value > threshold else 0
        return result

    try:
        from PIL import Image
    except ImportError:
        raise ComparisonError(
            "Pillow is required for non-PFM/non-raw masks; use a float mask or install approved Pillow"
        )
    with Image.open(str(path)) as source:
        if source.size != (width, height):
            raise ComparisonError("Mask dimensions do not match the HDR image: {0}".format(path))
        mode = source.mode
        if mode == "1":
            return bytearray(1 if value else 0 for value in source.getdata())
        if mode in ("RGB", "RGBA", "CMYK", "YCbCr", "HSV", "P"):
            rgb = source.convert("RGB")
            return bytearray(
                1 if (max(pixel[0], pixel[1], pixel[2]) / 255.0) > threshold else 0
                for pixel in rgb.getdata()
            )
        if mode in ("L", "LA"):
            luminance = source.getchannel(0)
            return bytearray(1 if (float(value) / 255.0) > threshold else 0 for value in luminance.getdata())
        if mode.startswith("I;16"):
            return bytearray(1 if (float(value) / 65535.0) > threshold else 0 for value in source.getdata())
        if mode == "I":
            minimum, maximum = source.getextrema()
            if minimum < 0:
                raise ComparisonError("Signed integer masks with negative values are unsupported: {0}".format(path))
            scale = 65535.0 if maximum <= 65535 else 2147483647.0
            return bytearray(1 if (float(value) / scale) > threshold else 0 for value in source.getdata())
        if mode == "F":
            return bytearray(1 if float(value) > threshold else 0 for value in source.getdata())
        raise ComparisonError("Unsupported mask image mode {0!r}: {1}".format(mode, path))


def full_mask(pixel_count):
    return bytearray([1]) * pixel_count


def mask_and(first, second):
    if len(first) != len(second):
        raise ComparisonError("Mask sizes do not match")
    return bytearray(1 if first[index] and second[index] else 0 for index in range(len(first)))


def mask_without(first, excluded):
    if len(first) != len(excluded):
        raise ComparisonError("Mask sizes do not match")
    return bytearray(1 if first[index] and not excluded[index] else 0 for index in range(len(first)))


def dilate_mask(mask, width, height, radius):
    radius = int(radius)
    if radius < 0 or radius > 64:
        raise ComparisonError("Edge exclusion radius must be between 0 and 64")
    current = bytearray(mask)
    for unused in range(radius):
        expanded = bytearray(current)
        for y in range(height):
            for x in range(width):
                index = y * width + x
                if not current[index]:
                    continue
                min_y, max_y = max(0, y - 1), min(height - 1, y + 1)
                min_x, max_x = max(0, x - 1), min(width - 1, x + 1)
                for adjacent_y in range(min_y, max_y + 1):
                    row = adjacent_y * width
                    for adjacent_x in range(min_x, max_x + 1):
                        expanded[row + adjacent_x] = 1
        current = expanded
    return current


def _safe_ratio(numerator, denominator):
    if abs(denominator) <= EPSILON:
        return 0.0 if abs(numerator) <= EPSILON else None
    return numerator / denominator


def _luminance(red, green, blue):
    return LUMA_WEIGHTS[0] * red + LUMA_WEIGHTS[1] * green + LUMA_WEIGHTS[2] * blue


def compute_image_metrics(
    reference, candidate, mask, chromaticity_min_rgb_sum=1.0e-6
):
    _require_same_extent(reference, candidate)
    if len(mask) != reference.pixel_count:
        raise ComparisonError("Metric mask dimensions do not match the images")

    selected = 0
    squared_error = 0.0
    squared_reference = 0.0
    absolute_error = 0.0
    absolute_reference = 0.0
    luminance_squared_error = 0.0
    luminance_squared_reference = 0.0
    luminance_absolute_error = 0.0
    luminance_absolute_reference = 0.0
    chromaticity_squared_error = 0.0
    chromaticity_l1_error = 0.0
    chromaticity_pixels = 0
    chromaticity_black_transitions = 0

    for pixel in range(reference.pixel_count):
        if not mask[pixel]:
            continue
        selected += 1
        reference_base = pixel * reference.channels
        candidate_base = pixel * candidate.channels
        reference_rgb = (
            reference.values[reference_base],
            reference.values[reference_base + 1],
            reference.values[reference_base + 2],
        )
        candidate_rgb = (
            candidate.values[candidate_base],
            candidate.values[candidate_base + 1],
            candidate.values[candidate_base + 2],
        )
        for channel in range(3):
            difference = candidate_rgb[channel] - reference_rgb[channel]
            squared_error += difference * difference
            squared_reference += reference_rgb[channel] * reference_rgb[channel]
            absolute_error += abs(difference)
            absolute_reference += abs(reference_rgb[channel])

        reference_luminance = _luminance(*reference_rgb)
        candidate_luminance = _luminance(*candidate_rgb)
        luminance_difference = candidate_luminance - reference_luminance
        luminance_squared_error += luminance_difference * luminance_difference
        luminance_squared_reference += reference_luminance * reference_luminance
        luminance_absolute_error += abs(luminance_difference)
        luminance_absolute_reference += abs(reference_luminance)

        positive_reference = tuple(max(0.0, value) for value in reference_rgb)
        positive_candidate = tuple(max(0.0, value) for value in candidate_rgb)
        reference_sum = sum(positive_reference)
        candidate_sum = sum(positive_candidate)
        reference_has_chroma = reference_sum > chromaticity_min_rgb_sum
        candidate_has_chroma = candidate_sum > chromaticity_min_rgb_sum
        if reference_has_chroma and candidate_has_chroma:
            chromaticity_pixels += 1
            for channel in range(3):
                reference_chroma = positive_reference[channel] / reference_sum
                candidate_chroma = positive_candidate[channel] / candidate_sum
                difference = candidate_chroma - reference_chroma
                chromaticity_squared_error += difference * difference
                chromaticity_l1_error += abs(difference)
        elif reference_has_chroma != candidate_has_chroma:
            chromaticity_black_transitions += 1

    if selected == 0:
        raise ComparisonError("Metric scope contains no pixels")
    component_count = selected * 3
    rgb_rmse = math.sqrt(squared_error / component_count)
    rgb_reference_rms = math.sqrt(squared_reference / component_count)
    luminance_rmse = math.sqrt(luminance_squared_error / selected)
    luminance_reference_rms = math.sqrt(luminance_squared_reference / selected)
    return {
        "pixel_count": selected,
        "nrmse": _safe_ratio(rgb_rmse, rgb_reference_rms),
        "rmse": rgb_rmse,
        "reference_rms": rgb_reference_rms,
        "relative_l1": _safe_ratio(absolute_error, absolute_reference),
        "mean_absolute_error": absolute_error / component_count,
        "luminance": {
            "nrmse": _safe_ratio(luminance_rmse, luminance_reference_rms),
            "rmse": luminance_rmse,
            "relative_l1": _safe_ratio(luminance_absolute_error, luminance_absolute_reference),
            "mean_absolute_error": luminance_absolute_error / selected,
        },
        "chromaticity": {
            "minimum_positive_rgb_sum": chromaticity_min_rgb_sum,
            "valid_pixel_count": chromaticity_pixels,
            "excluded_low_energy_pixel_count": selected - chromaticity_pixels,
            "black_transition_pixel_count": chromaticity_black_transitions,
            "rmse": (
                math.sqrt(chromaticity_squared_error / (chromaticity_pixels * 3))
                if chromaticity_pixels
                else None
            ),
            "mean_vector_l1": (
                chromaticity_l1_error / chromaticity_pixels
                if chromaticity_pixels
                else None
            ),
        },
    }


def compute_bleeding_roi(reference, candidate, mask, channel_name):
    _require_same_extent(reference, candidate)
    channel_indices = {"red": 0, "green": 1}
    if channel_name not in channel_indices:
        raise ComparisonError("Bleeding channel must be red or green")
    channel = channel_indices[channel_name]
    selected = 0
    reference_channel = 0.0
    candidate_channel = 0.0
    reference_luminance = 0.0
    candidate_luminance = 0.0
    channel_absolute_error = 0.0
    for pixel in range(reference.pixel_count):
        if not mask[pixel]:
            continue
        selected += 1
        reference_base = pixel * reference.channels
        candidate_base = pixel * candidate.channels
        reference_rgb = [reference.values[reference_base + item] for item in range(3)]
        candidate_rgb = [candidate.values[candidate_base + item] for item in range(3)]
        reference_channel += max(0.0, reference_rgb[channel])
        candidate_channel += max(0.0, candidate_rgb[channel])
        reference_luminance += max(0.0, _luminance(*reference_rgb))
        candidate_luminance += max(0.0, _luminance(*candidate_rgb))
        channel_absolute_error += abs(candidate_rgb[channel] - reference_rgb[channel])
    if selected == 0:
        raise ComparisonError("Bleeding ROI contains no pixels")
    reference_ratio = _safe_ratio(reference_channel, reference_luminance)
    candidate_ratio = _safe_ratio(candidate_channel, candidate_luminance)
    ratio_delta = None if reference_ratio is None or candidate_ratio is None else candidate_ratio - reference_ratio
    return {
        "available": True,
        "pixel_count": selected,
        "channel": channel_name,
        "reference_channel_to_luminance": reference_ratio,
        "candidate_channel_to_luminance": candidate_ratio,
        "channel_to_luminance_delta": ratio_delta,
        "channel_relative_l1": _safe_ratio(channel_absolute_error, reference_channel),
    }


def compute_occluded_leak(reference, candidate, mask):
    _require_same_extent(reference, candidate)
    selected = 0
    positive_excess = 0.0
    reference_energy = 0.0
    candidate_energy = 0.0
    for pixel in range(reference.pixel_count):
        if not mask[pixel]:
            continue
        selected += 1
        reference_base = pixel * reference.channels
        candidate_base = pixel * candidate.channels
        reference_luminance = max(
            0.0,
            _luminance(
                reference.values[reference_base],
                reference.values[reference_base + 1],
                reference.values[reference_base + 2],
            ),
        )
        candidate_luminance = max(
            0.0,
            _luminance(
                candidate.values[candidate_base],
                candidate.values[candidate_base + 1],
                candidate.values[candidate_base + 2],
            ),
        )
        reference_energy += reference_luminance
        candidate_energy += candidate_luminance
        positive_excess += max(candidate_luminance - reference_luminance, 0.0)
    if selected == 0:
        raise ComparisonError("Occluded ROI contains no pixels")
    signed_excess = candidate_energy - reference_energy
    positive_mean_excess = max(signed_excess, 0.0)
    return {
        "available": True,
        "pixel_count": selected,
        "signed_mean_luminance_difference": signed_excess / selected,
        "positive_mean_excess_luminance": positive_mean_excess / selected,
        "positive_excess_mean_luminance": positive_excess / selected,
        "reference_mean_luminance": reference_energy / selected,
        "candidate_mean_luminance": candidate_energy / selected,
        # Gate the systematic ROI-mean excess. Per-pixel half-wave rectification
        # is retained below as a conservative diagnostic, but it has a strictly
        # positive Monte-Carlo noise bias even for two equal unbiased estimators.
        "leak_ratio_to_reference": _safe_ratio(
            positive_mean_excess,
            reference_energy,
        ),
        "excess_fraction_of_candidate": _safe_ratio(
            positive_mean_excess,
            candidate_energy,
        ),
        "positive_pixel_excess_ratio_to_reference": _safe_ratio(
            positive_excess,
            reference_energy,
        ),
    }


def compute_scoped_metric(function, reference, candidate, source_mask, interior_mask, *extra):
    def compute(mask, scope):
        pixel_count = sum(mask)
        if pixel_count == 0:
            return {
                "available": False,
                "reason": "{0} scope contains no selected pixels".format(scope),
                "pixel_count": 0,
            }
        return function(reference, candidate, mask, *extra)

    return {
        "global": compute(source_mask, "global"),
        "interior": compute(mask_and(source_mask, interior_mask), "interior"),
    }


class TemporalAccumulator(object):
    def __init__(self, width, height, mask):
        self.width = width
        self.height = height
        self.mask = mask
        self.pixel_count = width * height
        if len(mask) != self.pixel_count:
            raise ComparisonError("Temporal mask dimensions do not match")
        if sum(mask) == 0:
            raise ComparisonError("Temporal mask contains no pixels")
        self.count = 0
        self.means = array.array("d", [0.0]) * self.pixel_count
        self.m2 = array.array("d", [0.0]) * self.pixel_count
        self.frame_mean_luminance = []
        self.normalization_sum = 0.0
        self.normalization_count = 0
        self.mode = None

    def add(self, image, reference=None):
        if image.width != self.width or image.height != self.height or image.channels < 3:
            raise ComparisonError("Temporal frame dimensions do not match")
        current_mode = "reference_residual" if reference is not None else "signal"
        if self.mode is not None and self.mode != current_mode:
            raise ComparisonError("Temporal accumulator cannot mix signal and residual frames")
        self.mode = current_mode
        if reference is not None:
            _require_same_extent(reference, image, "temporal frame")
        self.count += 1
        frame_sum = 0.0
        selected = 0
        for pixel in range(self.pixel_count):
            if not self.mask[pixel]:
                continue
            base = pixel * image.channels
            luminance = _luminance(
                image.values[base], image.values[base + 1], image.values[base + 2]
            )
            if reference is not None:
                reference_base = pixel * reference.channels
                reference_luminance = _luminance(
                    reference.values[reference_base],
                    reference.values[reference_base + 1],
                    reference.values[reference_base + 2],
                )
                sample = luminance - reference_luminance
                self.normalization_sum += abs(reference_luminance)
            else:
                sample = luminance
                self.normalization_sum += abs(luminance)
            self.normalization_count += 1
            selected += 1
            frame_sum += sample
            delta = sample - self.means[pixel]
            self.means[pixel] += delta / self.count
            self.m2[pixel] += delta * (sample - self.means[pixel])
        self.frame_mean_luminance.append(frame_sum / selected)

    def finalize(self):
        if self.count < 2:
            return {
                "available": False,
                "reason": "Temporal standard deviation requires at least two frames",
                "frame_count": self.count,
            }
        selected = 0
        sum_mean = 0.0
        sum_stddev = 0.0
        sum_coefficient = 0.0
        coefficient_count = 0
        for pixel in range(self.pixel_count):
            if not self.mask[pixel]:
                continue
            selected += 1
            mean = self.means[pixel]
            standard_deviation = math.sqrt(max(0.0, self.m2[pixel] / self.count))
            sum_mean += mean
            sum_stddev += standard_deviation
            if abs(mean) > EPSILON:
                sum_coefficient += standard_deviation / abs(mean)
                coefficient_count += 1
        mean_luminance = sum_mean / selected
        mean_pixel_stddev = sum_stddev / selected
        normalization_mean = self.normalization_sum / self.normalization_count
        frame_mean = sum(self.frame_mean_luminance) / self.count
        frame_variance = sum(
            (value - frame_mean) * (value - frame_mean) for value in self.frame_mean_luminance
        ) / self.count
        return {
            "available": True,
            "frame_count": self.count,
            "pixel_count": selected,
            "mode": self.mode,
            "mean_value": mean_luminance,
            "normalization_mean_luminance": normalization_mean,
            "mean_pixel_stddev": mean_pixel_stddev,
            "normalized_temporal_stddev": _safe_ratio(mean_pixel_stddev, normalization_mean),
            "mean_pixel_coefficient_of_variation": (
                sum_coefficient / coefficient_count
                if self.mode == "signal" and coefficient_count
                else None
            ),
            "frame_mean_value_stddev": math.sqrt(max(0.0, frame_variance)),
        }


class DisocclusionAccumulator(object):
    def __init__(self, width, height, mask, start_frame, threshold, consecutive_frames):
        self.width = width
        self.height = height
        self.mask = mask
        self.start_frame = int(start_frame)
        self.threshold = float(threshold)
        self.consecutive_frames = int(consecutive_frames)
        if self.start_frame < 0:
            raise ComparisonError("Disocclusion start frame cannot be negative")
        if self.threshold < 0.0:
            raise ComparisonError("Disocclusion threshold cannot be negative")
        if self.consecutive_frames <= 0:
            raise ComparisonError("Disocclusion consecutive frame count must be positive")
        if len(mask) != width * height or sum(mask) == 0:
            raise ComparisonError("Disocclusion mask is empty or has the wrong dimensions")
        self.curve = []

    def add(self, image, reference):
        _require_same_extent(reference, image, "temporal frame")
        if image.width != self.width or image.height != self.height:
            raise ComparisonError("Disocclusion frame dimensions do not match")
        error = 0.0
        reference_energy = 0.0
        for pixel in range(self.width * self.height):
            if not self.mask[pixel]:
                continue
            image_base = pixel * image.channels
            reference_base = pixel * reference.channels
            image_luminance = _luminance(
                image.values[image_base], image.values[image_base + 1], image.values[image_base + 2]
            )
            reference_luminance = _luminance(
                reference.values[reference_base],
                reference.values[reference_base + 1],
                reference.values[reference_base + 2],
            )
            error += abs(image_luminance - reference_luminance)
            reference_energy += abs(reference_luminance)
        self.curve.append(_safe_ratio(error, reference_energy))

    def finalize(self):
        first_good_frame = None
        confirmation_frame = None
        if self.start_frame < len(self.curve):
            run = 0
            for frame in range(self.start_frame, len(self.curve)):
                value = self.curve[frame]
                if value is not None and value <= self.threshold:
                    run += 1
                    if run >= self.consecutive_frames:
                        first_good_frame = frame - self.consecutive_frames + 1
                        confirmation_frame = frame
                        break
                else:
                    run = 0
        return {
            "available": True,
            "metric": "luminance_relative_l1",
            "start_frame": self.start_frame,
            "threshold": self.threshold,
            "required_consecutive_frames": self.consecutive_frames,
            "error_curve": self.curve,
            "first_good_frame": first_good_frame,
            "confirmation_frame": confirmation_frame,
            "recovery_frame": first_good_frame,
            "frames_after_start": (
                first_good_frame - self.start_frame if first_good_frame is not None else None
            ),
            "confirmation_frames_after_start": (
                confirmation_frame - self.start_frame
                if confirmation_frame is not None
                else None
            ),
            "recovered": first_good_frame is not None,
        }


def make_luminance_error_image(reference, candidate):
    _require_same_extent(reference, candidate)
    values = array.array("f")
    maximum = 0.0
    for pixel in range(reference.pixel_count):
        reference_base = pixel * reference.channels
        candidate_base = pixel * candidate.channels
        reference_luminance = _luminance(
            reference.values[reference_base],
            reference.values[reference_base + 1],
            reference.values[reference_base + 2],
        )
        candidate_luminance = _luminance(
            candidate.values[candidate_base],
            candidate.values[candidate_base + 1],
            candidate.values[candidate_base + 2],
        )
        error = abs(candidate_luminance - reference_luminance)
        maximum = max(maximum, error)
        values.extend((error, error, error))
    return FloatImage(reference.width, reference.height, 3, values), maximum


def _heat_color(value):
    value = min(1.0, max(0.0, value))
    stops = (
        (0.00, (0, 0, 0)),
        (0.20, (28, 20, 90)),
        (0.45, (0, 150, 220)),
        (0.70, (245, 225, 40)),
        (1.00, (220, 20, 20)),
    )
    for index in range(1, len(stops)):
        if value <= stops[index][0]:
            first_position, first = stops[index - 1]
            second_position, second = stops[index]
            alpha = (value - first_position) / (second_position - first_position)
            return tuple(int(round(first[item] + alpha * (second[item] - first[item]))) for item in range(3))
    return stops[-1][1]


def write_heatmap_png(path, error_image, scale):
    try:
        from PIL import Image
    except ImportError:
        return {
            "available": False,
            "reason": "Pillow is not installed; the authoritative PFM heatmap was still written",
        }
    normalization = scale if scale > EPSILON else 1.0
    pixels = bytearray(error_image.pixel_count * 3)
    for pixel in range(error_image.pixel_count):
        color = _heat_color(error_image.values[pixel * 3] / normalization)
        base = pixel * 3
        pixels[base], pixels[base + 1], pixels[base + 2] = color
    image = Image.frombytes("RGB", (error_image.width, error_image.height), bytes(pixels))
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(str(path), format="PNG")
    return {
        "available": True,
        "path": str(path.resolve()),
        "sha256": _sha256(path),
        "normalization_scale": normalization,
    }


def _validated_sha256(payload, field):
    value = str(payload.get(field, "")).lower()
    if len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
        raise ComparisonError("HDR-FLIP adapter {0} must be a SHA-256 digest".format(field))
    return value


def load_hdr_flip_policy(path):
    if not path:
        return None
    path = Path(path)
    try:
        with path.open("r", encoding="utf-8") as stream:
            payload = json.load(stream)
    except (OSError, ValueError) as error:
        raise ComparisonError("Unable to read HDR-FLIP policy: {0}".format(error))
    if not isinstance(payload, dict):
        raise ComparisonError("HDR-FLIP policy root must be a JSON object")
    if payload.get("schema") != "hengine.hdr-flip-policy.v1":
        raise ComparisonError("HDR-FLIP policy schema must be 'hengine.hdr-flip-policy.v1'")
    required = (
        "tool",
        "version",
        "pixels_per_degree",
        "tone_mapper",
        "exposure_policy",
        "p95_method",
    )
    for field in required:
        if field not in payload:
            raise ComparisonError("HDR-FLIP policy is missing field: {0}".format(field))
    try:
        pixels_per_degree = float(payload["pixels_per_degree"])
    except (TypeError, ValueError, OverflowError) as error:
        raise ComparisonError("HDR-FLIP policy PPD is invalid: {0}".format(error))
    if not math.isfinite(pixels_per_degree) or pixels_per_degree <= 0.0:
        raise ComparisonError("HDR-FLIP policy PPD must be finite and positive")
    exposure_policy = str(payload["exposure_policy"])
    if exposure_policy not in ("automatic", "fixed"):
        raise ComparisonError("HDR-FLIP exposure_policy must be automatic or fixed")
    p95_method = str(payload["p95_method"])
    if p95_method != "nearest-rank-unweighted-pixels":
        raise ComparisonError(
            "HDR-FLIP policy p95_method must be 'nearest-rank-unweighted-pixels'"
        )
    result = {
        "tool": str(payload["tool"]),
        "version": str(payload["version"]),
        "pixels_per_degree": pixels_per_degree,
        "tone_mapper": str(payload["tone_mapper"]),
        "exposure_policy": exposure_policy,
        "p95_method": p95_method,
        "source": str(path.resolve()),
        "sha256": _sha256(path),
    }
    if exposure_policy == "fixed":
        for field in ("start_exposure", "stop_exposure", "exposure_count"):
            if field not in payload:
                raise ComparisonError("Fixed HDR-FLIP policy is missing field: {0}".format(field))
        try:
            start_exposure = float(payload["start_exposure"])
            stop_exposure = float(payload["stop_exposure"])
        except (TypeError, ValueError, OverflowError) as error:
            raise ComparisonError("Fixed HDR-FLIP exposure range is invalid: {0}".format(error))
        exposure_count = payload["exposure_count"]
        if isinstance(exposure_count, bool) or not isinstance(exposure_count, int):
            raise ComparisonError("Fixed HDR-FLIP exposure_count must be an integer")
        if (
            not math.isfinite(start_exposure)
            or not math.isfinite(stop_exposure)
            or start_exposure > stop_exposure
            or exposure_count <= 0
        ):
            raise ComparisonError("Fixed HDR-FLIP policy exposure range is out of range")
        result.update(
            {
                "start_exposure": start_exposure,
                "stop_exposure": stop_exposure,
                "exposure_count": exposure_count,
            }
        )
    return result


def load_hdr_flip_adapter(path, expected_context=None, policy_path=None):
    if not path:
        return {
            "available": False,
            "status": "unavailable",
            "reason": (
                "No approved HDR-FLIP implementation is present in the repository and no "
                "--hdr-flip-report adapter result was provided; no substitute metric was used"
            ),
        }
    path = Path(path)
    try:
        with path.open("r", encoding="utf-8") as stream:
            payload = json.load(stream)
    except (OSError, ValueError) as error:
        raise ComparisonError("Unable to read HDR-FLIP adapter report: {0}".format(error))
    if not isinstance(payload, dict):
        raise ComparisonError("HDR-FLIP adapter report root must be a JSON object")
    if payload.get("schema") != "hdr-flip-adapter.v1":
        raise ComparisonError("HDR-FLIP adapter schema must be 'hdr-flip-adapter.v1'")
    required_fields = (
        "mean",
        "p95",
        "tool",
        "version",
        "reference_sha256",
        "candidate_sha256",
        "reference_decoded_rgb_sha256",
        "candidate_decoded_rgb_sha256",
        "width",
        "height",
        "color_space",
        "color_primaries",
        "scope",
            "parameters",
    )
    for field in required_fields:
        if field not in payload:
            raise ComparisonError("HDR-FLIP adapter report is missing field: {0}".format(field))
    if isinstance(payload["mean"], bool) or isinstance(payload["p95"], bool):
        raise ComparisonError("HDR-FLIP adapter mean/p95 must be numbers")
    try:
        mean = float(payload["mean"])
        p95 = float(payload["p95"])
    except (TypeError, ValueError, OverflowError) as error:
        raise ComparisonError("HDR-FLIP adapter values are invalid: {0}".format(error))
    if (
        not math.isfinite(mean)
        or not math.isfinite(p95)
        or mean < 0.0
        or p95 < 0.0
        or mean > 1.0
        or p95 > 1.0
    ):
        raise ComparisonError("HDR-FLIP adapter values must be finite and in [0, 1]")
    reference_sha256 = _validated_sha256(payload, "reference_sha256")
    candidate_sha256 = _validated_sha256(payload, "candidate_sha256")
    reference_decoded_rgb_sha256 = _validated_sha256(
        payload, "reference_decoded_rgb_sha256"
    )
    candidate_decoded_rgb_sha256 = _validated_sha256(
        payload, "candidate_decoded_rgb_sha256"
    )
    if (
        isinstance(payload["width"], bool)
        or not isinstance(payload["width"], int)
        or isinstance(payload["height"], bool)
        or not isinstance(payload["height"], int)
    ):
        raise ComparisonError("HDR-FLIP adapter dimensions must be integers")
    width = payload["width"]
    height = payload["height"]
    if width <= 0 or height <= 0:
        raise ComparisonError("HDR-FLIP adapter dimensions must be positive")
    if payload["color_space"] != "scene-linear-rgb":
        raise ComparisonError("HDR-FLIP adapter color_space must be 'scene-linear-rgb'")
    if payload["color_primaries"] != "Rec.709/sRGB":
        raise ComparisonError("HDR-FLIP adapter color_primaries must be 'Rec.709/sRGB'")
    if payload["scope"] != "global":
        raise ComparisonError("HDR-FLIP adapter v1 supports only a global, unmasked scope")
    parameters = payload["parameters"]
    if not isinstance(parameters, dict):
        raise ComparisonError("HDR-FLIP adapter parameters must be an object")
    parameter_fields = (
        "pixels_per_degree",
        "tone_mapper",
        "start_exposure",
        "stop_exposure",
        "exposure_count",
        "exposure_policy",
        "p95_method",
    )
    for field in parameter_fields:
        if field not in parameters:
            raise ComparisonError("HDR-FLIP adapter parameters are missing field: {0}".format(field))
    try:
        pixels_per_degree = float(parameters["pixels_per_degree"])
        start_exposure = float(parameters["start_exposure"])
        stop_exposure = float(parameters["stop_exposure"])
        exposure_count = parameters["exposure_count"]
    except (TypeError, ValueError, OverflowError) as error:
        raise ComparisonError("HDR-FLIP adapter parameters are invalid: {0}".format(error))
    if isinstance(exposure_count, bool) or not isinstance(exposure_count, int):
        raise ComparisonError("HDR-FLIP adapter exposure_count must be an integer")
    if (
        not math.isfinite(pixels_per_degree)
        or pixels_per_degree <= 0.0
        or not math.isfinite(start_exposure)
        or not math.isfinite(stop_exposure)
        or start_exposure > stop_exposure
        or exposure_count <= 0
    ):
        raise ComparisonError("HDR-FLIP adapter exposure/PPD parameters are out of range")
    tone_mapper = str(parameters["tone_mapper"]).strip()
    exposure_policy = str(parameters["exposure_policy"]).strip()
    p95_method = str(parameters["p95_method"]).strip()
    if not tone_mapper or exposure_policy not in ("automatic", "fixed"):
        raise ComparisonError("HDR-FLIP adapter tone_mapper/exposure_policy is invalid")
    if p95_method != "nearest-rank-unweighted-pixels":
        raise ComparisonError(
            "HDR-FLIP adapter p95_method must be 'nearest-rank-unweighted-pixels'"
        )
    if expected_context:
        expected_values = {
            "reference_sha256": expected_context["reference_sha256"].lower(),
            "candidate_sha256": expected_context["candidate_sha256"].lower(),
            "reference_decoded_rgb_sha256": expected_context[
                "reference_decoded_rgb_sha256"
            ].lower(),
            "candidate_decoded_rgb_sha256": expected_context[
                "candidate_decoded_rgb_sha256"
            ].lower(),
            "width": int(expected_context["width"]),
            "height": int(expected_context["height"]),
        }
        actual_values = {
            "reference_sha256": reference_sha256,
            "candidate_sha256": candidate_sha256,
            "reference_decoded_rgb_sha256": reference_decoded_rgb_sha256,
            "candidate_decoded_rgb_sha256": candidate_decoded_rgb_sha256,
            "width": width,
            "height": height,
        }
        for field, expected in expected_values.items():
            if actual_values[field] != expected:
                raise ComparisonError(
                    "HDR-FLIP adapter {0} does not match the current comparison".format(field)
                )
    policy = load_hdr_flip_policy(policy_path)
    gate_eligible = False
    gate_reason = "No --hdr-flip-policy was provided; values are provenance-only"
    if policy:
        comparisons = (
            ("tool", str(payload["tool"]), policy["tool"]),
            ("version", str(payload["version"]), policy["version"]),
            ("tone_mapper", tone_mapper.lower(), policy["tone_mapper"].lower()),
            ("exposure_policy", exposure_policy, policy["exposure_policy"]),
            ("p95_method", p95_method, policy["p95_method"]),
        )
        for field, actual, expected in comparisons:
            if actual != expected:
                raise ComparisonError(
                    "HDR-FLIP adapter {0} does not match the selected policy".format(field)
                )
        if not math.isclose(
            pixels_per_degree, policy["pixels_per_degree"], rel_tol=0.0, abs_tol=1.0e-6
        ):
            raise ComparisonError("HDR-FLIP adapter PPD does not match the selected policy")
        if exposure_policy == "fixed":
            fixed_comparisons = (
                ("start_exposure", start_exposure, policy["start_exposure"]),
                ("stop_exposure", stop_exposure, policy["stop_exposure"]),
                ("exposure_count", exposure_count, policy["exposure_count"]),
            )
            for field, actual, expected in fixed_comparisons:
                if actual != expected:
                    raise ComparisonError(
                        "HDR-FLIP adapter {0} does not match the fixed policy".format(field)
                    )
        gate_eligible = True
        gate_reason = None
    return {
        "available": True,
        "status": "provided-by-adapter",
        "mean": mean,
        "p95": p95,
        "gate_eligible": gate_eligible,
        "gate_ineligible_reason": gate_reason,
        "gated_mean": mean if gate_eligible else None,
        "gated_p95": p95 if gate_eligible else None,
        "tool": str(payload["tool"]),
        "version": str(payload["version"]),
        "source_report": str(path.resolve()),
        "source_report_sha256": _sha256(path),
        "reference_sha256": reference_sha256,
        "candidate_sha256": candidate_sha256,
        "reference_decoded_rgb_sha256": reference_decoded_rgb_sha256,
        "candidate_decoded_rgb_sha256": candidate_decoded_rgb_sha256,
        "width": width,
        "height": height,
        "color_space": payload["color_space"],
        "color_primaries": payload["color_primaries"],
        "scope": payload["scope"],
        "policy": policy,
        "parameters": {
            "pixels_per_degree": pixels_per_degree,
            "tone_mapper": tone_mapper,
            "start_exposure": start_exposure,
            "stop_exposure": stop_exposure,
            "exposure_count": exposure_count,
            "exposure_policy": exposure_policy,
            "p95_method": p95_method,
        },
    }


def _sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def _decoded_rgb_sha256(image, color_primaries):
    digest = hashlib.sha256()
    digest.update(b"HENGINE_SCENE_LINEAR_RGB_F32_V1\0")
    digest.update(struct.pack("<II", image.width, image.height))
    digest.update(color_primaries.encode("utf-8"))
    digest.update(b"\0")
    if image.channels == 3:
        values = array.array("f", image.values)
    else:
        values = array.array("f")
        for pixel in range(image.pixel_count):
            base = pixel * image.channels
            values.extend(
                (
                    image.values[base],
                    image.values[base + 1],
                    image.values[base + 2],
                )
            )
    if sys.byteorder != "little":
        values.byteswap()
    digest.update(values.tobytes())
    return digest.hexdigest()


def _input_record(path, sidecar=None, image=None, color_primaries="Rec.709/sRGB"):
    record = {"path": str(Path(path).resolve()), "sha256": _sha256(path)}
    if sidecar:
        record["sidecar"] = str(Path(sidecar).resolve())
        record["sidecar_sha256"] = _sha256(sidecar)
    if image is not None:
        record["decoded_rgb_sha256"] = _decoded_rgb_sha256(image, color_primaries)
    return record


def _mask_input_record(path, mask):
    sidecar = _effective_sidecar_path(path)
    record = _input_record(path, sidecar)
    record["selected_pixel_count"] = sum(mask)
    return record


def _sequence_record(records):
    digest_payload = []
    for record in records:
        digest_payload.append(
            {
                "sha256": record["sha256"],
                "sidecar_sha256": record.get("sidecar_sha256"),
                "decoded_rgb_sha256": record.get("decoded_rgb_sha256"),
            }
        )
    encoded = json.dumps(
        digest_payload, sort_keys=True, separators=(",", ":"), allow_nan=False
    ).encode("utf-8")
    return {
        "frame_count": len(records),
        "manifest_sha256": hashlib.sha256(encoded).hexdigest(),
        "frames": records,
    }


def _parse_named_paths(values, option_name):
    result = {}
    for value in values or []:
        if "=" not in value:
            raise ComparisonError("{0} expects NAME=PATH: {1}".format(option_name, value))
        name, path = value.split("=", 1)
        name = name.strip()
        path = path.strip()
        if not name or not path:
            raise ComparisonError("{0} expects a non-empty NAME=PATH".format(option_name))
        if name in result:
            raise ComparisonError("Duplicate mask name: {0}".format(name))
        result[name] = path
    return result


def _resolve_named_mask(name, roi_masks, material_masks, fallback, purpose):
    if not name:
        return fallback
    if name in roi_masks:
        return roi_masks[name]
    if name in material_masks:
        return material_masks[name]
    raise ComparisonError("Unknown {0} mask name: {1}".format(purpose, name))


def _collect_sequence_paths(explicit, pattern):
    paths = list(explicit or [])
    if pattern:
        matches = glob.glob(pattern)
        if not matches:
            raise ComparisonError("Temporal glob matched no files: {0}".format(pattern))
        paths.extend(sorted(matches, key=_natural_sort_key))
    unique = []
    seen = set()
    for path in paths:
        normalized = os.path.normcase(os.path.abspath(path))
        if normalized not in seen:
            seen.add(normalized)
            unique.append(path)
    return unique


def _natural_sort_key(path):
    return [
        (1, int(part)) if part.isdigit() else (0, part.lower())
        for part in re.split(r"([0-9]+)", os.path.normcase(str(path)))
    ]


def _metric_at(report, path):
    value = report
    for key in path:
        if not isinstance(value, dict) or key not in value:
            return None
        value = value[key]
    return value


def evaluate_thresholds(args, report):
    checks = []
    scope = args.threshold_scope
    metric_scope_path = ("metrics", scope)
    metric_scope_label = scope
    if args.threshold_mask:
        if args.threshold_mask in report.get("metrics", {}).get("materials", {}):
            metric_scope_path = (
                "metrics",
                "materials",
                args.threshold_mask,
                scope,
            )
        elif args.threshold_mask in report.get("metrics", {}).get("rois", {}):
            metric_scope_path = (
                "metrics",
                "rois",
                args.threshold_mask,
                scope,
            )
        else:
            raise ComparisonError(
                "Unknown threshold mask: {0}".format(args.threshold_mask)
            )
        metric_scope_label = "{0}:{1}".format(args.threshold_mask, scope)
    definitions = (
        (
            "nrmse",
            args.max_nrmse,
            metric_scope_path + ("nrmse",),
            metric_scope_label,
        ),
        (
            "relative_l1",
            args.max_relative_l1,
            metric_scope_path + ("relative_l1",),
            metric_scope_label,
        ),
        (
            "occluded_leak_ratio",
            args.max_leak_ratio,
            (
                "metrics",
                "occluded_leak",
                args.threshold_scope,
                "leak_ratio_to_reference",
            ),
            args.threshold_scope,
        ),
        (
            "normalized_temporal_stddev",
            args.max_temporal_stddev,
            ("metrics", "temporal", "normalized_temporal_stddev"),
            args.temporal_roi or "interior",
        ),
        (
            "hdr_flip_mean",
            args.max_hdr_flip_mean,
            ("metrics", "hdr_flip", "gated_mean"),
            "global",
        ),
        (
            "hdr_flip_p95",
            args.max_hdr_flip_p95,
            ("metrics", "hdr_flip", "gated_p95"),
            "global",
        ),
        (
            "disocclusion_recovery_frames",
            args.max_disocclusion_recovery_frames,
            ("metrics", "disocclusion", "frames_after_start"),
            args.disocclusion_roi or "interior",
        ),
    )
    for name, limit, path, check_scope in definitions:
        if limit is None:
            continue
        value = _metric_at(report, path)
        status = "PASS" if value is not None and value <= limit else ("UNAVAILABLE" if value is None else "FAIL")
        checks.append(
            {
                "metric": name,
                "scope": check_scope,
                "value": value,
                "maximum": limit,
                "status": status,
            }
        )
    if not checks:
        overall = "NOT_REQUESTED"
    elif all(check["status"] == "PASS" for check in checks):
        overall = "PASS"
    else:
        overall = "FAIL"
    return {"overall": overall, "default_scope": scope, "checks": checks}


def build_argument_parser():
    parser = argparse.ArgumentParser(
        description="Compare pre-tonemap scene-linear HDR PFM/raw-float captures"
    )
    parser.add_argument("--reference", required=True, help="Reference .pfm or raw-float input")
    parser.add_argument("--candidate", required=True, help="Candidate .pfm or raw-float input")
    parser.add_argument("--reference-sidecar", help="JSON sidecar for the reference raw-float input")
    parser.add_argument("--candidate-sidecar", help="JSON sidecar for the candidate raw-float input")
    parser.add_argument(
        "--color-primaries",
        default="Rec.709/sRGB",
        help="Input RGB primaries; currently only Rec.709/sRGB is supported",
    )
    parser.add_argument("--output-dir", required=True, help="Directory for report and heatmaps")
    parser.add_argument("--report-name", default="hdr_comparison.json")
    parser.add_argument("--roi-mask", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--material-mask", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--mask-threshold", type=float, default=0.5)
    parser.add_argument("--chromaticity-min-rgb-sum", type=float, default=1.0e-6)
    parser.add_argument("--edge-mask", help="Mask containing object/material silhouette pixels")
    parser.add_argument("--edge-exclusion-radius", type=int, default=2)
    parser.add_argument("--red-bleeding-roi", metavar="NAME")
    parser.add_argument("--green-bleeding-roi", metavar="NAME")
    parser.add_argument("--occluded-roi", metavar="NAME")
    parser.add_argument("--temporal-frame", action="append", default=[], metavar="PATH")
    parser.add_argument("--temporal-glob", metavar="GLOB")
    parser.add_argument("--temporal-reference-frame", action="append", default=[], metavar="PATH")
    parser.add_argument("--temporal-reference-glob", metavar="GLOB")
    parser.add_argument("--temporal-roi", metavar="NAME")
    parser.add_argument("--disocclusion-roi", metavar="NAME")
    parser.add_argument("--disocclusion-start", type=int, default=0)
    parser.add_argument("--disocclusion-threshold", type=float, default=0.05)
    parser.add_argument("--disocclusion-consecutive", type=int, default=1)
    parser.add_argument(
        "--hdr-flip-report",
        help="Optional externally generated hdr-flip-adapter.v1 JSON; no substitute is computed",
    )
    parser.add_argument(
        "--hdr-flip-policy",
        help="Approved hengine.hdr-flip-policy.v1 JSON required for HDR-FLIP gates",
    )
    parser.add_argument("--heatmap-scale", type=float, help="Fixed linear luminance-error scale for PNG")
    parser.add_argument("--no-heatmap-png", action="store_true")
    parser.add_argument("--threshold-scope", choices=("global", "interior"), default="interior")
    parser.add_argument(
        "--threshold-mask",
        help="Named ROI/material mask used for NRMSE and relative-L1 gates",
    )
    parser.add_argument("--max-nrmse", type=float)
    parser.add_argument("--max-relative-l1", type=float)
    parser.add_argument("--max-leak-ratio", type=float)
    parser.add_argument("--max-temporal-stddev", type=float)
    parser.add_argument("--max-hdr-flip-mean", type=float)
    parser.add_argument("--max-hdr-flip-p95", type=float)
    parser.add_argument("--max-disocclusion-recovery-frames", type=int)
    return parser


def validate_arguments(args):
    finite_nonnegative = (
        ("--disocclusion-threshold", args.disocclusion_threshold),
        ("--heatmap-scale", args.heatmap_scale),
        ("--max-nrmse", args.max_nrmse),
        ("--max-relative-l1", args.max_relative_l1),
        ("--max-leak-ratio", args.max_leak_ratio),
        ("--max-temporal-stddev", args.max_temporal_stddev),
        ("--max-hdr-flip-mean", args.max_hdr_flip_mean),
        ("--max-hdr-flip-p95", args.max_hdr_flip_p95),
        ("--max-disocclusion-recovery-frames", args.max_disocclusion_recovery_frames),
        ("--chromaticity-min-rgb-sum", args.chromaticity_min_rgb_sum),
    )
    for name, value in finite_nonnegative:
        if value is not None and (not math.isfinite(value) or value < 0.0):
            raise ComparisonError("{0} must be finite and non-negative".format(name))
    if not math.isfinite(args.mask_threshold):
        raise ComparisonError("--mask-threshold must be finite")
    if args.color_primaries != "Rec.709/sRGB":
        raise ComparisonError(
            "Only Rec.709/sRGB primaries are supported; convert other linear RGB spaces first"
        )
    if args.edge_exclusion_radius < 0 or args.edge_exclusion_radius > 64:
        raise ComparisonError("--edge-exclusion-radius must be between 0 and 64")
    if args.disocclusion_start < 0:
        raise ComparisonError("--disocclusion-start cannot be negative")
    if args.disocclusion_consecutive <= 0:
        raise ComparisonError("--disocclusion-consecutive must be positive")
    if (
        args.max_disocclusion_recovery_frames is not None
        and not args.disocclusion_roi
    ):
        raise ComparisonError(
            "--max-disocclusion-recovery-frames requires an explicit --disocclusion-roi"
        )


def run_comparison(args):
    validate_arguments(args)
    reference_sidecar = _effective_sidecar_path(args.reference, args.reference_sidecar)
    candidate_sidecar = _effective_sidecar_path(args.candidate, args.candidate_sidecar)
    reference = load_float_image(args.reference, reference_sidecar)
    candidate = load_float_image(args.candidate, candidate_sidecar)
    _require_same_extent(reference, candidate)
    reference_input = _input_record(
        args.reference, reference_sidecar, reference, args.color_primaries
    )
    candidate_input = _input_record(
        args.candidate, candidate_sidecar, candidate, args.color_primaries
    )
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    all_pixels = full_mask(reference.pixel_count)
    named_roi_paths = _parse_named_paths(args.roi_mask, "--roi-mask")
    named_material_paths = _parse_named_paths(args.material_mask, "--material-mask")
    overlap = set(named_roi_paths).intersection(named_material_paths)
    if overlap:
        raise ComparisonError("ROI and material masks share names: {0}".format(", ".join(sorted(overlap))))
    roi_masks = {
        name: load_mask(path, reference.width, reference.height, args.mask_threshold)
        for name, path in named_roi_paths.items()
    }
    material_masks = {
        name: load_mask(path, reference.width, reference.height, args.mask_threshold)
        for name, path in named_material_paths.items()
    }
    if args.edge_mask:
        edge_mask = load_mask(args.edge_mask, reference.width, reference.height, args.mask_threshold)
        excluded_edges = dilate_mask(
            edge_mask, reference.width, reference.height, args.edge_exclusion_radius
        )
        interior = mask_without(all_pixels, excluded_edges)
        if sum(interior) == 0:
            raise ComparisonError("Edge exclusion removed every pixel")
    else:
        edge_mask = None
        excluded_edges = bytearray(reference.pixel_count)
        interior = bytearray(all_pixels)

    metrics = {
        "global": compute_image_metrics(
            reference, candidate, all_pixels, args.chromaticity_min_rgb_sum
        ),
        "interior": compute_image_metrics(
            reference, candidate, interior, args.chromaticity_min_rgb_sum
        ),
        "rois": {},
        "materials": {},
    }
    for name, mask in sorted(roi_masks.items()):
        metrics["rois"][name] = compute_scoped_metric(
            compute_image_metrics,
            reference,
            candidate,
            mask,
            interior,
            args.chromaticity_min_rgb_sum,
        )
    for name, mask in sorted(material_masks.items()):
        metrics["materials"][name] = compute_scoped_metric(
            compute_image_metrics,
            reference,
            candidate,
            mask,
            interior,
            args.chromaticity_min_rgb_sum,
        )

    if args.red_bleeding_roi:
        red_mask = _resolve_named_mask(
            args.red_bleeding_roi, roi_masks, material_masks, interior, "red bleeding ROI"
        )
        metrics["red_bleeding"] = compute_scoped_metric(
            compute_bleeding_roi, reference, candidate, red_mask, interior, "red"
        )
    else:
        metrics["red_bleeding"] = {
            "available": False,
            "reason": "No --red-bleeding-roi was selected",
        }
    if args.green_bleeding_roi:
        green_mask = _resolve_named_mask(
            args.green_bleeding_roi, roi_masks, material_masks, interior, "green bleeding ROI"
        )
        metrics["green_bleeding"] = compute_scoped_metric(
            compute_bleeding_roi, reference, candidate, green_mask, interior, "green"
        )
    else:
        metrics["green_bleeding"] = {
            "available": False,
            "reason": "No --green-bleeding-roi was selected",
        }
    if args.occluded_roi:
        occluded_mask = _resolve_named_mask(
            args.occluded_roi, roi_masks, material_masks, interior, "occluded ROI"
        )
        metrics["occluded_leak"] = compute_scoped_metric(
            compute_occluded_leak, reference, candidate, occluded_mask, interior
        )
    else:
        metrics["occluded_leak"] = {
            "available": False,
            "reason": "No --occluded-roi was selected",
        }

    temporal_paths = _collect_sequence_paths(args.temporal_frame, args.temporal_glob)
    temporal_reference_paths = _collect_sequence_paths(
        args.temporal_reference_frame, args.temporal_reference_glob
    )
    if temporal_reference_paths and len(temporal_reference_paths) != len(temporal_paths):
        raise ComparisonError("Temporal reference sequence length must match the temporal frame sequence")
    if temporal_paths:
        temporal_mask = _resolve_named_mask(
            args.temporal_roi, roi_masks, material_masks, interior, "temporal ROI"
        )
        disocclusion_mask = _resolve_named_mask(
            args.disocclusion_roi, roi_masks, material_masks, interior, "disocclusion ROI"
        )
        temporal_mask = mask_and(temporal_mask, interior)
        disocclusion_mask = mask_and(disocclusion_mask, interior)
        temporal_accumulator = TemporalAccumulator(
            reference.width, reference.height, temporal_mask
        )
        disocclusion_accumulator = DisocclusionAccumulator(
            reference.width,
            reference.height,
            disocclusion_mask,
            args.disocclusion_start,
            args.disocclusion_threshold,
            args.disocclusion_consecutive,
        )
        temporal_frame_records = []
        temporal_reference_records = []
        for index, frame_path in enumerate(temporal_paths):
            frame_sidecar = _effective_sidecar_path(frame_path)
            frame = load_float_image(frame_path, frame_sidecar)
            temporal_frame_records.append(
                _input_record(
                    frame_path, frame_sidecar, frame, args.color_primaries
                )
            )
            if temporal_reference_paths:
                frame_reference_path = temporal_reference_paths[index]
                frame_reference_sidecar = _effective_sidecar_path(frame_reference_path)
                frame_reference = load_float_image(
                    frame_reference_path, frame_reference_sidecar
                )
                temporal_reference_records.append(
                    _input_record(
                        frame_reference_path,
                        frame_reference_sidecar,
                        frame_reference,
                        args.color_primaries,
                    )
                )
            else:
                frame_reference = reference
            temporal_accumulator.add(frame, frame_reference)
            disocclusion_accumulator.add(frame, frame_reference)
        metrics["temporal"] = temporal_accumulator.finalize()
        metrics["disocclusion"] = disocclusion_accumulator.finalize()
    else:
        temporal_frame_records = []
        temporal_reference_records = []
        metrics["temporal"] = {
            "available": False,
            "reason": "No temporal sequence was provided",
        }
        metrics["disocclusion"] = {
            "available": False,
            "reason": "No temporal sequence was provided",
        }
    metrics["hdr_flip"] = load_hdr_flip_adapter(
        args.hdr_flip_report,
        {
            "reference_sha256": reference_input["sha256"],
            "candidate_sha256": candidate_input["sha256"],
            "reference_decoded_rgb_sha256": reference_input["decoded_rgb_sha256"],
            "candidate_decoded_rgb_sha256": candidate_input["decoded_rgb_sha256"],
            "width": reference.width,
            "height": reference.height,
        },
        args.hdr_flip_policy,
    )

    error_image, maximum_error = make_luminance_error_image(reference, candidate)
    error_pfm_path = output_dir / "luminance_absolute_error.pfm"
    write_pfm(error_pfm_path, error_image)
    png_scale = args.heatmap_scale if args.heatmap_scale is not None else maximum_error
    if png_scale < 0.0:
        raise ComparisonError("Heatmap scale cannot be negative")
    if args.no_heatmap_png:
        png_record = {"available": False, "reason": "Disabled by --no-heatmap-png"}
    else:
        png_record = write_heatmap_png(
            output_dir / "luminance_absolute_error_heatmap.png", error_image, png_scale
        )

    report = {
        "schema": SCHEMA,
        "inputs": {
            "reference": reference_input,
            "candidate": candidate_input,
            "dimensions": {
                "width": reference.width,
                "height": reference.height,
                "reference_channels": reference.channels,
                "candidate_channels": candidate.channels,
            },
            "color_space": "scene-linear-rgb",
            "color_primaries": args.color_primaries,
            "temporal_sequence": (
                {
                    "candidates": _sequence_record(temporal_frame_records),
                    "reference_mode": "per-frame" if temporal_reference_paths else "fixed-main-reference",
                    "references": (
                        _sequence_record(temporal_reference_records)
                        if temporal_reference_paths
                        else _sequence_record([reference_input])
                    ),
                }
                if temporal_paths
                else None
            ),
        },
        "masking": {
            "mask_threshold": args.mask_threshold,
            "edge_mask": (
                _mask_input_record(args.edge_mask, edge_mask) if args.edge_mask else None
            ),
            "edge_exclusion_radius_pixels": args.edge_exclusion_radius if args.edge_mask else 0,
            "excluded_edge_pixel_count": sum(excluded_edges),
            "global_pixel_count": reference.pixel_count,
            "interior_pixel_count": sum(interior),
            "roi_masks": {
                name: _mask_input_record(named_roi_paths[name], roi_masks[name])
                for name in sorted(named_roi_paths)
            },
            "material_masks": {
                name: _mask_input_record(named_material_paths[name], material_masks[name])
                for name in sorted(named_material_paths)
            },
            "selections": {
                "red_bleeding_roi": args.red_bleeding_roi,
                "green_bleeding_roi": args.green_bleeding_roi,
                "occluded_roi": args.occluded_roi,
                "temporal_roi": args.temporal_roi,
                "disocclusion_roi": args.disocclusion_roi,
                "threshold_scope": args.threshold_scope,
            },
        },
        "metric_definitions": {
            "nrmse": "RGB RMSE divided by reference RGB RMS over the selected pixels",
            "relative_l1": "sum(abs(candidate-reference)) divided by sum(abs(reference))",
            "luminance": "Rec.709 linear luminance weights (0.2126, 0.7152, 0.0722)",
            "chromaticity": (
                "positive RGB normalized by RGB sum; pixels below the configured energy threshold "
                "are excluded and black/non-black transitions are counted separately"
            ),
            "bleeding": "Selected channel energy divided by luminance energy inside the named ROI",
            "occluded_leak": "positive systematic ROI-mean candidate luminance excess divided by reference luminance energy; per-pixel half-wave excess is diagnostic only because it is Monte-Carlo-noise biased",
            "temporal": (
                "per-pixel population standard deviation of candidate-minus-reference "
                "scene-linear luminance, normalized by mean absolute reference luminance"
            ),
            "disocclusion": "per-frame luminance relative L1 against matching or fixed reference",
        },
        "metrics": metrics,
        "artifacts": {
            "linear_luminance_error_pfm": {
                "path": str(error_pfm_path.resolve()),
                "sha256": _sha256(error_pfm_path),
                "maximum": maximum_error,
            },
            "display_heatmap_png": png_record,
        },
    }
    report["thresholds"] = evaluate_thresholds(args, report)
    report_path = output_dir / args.report_name
    with report_path.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(report, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    return report_path, report["thresholds"]["overall"]


def main(argv=None):
    parser = build_argument_parser()
    args = parser.parse_args(argv)
    try:
        report_path, threshold_status = run_comparison(args)
    except (ComparisonError, OSError) as error:
        print("HDR comparison failed: {0}".format(error), file=sys.stderr)
        return 2
    print("HDR comparison report: {0}".format(report_path.resolve()))
    if threshold_status == "FAIL":
        print("One or more requested thresholds failed", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
