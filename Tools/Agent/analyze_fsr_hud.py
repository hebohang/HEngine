"""Check completed Test-FsrHud runs; pixel comparisons exclude translucent edges."""
import argparse
import json
from pathlib import Path
import re

import numpy as np
from PIL import Image


def read(root, name):
    return json.loads((root / name).read_text(encoding="utf-8-sig"))


def compare(root, first, second, height):
    native = read(root, first + "-ui.json")
    fsr = read(root, second + "-ui.json")
    assert native == fsr, "HUD layout changed with internal render resolution"
    a = np.array(Image.open(root / (first + ".png")).convert("RGB"))
    b = np.array(Image.open(root / (second + ".png")).convert("RGB"))
    assert a.shape == b.shape == (height, height * 16 // 9, 3)
    report = []
    for name in ("objective", "weapon", "ammo", "health", "crosshair", "healthbar"):
        x, y, w, h = map(round, fsr[name]["rect"])
        assert x >= 0 and y >= 0 and w > 2 and h > 2 and x + w <= a.shape[1] and y + h <= height
        aa = a[y:y+h, x:x+w]
        bb = b[y:y+h, x:x+w]
        color = (225, 237, 239) if name == "healthbar" else (237, 241, 244)
        opaque = np.all(aa == color, axis=2)
        interior = opaque.copy()
        for shift, axis in ((1, 0), (-1, 0), (1, 1), (-1, 1)):
            interior &= np.roll(opaque, shift, axis)
        interior[[0, -1], :] = False
        interior[:, [0, -1]] = False
        count = int(interior.sum())
        assert count > 0, (name, "No opaque HUD pixels")
        changed = int(np.any(aa[interior] != bb[interior], axis=1).sum())
        assert changed == 0, (name, changed, count)
        report.append(dict(outputHeight=height, element=name, opaquePixels=count,
                           changedPixels=changed, comparison=[first, second]))
    return report


def analyze(root):
    result = read(root, "result.json")
    assert result["status"] == "success" and result["shutdownVerified"], "Run did not complete with verified shutdown"
    stop = (root / "stop.log").read_bytes()
    # PowerShell redirection may use UTF-16; only the ASCII stop marker is needed.
    assert ("agent session: STOPPED pid=" + str(result["pid"])).encode() in stop.replace(b"\0", b"")
    assert (root / "stderr.log").stat().st_size == 0, "Nonempty runtime stderr"
    log = (root / "runtime.log").read_bytes()
    assert not re.search(rb"Validation Error|ScriptException", log), "Runtime/shutdown validation error"
    rejected = result["native4KRejected"]
    samples = read(root, "receipts.json")
    assert len(samples) == 4
    # SimulationClock frame numbers also advance on zero-delta render-only frames.
    # The actual paused control state, step count and elapsed simulation time matter.
    for sample in samples:
        control = sample["runtimeControl"]
        assert control["paused"] and control["controlMode"] == "paused"
        assert control["pendingSteps"] == 0 and not control["stepInProgress"]
    assert len({s["runtimeControl"]["completedSteps"] for s in samples}) == 1
    times = [s["simulationTimeSeconds"] for s in samples]
    assert max(times) - min(times) < 1e-9, "Gameplay advanced between HUD comparisons"
    for height in (1440, 2160):
        raster = read(root, f"{height}-fsr.world.json")["recordedRaster"]
        upscaling = raster["upscaling"]
        assert upscaling["active"] and not upscaling["failure"] and upscaling["provider"] == "3.1.4"
        assert not raster["taaActive"]
        assert upscaling["renderSize"] == [height * 16 // 18, height // 2]
        assert upscaling["outputSize"] == [height * 16 // 9, height]
    comparisons = compare(root, "1440-native", "1440-fsr", 1440)
    for height in ([1440] if rejected else [1440, 2160]):
        raster = read(root, f"{height}-native.world.json")["recordedRaster"]
        assert raster["valid"] and not raster["upscaling"]["active"] and raster["taaActive"]
        assert [raster["width"], raster["height"]] == [height * 16 // 9, height]
    native_apply = read(root, "native4k-apply.json")
    assert native_apply["success"] != rejected
    if rejected:
        snapshots = read(root, "rejection-settings.json")
        before, after = snapshots["before"], snapshots["after"]
        assert before["revision"] == after["revision"] and before["hash"] == after["hash"]
        assert after["effective"]["Rendering.Upscaling.Mode"] == "FSR3.1"
        raster = read(root, "2160-after-rejection.world.json")["recordedRaster"]
        assert raster["upscaling"]["active"] and not raster["upscaling"]["failure"] and not raster["taaActive"]
        comparisons += compare(root, "2160-fsr", "2160-after-rejection", 2160)
    else:
        comparisons += compare(root, "2160-native", "2160-fsr", 2160)
    names = [p["name"] for p in read(root, "passes.json") if not p["culled"]]
    order = ["FSR31Upscale", "PostProcessComposite", "ToneMapping", "RuntimeUiRender", "UiComposite", "Present"]
    positions = [names.index(name) for name in order]
    assert positions == sorted(positions) and "SceneTAA" not in names
    for height in (1440, 2160):
        im = Image.open(root / f"{height}-fsr.png").convert("RGB")
        im.thumbnail((1280, 1280))
        im.save(root / f"{height}-preview.jpg", quality=85)
    report = dict(status="PASS_FOR_RECORDED_SCOPE", api=result["api"], native1440Compared=True,
                  native2160Compared=not rejected, rejectionRollbackVerified=rejected,
                  cleanShutdownVerified=True, passOrder=order, elements=comparisons,
                  limits="Opaque HUD interiors and layout only; no equivalence claim for antialiased edges. Rejected native 4K is rollback evidence, not a native/FSR 4K comparison.")
    (root / "analysis.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"directory": str(root), "status": report["status"], "native2160Compared": not rejected,
                      "rejectionRollbackVerified": rejected, "opaquePixels": sum(r["opaquePixels"] for r in comparisons)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="+", type=Path)
    for directory in parser.parse_args().directories:
        analyze(directory)
