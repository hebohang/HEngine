"""Validate the same composition schema used by runtime, including dependencies."""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT / "Project"))
from Scripts.PresentationSchema import validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("composition",type=Path)
    args=parser.parse_args()
    try:
        resource=json.loads(args.composition.read_text(encoding="utf-8"))
        dependencies=validate(resource)
        missing=[path for path in dependencies if not (ROOT/"Project"/path).is_file()]
        if missing:
            raise ValueError("Missing dependencies: " + ", ".join(missing))
        print(json.dumps({"status":"schema_valid", "dependencies":dependencies,"runtime_validation":"not_verified"},indent=2))
        return 0
    except (ValueError,OSError) as error:
        print(json.dumps({"status":"FAIL","error":str(error)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
