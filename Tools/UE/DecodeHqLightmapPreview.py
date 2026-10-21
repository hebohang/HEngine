"""Preview one UE 5.4 HQ static lightmap binding exported by HELightmapBridge.

This is a diagnostic decoder, not a replacement for runtime directional lightmap
sampling. The normal is constant across the preview and the output is tone mapped.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def decode(binding: dict, normal: np.ndarray) -> tuple[np.ndarray, dict]:
    if len(binding["coefficientScales"]) != 2 or len(binding["coefficientAdds"]) != 2:
        raise ValueError("Expected two UE HQ lightmap coefficients; run with -AllowCommandletRendering")
    texture = next((item for item in binding["textures"] if item["basis"] == 0), None)
    if texture is None or texture["sourceFormat"] != 2 or not texture.get("rawFile"):
        raise ValueError("Expected a raw TSF_BGRA8 HQ texture exported by HELightmapBridge")
    width, height = texture["sourceWidth"], texture["sourceHeight"]
    if height % 2:
        raise ValueError("UE HQ atlas height must contain two equal coefficient halves")
    pixels = Path(texture["rawFile"]).read_bytes()
    if len(pixels) != width * height * 4:
        raise ValueError("Raw BGRA8 mip length does not match texture dimensions")
    rgba = np.asarray(Image.frombytes("RGBA", (width, height), pixels, "raw", "BGRA"),
                      dtype=np.float32) / 255.0
    scale_x, scale_y = binding["coordinateScale"]
    bias_x, bias_y = binding["coordinateBias"]
    x0 = round(bias_x * width)
    x1 = round((bias_x + scale_x) * width)
    y0 = round(bias_y * height * 0.5)
    y1 = round((bias_y + scale_y) * height * 0.5)
    if not 0 <= x0 < x1 <= width or not 0 <= y0 < y1 <= height // 2:
        raise ValueError("Invalid HQ atlas rectangle")
    lm0 = rgba[y0:y1, x0:x1]
    lm1 = rgba[y0 + height // 2:y1 + height // 2, x0:x1]
    first_scale = np.asarray(binding["coefficientScales"][0], dtype=np.float32)
    first_add = np.asarray(binding["coefficientAdds"][0], dtype=np.float32)
    second_scale = np.asarray(binding["coefficientScales"][1], dtype=np.float32)
    second_add = np.asarray(binding["coefficientAdds"][1], dtype=np.float32)

    # UE 5.4 Engine/Shaders/Private/LightmapCommon.ush, GetLightMapColorHQ.
    log_l = (lm0[..., 3] + (lm1[..., 3] - 0.5) / 255.0) * first_scale[3] + first_add[3]
    uvw = lm0[..., :3] ** 2 * first_scale[:3] + first_add[:3]
    luminance = np.exp2(log_l) - 0.01858136
    sh = lm1 * second_scale + second_add
    direction = np.maximum(0.0, sh[..., :3] @ normal[[1, 2, 0]] + sh[..., 3])
    irradiance = np.maximum(0.0, luminance[..., None] * direction[..., None] * uvw)
    statistics = {
        "actor": binding["actor"],
        "atlasRectangle": [x0, y0, x1, y1],
        "constantWorldNormal": normal.tolist(),
        "linearIrradianceMedian": np.median(irradiance.reshape(-1, 3), axis=0).tolist(),
        "linearIrradianceMaximum": np.max(irradiance.reshape(-1, 3), axis=0).tolist(),
    }
    return irradiance, statistics


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--binding-index", type=int, default=0)
    parser.add_argument("--normal", type=float, nargs=3, default=(0.0, 0.0, 1.0))
    args = parser.parse_args()
    bindings = json.loads(args.report.read_text(encoding="utf-8"))["bindings"]
    normal = np.asarray(args.normal, dtype=np.float32)
    normal /= np.linalg.norm(normal)
    irradiance, statistics = decode(bindings[args.binding_index], normal)
    mapped = irradiance / (1.0 + irradiance)
    srgb = np.where(mapped <= 0.0031308, mapped * 12.92,
                    1.055 * np.power(mapped, 1 / 2.4) - 0.055)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.uint8(np.clip(np.round(srgb * 255), 0, 255)), "RGB").save(args.output)
    print(json.dumps(statistics))


if __name__ == "__main__":
    main()
