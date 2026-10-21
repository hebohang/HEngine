"""Apply an explicit presentation-only asset tier to a staged export."""
import argparse
import audioop
import hashlib
import io
import json
import wave
from pathlib import Path
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument("directory", type=Path)
parser.add_argument("--texture-limit", type=int, default=512)
args = parser.parse_args()
root = args.directory.resolve()
if args.texture_limit not in (256, 512, 1024, 2048):
    raise ValueError("Unsupported texture limit")
manifest = json.loads((root / "content-manifest.json").read_text(encoding="utf-8"))
before = after = 0
for name, entry in manifest["assets"].items():
    file = (root / name).resolve()
    if not file.is_relative_to(root):
        raise ValueError("Asset path escape")
    payload = file.read_bytes()
    if hashlib.sha256(payload).hexdigest() != entry["sha256"]:
        raise ValueError("Asset changed before optimization: " + name)
    before += len(payload)
    if file.suffix == ".png":
        with Image.open(io.BytesIO(payload)) as image:
            image.thumbnail((args.texture_limit, args.texture_limit), Image.Resampling.LANCZOS)
            output = io.BytesIO()
            image.save(output, format="PNG", optimize=True)
            payload = output.getvalue()
    elif file.suffix == ".wav":
        with wave.open(io.BytesIO(payload), "rb") as source:
            width, channels, rate = source.getsampwidth(), source.getnchannels(), source.getframerate()
            frames = source.readframes(source.getnframes())
            if channels == 2:
                frames = audioop.tomono(frames, width, .5, .5)
                channels = 1
            if rate > 22050:
                frames, _ = audioop.ratecv(frames, width, channels, rate, 22050, None)
                rate = 22050
            output = io.BytesIO()
            with wave.open(output, "wb") as target:
                target.setparams((channels, width, rate, 0, "NONE", "not compressed"))
                target.writeframes(frames)
            payload = output.getvalue()
    elif file.suffix in (".he", ".hmesh", ".hmat", ".hskel", ".hanim", ".hanimctrl", ".hprefab"):
        payload = json.dumps(json.loads(payload), ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    file.write_bytes(payload)
    entry.update(bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest())
    after += len(payload)
manifest["presentationTier"] = {"textureLimit": args.texture_limit, "audioRateLimit": 22050, "audioChannels": "mono"}
(root / "content-manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
print(json.dumps({"originalBytes": before, "exportedBytes": after}))
