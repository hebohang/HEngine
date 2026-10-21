"""Read-only stub syntax, export coverage and source/output freshness check."""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Engine/Tools"))
from StubGen.Src.GenerateStubs import declared_symbols, digest, source_hashes


def check(output, repository=ROOT):
    errors = []
    for path in sorted(output.rglob("*.pyi")):
        try:
            declared_symbols(path.read_text(encoding="utf-8"))
        except SyntaxError as error:
            errors.append("{}:{}: {}".format(path, error.lineno, error.msg))
    manifest_path = output / "stub-manifest.json"
    if not manifest_path.exists():
        return errors + ["Missing stub-manifest.json; regenerate in the current HEngine runtime."]
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("sources") != source_hashes(repository):
        errors.append("Binding/generator sources changed; regenerate stubs.")
    for name, expected in manifest.get("files", {}).items():
        path = output / name
        if not path.is_file() or digest(path) != expected:
            errors.append("Missing or changed generated file: " + name)
            continue
        module = name.replace("/__init__.pyi", "").replace("/", ".")
        missing = set(manifest["exports"][module]) - declared_symbols(path.read_text(encoding="utf-8"))
        if missing:
            errors.append("Missing exports in {}: {}".format(module, sorted(missing)))
    return errors


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "Engine/Tools/HEngineStub")
    args = parser.parse_args()
    errors = check(args.output)
    print(json.dumps({"status": "FAIL" if errors else "PASS", "errors": errors}, indent=2))
    raise SystemExit(bool(errors))
