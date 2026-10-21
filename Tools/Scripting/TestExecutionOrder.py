"""Exercise the real registry resolver, including merged module constraints."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

GENERATOR = str(Path(sys.argv.pop(1)).resolve())


class ExecutionOrderTests(unittest.TestCase):
    def resolve(self, entries, valid=True):
        with tempfile.TemporaryDirectory(prefix="HE order 中文 ") as folder:
            root = Path(folder)
            original = json.dumps({"version": 1, "types": entries})
            (root / "GameScripts.metadata.json").write_text(original, encoding="utf-8")
            source = "public const string Metadata = " + json.dumps(original) + ";\n"
            (root / "GameScripts.Registry.g.cs").write_text(source, encoding="utf-8")
            result = subprocess.run(["dotnet", GENERATOR, "--resolve-order", folder], capture_output=True, text=True)
            if not valid:
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual((root / "GameScripts.metadata.json").read_text(encoding="utf-8"), original)
                return
            self.assertEqual(result.returncode, 0, result.stderr)
            metadata = (root / "GameScripts.metadata.json").read_text(encoding="utf-8")
            embedded = (root / "GameScripts.Registry.g.cs").read_text(encoding="utf-8").split(" = ", 1)[1].rstrip(";\n")
            self.assertEqual(json.loads(embedded), metadata)
            return [entry["id"] for entry in sorted(json.loads(metadata)["types"], key=lambda entry: entry["executionOrder"])]

    def test_defaults_stable(self):
        self.assertEqual(self.resolve([{"id": "z"}, {"id": "b", "executionOrder": -10}, {"id": "a"}]), ["b", "a", "z"])

    def test_cross_module_dependencies(self):
        entries = [{"id": "consumer", "moduleId": "a", "executionOrder": -100, "executeAfter": ["provider"]},
                   {"id": "provider", "moduleId": "b"}, {"id": "last", "executeAfter": ["consumer"]}]
        self.assertEqual(self.resolve(entries), ["provider", "consumer", "last"])

    def test_invalid_graphs(self):
        for entries in ([{"id": "a", "executeBefore": ["missing"]}],
                        [{"id": "a", "executeAfter": ["a"]}],
                        [{"id": "a", "executeBefore": ["b", "b"]}, {"id": "b"}],
                        [{"id": "a", "executeBefore": ["b"]}, {"id": "b", "executeBefore": ["a"]}]):
            with self.subTest(entries=entries):
                self.resolve(entries, False)


if __name__ == "__main__":
    unittest.main()
