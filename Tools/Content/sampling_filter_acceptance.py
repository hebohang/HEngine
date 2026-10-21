"""Independent min/mag fixture; real Content import and owned pinned raster capture.

--prepare-only generates source and projected footprint evidence without binaries.
Runtime mode requires current importer/Game hashes. Images remain NOT_REVIEWED.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import uuid

import content_sync
from generate_sampling_fixture import encode, png
from sampling_acceptance import ROOT, PROJECT, Storage, CleanupFailure, capture, digest, read, require, source_for_run, write_json

FILTERS = [9728, 9729, 9984, 9985, 9986, 9987]


def generate(directory, namespace):
    document = source_for_run(directory, "initial", namespace)
    data = bytearray((directory / "sampling.bin").read_bytes())
    document["nodes"] = document["nodes"][:8]
    document["meshes"] = document["meshes"][:8]
    document["materials"] = document["materials"][:8]
    document["scenes"][0]["nodes"] = list(range(8))
    document["images"] = [dict(uri="frequency256.png"), dict(uri="frequency8.png")]
    document["textures"], document["samplers"] = [], []
    # Authored frequency bands (1, 2, 4, 8 texels), opaque and grayscale.
    # No color management ambiguity from an external/proprietary texture.
    for size in (256, 8):
        pixels = []
        for y in range(size):
            for x in range(size):
                period = 1 << min(3, x * 4 // size)
                value = 255 if ((x // period + y // period) & 1) else 0
                pixels.extend((value, value, value, 255))
        (directory / ("frequency" + str(size) + ".png")).write_bytes(png(size, size, pixels))
    panels = []
    focal = 360 / math.tan(1.0471980571746826 / 2)
    for index, (node, mesh, material) in enumerate(zip(document["nodes"], document["meshes"], document["materials"])):
        minimum = FILTERS[index] if index < 6 else 9987
        maximum = 9728 if index == 6 else 9729
        name = "min_" + str(minimum) if index < 6 else "mag_" + str(maximum)
        x, y = (index % 3) * 1.4, (index // 3) * 1.4
        node.update(name=name, translation=[x, y, 0])
        mesh["name"] = name
        attributes = mesh["primitives"][0]["attributes"]
        for semantic, values in (("TEXCOORD_0", [0,0,1,0,1,1,0,1]), ("COLOR_0", [1]*16)):
            accessor = document["accessors"][attributes[semantic]]
            view = document["bufferViews"][accessor["bufferView"]]
            offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
            struct.pack_into("<" + "f" * len(values), data, offset, *values)
        material.clear()
        material.update(name=name, extras=dict(he_schema=1, he_id=str(uuid.uuid5(namespace, "filter-material/" + name))),
                        doubleSided=True, pbrMetallicRoughness=dict(baseColorFactor=[1,1,1,1],
                        baseColorTexture=dict(index=index, texCoord=0), metallicFactor=0, roughnessFactor=1))
        document["samplers"].append(dict(minFilter=minimum, magFilter=maximum, wrapS=10497, wrapT=10497))
        document["textures"].append(dict(source=0 if index < 6 else 1, sampler=index))
        pixel_width = focal / 8
        size = 256 if index < 6 else 8
        bbox = [640+(x-2.5)*pixel_width, 360-(y+1-1.9)*pixel_width,
                640+(x+1-2.5)*pixel_width, 360-(y-1.9)*pixel_width]
        panels.append(dict(name=name, minFilter=minimum, magFilter=maximum, textureSize=size,
                           projectedBbox=bbox, projectedPixelsPerEdge=pixel_width,
                           texelsPerPixel=size/pixel_width, lambdaEstimate=math.log2(size/pixel_width)))
    (directory / "sampling.bin").write_bytes(data)
    (directory / "sampling.gltf").write_bytes(encode(document))
    manifest = dict(schemaVersion=1, namespace=str(namespace), panels=panels,
        camera=dict(position=[2.5,1.9,8], rotation=[0,0,0], resolution=[1280,720], verticalFov=1.0471980571746826),
        sourceSha256={name:digest(directory/name) for name in ("sampling.gltf","sampling.bin","frequency256.png","frequency8.png")},
        footprintScope="Geometric prediction at the capture camera; not a shader LOD readback",
        pixelCriteria="Interior ROI mean/stddev/neighbor variation and pair differences are diagnostic, not visual PASS")
    write_json(directory / "manifest.json", manifest)
    return document, manifest


def pixel_metrics(path, manifest):
    from PIL import Image, ImageChops, ImageStat
    with Image.open(path) as opened:
        image = opened.convert("RGB")
    require(image.size == (1280,720), "Unexpected capture dimensions invalidate projected ROIs")
    rows, crops = [], {}
    for panel in manifest["panels"]:
        left, top, right, bottom = panel["projectedBbox"]
        # Central region excludes geometry edges and raster coverage differences.
        box = (math.ceil(left+8), math.ceil(top+8), math.floor(right-8), math.floor(bottom-8))
        crop = image.crop(box)
        stats = ImageStat.Stat(crop)
        horizontal = ImageChops.difference(crop.crop((1,0,crop.width,crop.height)), crop.crop((0,0,crop.width-1,crop.height)))
        rows.append(dict(name=panel["name"], roi=box, mean=stats.mean, stddev=stats.stddev,
                         horizontalMeanAbsoluteDifference=ImageStat.Stat(horizontal).mean))
        crops[panel["name"]] = crop
    pairs = []
    for first, second in (("min_9728","min_9984"),("min_9729","min_9987"),
                          ("min_9984","min_9986"),("min_9985","min_9987"),("mag_9728","mag_9729")):
        a, b = crops[first], crops[second]
        width, height = min(a.width,b.width), min(a.height,b.height)
        delta = ImageChops.difference(a.crop((0,0,width,height)),b.crop((0,0,width,height)))
        pairs.append(dict(first=first,second=second,meanAbsoluteDifference=ImageStat.Stat(delta).mean,
                          limitation="Different panel subpixel phases/lighting; no exact pixel equality required"))
    return dict(panels=rows,pairs=pairs,visualStatus="NOT_REVIEWED",
                warning="Uniform/background-only crops cannot establish correct filtering; inspect silhouettes and frequency bands")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--backend", choices=["DX12","Vulkan"])
    parser.add_argument("--importer", type=Path)
    parser.add_argument("--importer-sha256")
    parser.add_argument("--existing-build-sha256")
    args = parser.parse_args()
    output = args.output.resolve()
    require((ROOT/"out/agent/rendering-evidence/vfx-authoring").resolve() in output.parents, "Output outside evidence whitelist")
    if not args.prepare_only:
        require(all((args.backend,args.importer,args.importer_sha256,args.existing_build_sha256)), "Runtime mode requires backend and both pinned binaries")
        from PIL import Image  # Check dependency before any process starts.
        args.importer = args.importer.resolve()
        require(digest(args.importer)==args.importer_sha256.lower(), "Importer hash mismatch")
        require(digest(ROOT/"out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe")==args.existing_build_sha256.lower(), "Game hash mismatch")
    output.mkdir(parents=True, exist_ok=False)
    namespace = uuid.uuid4()
    report = dict(status="RUNNING",visualStatus="NOT_REVIEWED",scope="Raster min/mag only; no RT or performance acceptance",
                  driverSha256=digest(__file__),backend=args.backend)
    with Storage(ROOT).workflow_scope(str(PROJECT),"sampling-filter-acceptance",[output]) as run:
        try:
            document, manifest = generate(output/"source",namespace)
            report["inputs"] = manifest
            require(all(p["lambdaEstimate"]>1 for p in manifest["panels"][:6]), "Min panels do not predict minification")
            require(all(p["lambdaEstimate"]<0 for p in manifest["panels"][6:]), "Mag controls do not predict magnification")
            if args.prepare_only:
                report["status"] = "PREPARED_NOT_RUN"
            else:
                imported = output/"import"; imported.mkdir()
                arguments = argparse.Namespace(action="sync",source=output/"source/sampling.gltf",project=PROJECT,
                    importer=args.importer,blender=None,initialize_ids=False,verify="none",timeout=120,fault=None)
                summary = content_sync.synchronize(arguments,imported)
                report["import"] = summary
                require(summary["metrics"]["entities"]==8, "Expected eight imported filter panels")
                state = read(PROJECT/".he-content"/document["scenes"][0]["extras"]["he_id"]/"state.json")
                actual = []
                for name, expected in state["artifacts"].items():
                    require(digest(PROJECT/name)==expected,"Published resource hash mismatch")
                    if name.endswith(".hmat"):
                        actual.append(read(PROJECT/name)["Textures"]["g_AlbedoMap"]["Sampling"])
                require({s["minFilter"] for s in actual}==set(FILTERS),"Published min modes missing")
                require({s["magFilter"] for s in actual}=={9728,9729},"Published mag modes missing")
                write_json(imported/"state.json",state)
                report["capture"] = capture(PROJECT/summary["scene"],output/"gpu",args,namespace)
                report["pixels"] = pixel_metrics(output/"gpu/sampling.png",manifest)
                require(all(digest(output/"source"/name)==expected for name,expected in manifest["sourceSha256"].items()),
                        "Source inputs changed during capture")
                require(digest(args.importer)==args.importer_sha256.lower(),"Importer changed")
                require(digest(ROOT/"out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe")==args.existing_build_sha256.lower(),"Game changed")
                report.update(status="PASS",importerSha256=args.importer_sha256,executableSha256=args.existing_build_sha256)
            run["status"]="success"
        except Exception as error:
            report.update(status="FAIL",error=str(error)[:2400])
            run["status"]="unknown" if isinstance(error,CleanupFailure) else "failed"
            raise
        finally:
            write_json(output/"report.json",report)
    print(json.dumps(dict(status=report["status"],visualStatus=report["visualStatus"],report=str(output/"report.json"))))


if __name__=="__main__":
    main()
