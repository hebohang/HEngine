"""Regenerate the tiny BC texture fixtures used by the device upload probe."""

from pathlib import Path
import struct
import subprocess
import zlib


ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "Project/Assets/TestData/TextureUpload"
TEXCONV = ROOT / "Examples/RenderingTests/SourceAssets/RenderingBenchmarks/Tools/DirectXTexMay2026/texconv.exe"


def png_chunk(name: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + name + payload + struct.pack(
        ">I", zlib.crc32(name + payload)
    )


def write_png(path: Path, normal: bool, extreme: bool = False) -> None:
    rows = bytearray()
    for y in range(16):
        rows.append(0)
        for x in range(16):
            if normal:
                rows.extend(((0 if x < 8 else 255) if extreme else 96 + x * 4,
                             (0 if y < 8 else 255) if extreme else 96 + y * 4,
                             255, 255))
            else:
                rows.extend((x * 17, y * 17, ((x ^ y) & 15) * 17,
                             255 if (x + y) % 4 else 0))
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 6, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + png_chunk(b"IEND", b"")
    )


def write_budget_png(path: Path) -> None:
    """Fast-to-decode RGBA fixture with a one MiB uncompressed mip."""
    row = bytes((0,)) + bytes((32, 128, 224, 255)) * 512
    pixels = row * 512
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 512, 512, 8, 6, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(pixels, 9))
        + png_chunk(b"IEND", b"")
    )


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for name, fmt, normal, extreme in (
        ("BC3Color", "BC3_UNORM", False, False),
        ("BC5Normal", "BC5_UNORM", True, False),
        ("BC5Extreme", "BC5_UNORM", True, True),
        ("BC7Color", "BC7_UNORM", False, False),
    ):
        source = OUT / f"{name}.png"
        write_png(source, normal, extreme)
        subprocess.run(
            [str(TEXCONV), "-f", fmt, "-m", "0", "-dx10", "-nogpu", "-y", "-nologo",
             "-o", str(OUT), str(source)],
            check=True,
        )
    write_budget_png(OUT / "PreparationBudget.png")


if __name__ == "__main__":
    main()
