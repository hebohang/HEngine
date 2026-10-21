"""Report alpha coverage of every BC7 DDS mip at a material mask cutoff."""

import argparse
import io
import json
import math
import struct
from pathlib import Path

from PIL import Image


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("dds", type=Path)
parser.add_argument("--cutoff", type=float, default=0.5)
args = parser.parse_args()
if not math.isfinite(args.cutoff) or not 0 <= args.cutoff <= 1:
    parser.error("cutoff must be finite and in [0, 1]")
data = args.dds.read_bytes()
if len(data) < 148 or data[:4] != b"DDS " or data[84:88] != b"DX10":
    raise ValueError("Expected a DX10 DDS")
height, width = struct.unpack_from("<II", data, 12)
mip_count = struct.unpack_from("<I", data, 28)[0]
format_id = struct.unpack_from("<I", data, 128)[0]
if format_id not in (98, 99):
    raise ValueError("Expected BC7 UNORM or BC7 UNORM SRGB")
offset = 148
rows = []
for level in range(mip_count):
    size = ((width + 3) // 4) * ((height + 3) // 4) * 16
    payload = data[offset:offset + size]
    if len(payload) != size:
        raise ValueError("Truncated mip " + str(level))
    header = bytearray(data[:148])
    struct.pack_into("<II", header, 12, height, width)
    struct.pack_into("<I", header, 20, size)
    struct.pack_into("<I", header, 28, 1)
    alpha = Image.open(io.BytesIO(header + payload)).convert("RGBA").getchannel("A")
    histogram = alpha.histogram()
    threshold = max(0, min(255, math.ceil(args.cutoff * 255)))
    rows.append({"mip": level, "width": width, "height": height,
                 "coverage": sum(histogram[threshold:]) / (width * height)})
    offset += size
    width, height = max(1, width // 2), max(1, height // 2)
if offset != len(data):
    raise ValueError("Unexpected trailing DDS data")
print(json.dumps({"dds": str(args.dds), "cutoff": args.cutoff, "mips": rows}))
