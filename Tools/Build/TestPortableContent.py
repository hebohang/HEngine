"""Small independent fixture for portable asset traversal and license copying."""

import hashlib
import json
import tempfile
from pathlib import Path

from PortableContent import copy_licenses, export_assets


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    project, output = root / "project", root / "output"
    (project / "Assets").mkdir(parents=True)
    (project / "Legal").mkdir()
    (project / "Assets/scene.he").write_text(json.dumps({"mesh": "Assets/mesh.hmesh"}), encoding="utf-8")
    (project / "Assets/mesh.hmesh").write_text(json.dumps({"image": "Assets/image.png"}), encoding="utf-8")
    (project / "Assets/image.png").write_bytes(b"image fixture")
    (project / "Legal/NOTICE.txt").write_text("license fixture", encoding="utf-8")
    copied = export_assets(project, output, {}, {"Assets/scene.he"})
    assert set(copied) == {"Assets/scene.he", "Assets/mesh.hmesh", "Assets/image.png"}
    assert copied["Assets/image.png"]["sha256"] == hashlib.sha256(b"image fixture").hexdigest()
    copy_licenses(project, output, {"Licenses": [{"Source": "Legal/NOTICE.txt", "Output": "NOTICE.txt"}]}, copied)
    assert (output / "Licenses/NOTICE.txt").read_text() == "license fixture"
    for unsafe in ("Assets/../../outside.he", "C:/outside.he"):
        try:
            export_assets(project, output, {}, {unsafe})
        except (ValueError, FileNotFoundError):
            pass
        else:
            raise AssertionError(f"unsafe asset path accepted: {unsafe}")
print("PortableContent PASS")
