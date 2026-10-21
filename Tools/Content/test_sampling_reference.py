"""Numerical reference cases for GPU sampling capture comparison; no GPU execution."""
import json
import math
import unittest


def decode_srgb(value):
    return value / 12.92 if value <= .04045 else ((value + .055) / 1.055) ** 2.4


def texel(index, size, mode):
    if mode == "clamp":
        return min(max(index, 0), size - 1)
    period = size * (2 if mode == "mirror" else 1)
    wrapped = index % period
    return period - 1 - wrapped if wrapped >= size else wrapped


def sample_row(values, uv, mode="repeat", linear=True):
    # Values are linear-light texels, including sRGB decode before this operation.
    position = uv * len(values) - (.5 if linear else 0)
    origin = math.floor(position)
    first = values[texel(origin, len(values), mode)]
    if not linear:
        return first
    second = values[texel(origin + 1, len(values), mode)]
    return first + (second - first) * (position - origin)


def sample_lod(mips, uv, lod, min_filter, mag_linear, mode="repeat"):
    magnifying = lod <= 0
    linear = mag_linear if magnifying else min_filter in (9729, 9985, 9987)
    level = 0 if magnifying or min_filter < 9984 else min(max(lod, 0), len(mips) - 1)
    if magnifying or min_filter not in (9986, 9987):
        return sample_row(mips[math.floor(level + .5)], uv, mode, linear)
    low = math.floor(level)
    a = sample_row(mips[low], uv, mode, linear)
    b = sample_row(mips[min(low + 1, len(mips) - 1)], uv, mode, linear)
    return a + (b - a) * (level - low)


class SamplingReferenceTests(unittest.TestCase):
    def test_negative_and_mirror_boundaries(self):
        self.assertEqual([texel(i, 2, "mirror") for i in range(-4, 5)], [0,1,1,0,0,1,1,0,0])
        self.assertEqual(sample_row([0,1], -.25), 1)
        self.assertEqual(sample_row([0,1], 0), .5)
        self.assertEqual(sample_row([0,1], 0, "clamp"), 0)
        self.assertEqual(sample_row([0,1], 1, "mirror"), 1)
        self.assertEqual(sample_row([0,1], -.25, "mirror"), 0)

    def test_srgb_is_decoded_before_interpolation(self):
        actual = sample_row([decode_srgb(0), decode_srgb(1)], .5)
        self.assertEqual(actual, .5)
        self.assertGreater(abs(actual - decode_srgb(.5)), .28)

    def test_min_mag_and_mip_selection_are_distinct(self):
        mips = [[0,1,0,1], [.2,.8], [.25]]
        self.assertEqual(sample_lod(mips, .5, -1, 9728, True), .5)
        self.assertEqual(sample_lod(mips, .5, .1, 9728, True), 0)
        self.assertAlmostEqual(sample_lod(mips, .5, 1.25, 9986, False), .6625)
        self.assertAlmostEqual(sample_lod(mips, .5, 1.25, 9987, False), .4375)
        self.assertEqual(sample_lod(mips, .5, 100, 9987, False), .25)
        self.assertEqual(sample_lod(mips, .5, 0, 9987, False), 0)  # RT fixed LOD0 uses mag.


if __name__ == "__main__":
    unittest.main(verbosity=2)
