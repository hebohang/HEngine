"""Small reusable Effekseer 1.80 XML authoring helpers.

These operate on editable node XML before compiling with the pinned official
tool. The texture named by a node must already exist beside the .efkproj.
"""

from __future__ import annotations

import xml.etree.ElementTree as ET


def set_infinite_emission(node: ET.Element, interval_frames: int) -> None:
    if not 1 <= interval_frames <= 600:
        raise ValueError("Emission interval must be within 1..600 frames")
    common = node.find("CommonValues")
    if common is None:
        raise ValueError("Effekseer node has no CommonValues")
    infinite = common.find("MaxGeneration/Infinite")
    if infinite is None:
        raise ValueError("Effekseer node has no MaxGeneration/Infinite")
    infinite.text = "True"
    generation = common.find("Generation")
    if generation is None:
        generation = ET.SubElement(common, "Generation")
    for field in ("GenerationTime", "GenerationTimeOffset"):
        timing = common.find(field)
        if timing is not None:
            common.remove(timing)
            generation.append(timing)
    interval = generation.find("GenerationTime")
    if interval is None:
        raise ValueError("Effekseer node has no generation interval")
    for field in ("Center", "Max", "Min"):
        value = interval.find(field)
        if value is None:
            raise ValueError("Effekseer generation interval has no " + field)
        value.text = str(interval_frames)


def set_flipbook_uv(
    node: ET.Element, *, columns: int, rows: int, frame_width: int,
    frame_height: int, frame_length: int,
) -> None:
    if not (1 <= columns <= 32 and 1 <= rows <= 32 and
            1 <= frame_width <= 4096 and 1 <= frame_height <= 4096 and
            1 <= frame_length <= 120):
        raise ValueError("Flipbook grid, tile dimensions or frame length are out of bounds")
    renderer = node.find("RendererCommonValues")
    if renderer is None:
        raise ValueError("Effekseer node has no RendererCommonValues")
    uv = renderer.find("UV")
    if uv is None:
        uv = ET.SubElement(renderer, "UV")
    uv.text = "2"  # Effekseer UVAnimationType::Animation
    animation = renderer.find("UVAnimation")
    if animation is None:
        animation = ET.SubElement(renderer, "UVAnimation")
    for child in list(animation):
        animation.remove(child)
    start = ET.SubElement(animation, "Start")
    ET.SubElement(start, "X").text = "0"
    ET.SubElement(start, "Y").text = "0"
    size = ET.SubElement(animation, "Size")
    ET.SubElement(size, "X").text = str(frame_width)
    ET.SubElement(size, "Y").text = str(frame_height)
    ET.SubElement(animation, "FrameLength").text = str(frame_length)
    ET.SubElement(animation, "FrameCountX").text = str(columns)
    ET.SubElement(animation, "FrameCountY").text = str(rows)
    ET.SubElement(animation, "LoopType").text = "1"
