"""Verify UE face axes and the Radiance HDR output without launching the engine."""

import tempfile
import unittest
from pathlib import Path
import re

import numpy as np
import OpenEXR

from ConvertProbeFaces import convert


COLORS = {
    "px": (1, 0, 0), "nx": (0, 1, 0),
    "py": (0, 0, 1), "ny": (1, 1, 0),
    "pz": (0, 1, 1), "nz": (1, 0, 1),
}


def read_literal_hdr(path):
    with path.open("rb") as source:
        while True:
            line = source.readline()
            if not line:
                raise ValueError("Missing Radiance dimensions")
            match = re.fullmatch(rb"-Y (\d+) \+X (\d+)\r?\n", line)
            if match:
                height, width = map(int, match.groups())
                break
        rows = []
        for _ in range(height):
            assert source.read(4) == bytes((2, 2, width >> 8, width & 255))
            channels = []
            for _ in range(4):
                data = bytearray()
                while len(data) < width:
                    length = source.read(1)[0]
                    data.extend(source.read(length))
                channels.append(np.frombuffer(bytes(data), dtype=np.uint8))
            rows.append(np.stack(channels, axis=-1))
        rgbe = np.stack(rows).astype(np.float32)
        scale = np.exp2(rgbe[:, :, 3] - 128 - 8)
        return rgbe[:, :, :3] * scale[:, :, None]


class ProbeFaceConversionTest(unittest.TestCase):
    def test_ue_to_he_axes(self):
        with tempfile.TemporaryDirectory() as root:
            prefix = Path(root) / "cube"
            for name, color in COLORS.items():
                pixels = np.broadcast_to(color, (16, 16, 3)).astype(np.float32).copy()
                OpenEXR.File({}, {"RGB": pixels}).write(str(prefix) + "-" + name)
            output = Path(root) / "cube.hdr"
            convert(prefix, output, 256, 128)
            hdr = read_literal_hdr(output)
            samples = {
                "px": hdr[64, 128], "nx": hdr[64, 0],
                "py": hdr[64, 192], "ny": hdr[64, 64],
                "pz": hdr[0, 128], "nz": hdr[-1, 128],
            }
            for name, expected in COLORS.items():
                np.testing.assert_allclose(samples[name], expected, atol=0.02, err_msg=name)


if __name__ == "__main__":
    unittest.main()
