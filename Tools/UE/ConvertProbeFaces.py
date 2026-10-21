"""Convert six UE SceneCapture2D OpenEXR faces into an HE linear Radiance HDR probe.

Faces are named <prefix>-px/nx/py/ny/pz/nz. UE uses X forward, Y right,
Z up; HE uses X forward, Y up, Z right. The matching capture rotations are
px=(0,0), nx=(0,180), py=(0,90), ny=(0,-90), pz=(90,0), nz=(-90,0).
Requires NumPy and OpenEXR Python bindings; neither is an engine dependency.
"""

import argparse
import math
from pathlib import Path

import numpy as np
import OpenEXR


FACES = {
    "px": ((1, 0, 0), (0, 1, 0), (0, 0, 1)),
    "nx": ((-1, 0, 0), (0, -1, 0), (0, 0, 1)),
    "py": ((0, 1, 0), (-1, 0, 0), (0, 0, 1)),
    "ny": ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
    "pz": ((0, 0, 1), (0, 1, 0), (-1, 0, 0)),
    "nz": ((0, 0, -1), (0, 1, 0), (1, 0, 0)),
}


def load_face(path):
    channels = OpenEXR.File(str(path)).channels()
    if "RGBA" in channels:
        rgb = channels["RGBA"].pixels[:, :, :3]
    elif "RGB" in channels:
        rgb = channels["RGB"].pixels
    else:
        rgb = np.stack([channels[name].pixels for name in "RGB"], axis=-1)
    rgb = np.asarray(rgb, dtype=np.float32)
    if rgb.ndim != 3 or rgb.shape[0] != rgb.shape[1] or rgb.shape[2] != 3:
        raise ValueError("Expected square RGB face: " + str(path))
    if not np.isfinite(rgb).all() or np.min(rgb) < 0:
        raise ValueError("Invalid linear HDR pixels: " + str(path))
    return rgb


def write_hdr(path, rgb):
    height, width, _ = rgb.shape
    maximum = np.max(rgb, axis=2)
    mantissa, exponent = np.frexp(maximum)
    scale = np.zeros_like(maximum)
    active = mantissa > 1e-32
    scale[active] = np.ldexp(np.full(active.sum(), 256.0, np.float32),
                             -exponent[active])
    rgbe = np.empty((height, width, 4), dtype=np.uint8)
    rgbe[:, :, :3] = np.minimum(255, rgb * scale[:, :, None]).astype(np.uint8)
    rgbe[:, :, 3] = np.where(active, exponent + 128, 0).astype(np.uint8)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as output:
        output.write(("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n"
                      "-Y {} +X {}\n".format(height, width)).encode("ascii"))
        for row in rgbe:
            output.write(bytes((2, 2, width >> 8, width & 255)))
            for channel in range(4):
                for offset in range(0, width, 128):
                    values = row[offset:offset + 128, channel].tobytes()
                    output.write(bytes((len(values),)) + values)


def convert(prefix, output, width, height):
    faces = {name: load_face(Path(str(prefix) + "-" + name)) for name in FACES}
    size = next(iter(faces.values())).shape[0]
    if any(face.shape != (size, size, 3) for face in faces.values()):
        raise ValueError("All cubemap faces must have the same dimensions")
    u = (np.arange(width, dtype=np.float32) + 0.5) / width
    v = (np.arange(height, dtype=np.float32) + 0.5) / height
    phi = (u - 0.5) * (2.0 * math.pi)
    latitude = (0.5 - v) * math.pi
    horizontal = np.cos(latitude)[:, None]
    # HE (X,Y,Z) -> UE (X,Z,Y).
    directions = np.stack(np.broadcast_arrays(
        horizontal * np.cos(phi)[None, :],
        horizontal * np.sin(phi)[None, :],
        np.sin(latitude)[:, None]), axis=-1)
    result = np.empty((height, width, 3), dtype=np.float32)
    forward = np.stack([basis[0] for basis in FACES.values()], axis=0)
    face_ids = np.argmax(directions @ forward.T, axis=2)
    for index, (name, (fwd, right, up)) in enumerate(FACES.items()):
        mask = face_ids == index
        rays = directions[mask]
        divisor = rays @ np.asarray(fwd, dtype=np.float32)
        x = ((rays @ np.asarray(right, dtype=np.float32)) / divisor + 1) * 0.5 * (size - 1)
        y = (1 - (rays @ np.asarray(up, dtype=np.float32)) / divisor) * 0.5 * (size - 1)
        x = np.clip(x, 0, size - 1)
        y = np.clip(y, 0, size - 1)
        x0, y0 = x.astype(np.int32), y.astype(np.int32)
        x1, y1 = np.minimum(x0 + 1, size - 1), np.minimum(y0 + 1, size - 1)
        tx, ty = (x - x0)[:, None], (y - y0)[:, None]
        face = faces[name]
        result[mask] = ((1 - ty) * ((1 - tx) * face[y0, x0] + tx * face[y0, x1])
                        + ty * ((1 - tx) * face[y1, x0] + tx * face[y1, x1]))
    write_hdr(output, result)
    print("Wrote {} ({}x{}, six {}x{} faces, RGB min/median/max {:.3f}/{:.3f}/{:.3f})".format(
        output, width, height, size, size,
        np.min(result), np.median(result), np.max(result)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--width", type=int, default=1024)
    parser.add_argument("--height", type=int, default=512)
    args = parser.parse_args()
    if args.width < 8 or args.width > 32767 or args.height < 4:
        parser.error("Unsupported HDR dimensions")
    convert(args.prefix, args.output, args.width, args.height)
