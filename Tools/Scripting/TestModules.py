import json
from pathlib import Path
import tempfile
import unittest
import contextlib
import io
import Modules


class Contracts(unittest.TestCase):
    def setUp(self):
        work = Path(__file__).resolve().parents[2] / "out/agent/scripting/module-tests"
        work.mkdir(parents=True, exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix="中文 modules ", dir=work)
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.module("base")
        self.module("leaf", references=["base"])
        self.module("other")

    def module(self, name, **values):
        directory = self.root / "Scripts" / name
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "Code.cs").write_text(f"namespace Fixture; public class {name.title()} {{ }}", encoding="utf-8")
        definition = {"schema": 1, "id": name, "assembly": "Fixture." + name.title(), "sources": ["Scripts/" + name], **values}
        (directory / "module.hemodule").write_text(json.dumps(definition), encoding="utf-8")

    def keys(self, platform="Windows", categories={"Runtime"}, **kwargs):
        return {entry["module"]["id"]: entry["identity"] for entry in Modules.plan(self.root, platform, categories, "Release", kwargs.get("sdk", "10.0.302"), kwargs.get("api", "api1"))}

    def test_leaf_and_public_invalidation(self):
        before = self.keys()
        (self.root / "Scripts/leaf/Code.cs").write_text("// changed leaf")
        leaf = self.keys()
        self.assertNotEqual(before["leaf"], leaf["leaf"])
        self.assertEqual(before["base"], leaf["base"])
        self.assertEqual(before["other"], leaf["other"])
        (self.root / "Scripts/base/Code.cs").write_text("// changed public")
        public = self.keys()
        self.assertNotEqual(leaf["base"], public["base"])
        self.assertNotEqual(leaf["leaf"], public["leaf"])
        self.assertEqual(leaf["other"], public["other"])

    def test_editor_isolation(self):
        self.module("editor", category="Editor", references=["base"])
        before = self.keys()
        (self.root / "Scripts/editor/Code.cs").write_text("// editor only")
        self.assertEqual(before, self.keys())
        self.assertNotIn("editor", before)

    def test_platform_filter(self):
        self.module("desktop", includePlatforms=["Windows"])
        self.assertIn("desktop", self.keys())
        self.assertNotIn("desktop", self.keys("Web"))

    def test_excluded_reference(self):
        self.module("base", includePlatforms=["Windows"])
        with self.assertRaisesRegex(ValueError, "excludes dependency"):
            self.keys("WeChat")

    def test_cycle(self):
        self.module("base", references=["leaf"])
        with self.assertRaisesRegex(ValueError, "cycle"):
            self.keys()

    def test_missing_reference(self):
        self.module("leaf", references=["missing"])
        with self.assertRaisesRegex(ValueError, "Missing module"):
            self.keys()

    def test_duplicate_assembly(self):
        self.module("duplicate", assembly="Fixture.Base")
        with self.assertRaisesRegex(ValueError, "assembly name"):
            self.keys()

    def test_runtime_editor_leak(self):
        self.module("base", category="Editor")
        with self.assertRaisesRegex(ValueError, "leaks Editor"):
            self.keys(categories={"Runtime", "Editor"})

    def test_identity_inputs(self):
        before = self.keys()
        self.assertNotEqual(before, self.keys(sdk="different"))
        self.assertNotEqual(before, self.keys(api="different"))
        self.assertNotEqual(before, self.keys("Web"))

    def test_real_compiler_cache(self):
        engine = Path(__file__).resolve().parents[2]
        cache = self.root / "cache"
        run = lambda: Modules.build(self.root, engine, cache, "Windows", "Release", {"Runtime"}, "dotnet")
        first = run()
        self.assertEqual(3, sum(not value["cached"] for value in first["modules"]))
        self.assertTrue(all(value["cached"] for value in run()["modules"]))
        with (self.root / "Scripts/leaf/Code.cs").open("a") as stream:
            stream.write("\n// leaf changed\n")
        result = run()
        self.assertEqual(["leaf"], [value["id"] for value in result["modules"] if not value["cached"]])
        dll = Path(result["modules"][0]["dll"])
        dll.write_bytes(b"corrupted cached assembly")
        self.assertFalse(run()["modules"][0]["cached"])
        (self.root / "Scripts/leaf/Code.cs").write_text("#error expected module diagnostic\n", encoding="utf-8")
        output = io.StringIO()
        with contextlib.redirect_stdout(output), self.assertRaisesRegex(ValueError, "leaf compilation failed"):
            run()
        diagnostics = [json.loads(line.split("=", 1)[1]) for line in output.getvalue().splitlines() if line.startswith("HE_SCRIPT_MODULE_DIAGNOSTIC=")]
        self.assertTrue(any(item["module"] == "leaf" and item["code"] == "CS1029" and item["line"] == 1 and Path(item["log"]).is_file() for item in diagnostics))


if __name__ == "__main__":
    unittest.main(verbosity=2)
