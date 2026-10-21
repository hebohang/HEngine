import json
from pathlib import Path
import sys
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Engine/Tools"))
from StubGen.Src.GenerateStubs import generate
from StubGen.Src.Pybind11Stubgen import FunctionSignature, StubsGenerator, normalize_cpp_types
from CheckPythonStubs import check


class StubTests(unittest.TestCase):
    def test_dynamically_attached_anonymous_cpp_function(self):
        def dynamic(self, seconds): pass
        dynamic.__name__ = ""
        dynamic.__doc__ = "(self: HEngine.AnimatorComponent, seconds: float) -> None"
        signatures = StubsGenerator.function_signatures_from_docstring("Step", dynamic, "HEngine")
        self.assertEqual(len(signatures), 1)
        self.assertEqual(signatures[0].name, "Step")
        self.assertEqual(signatures[0].args, "self: AnimatorComponent, seconds: float")
    def test_cpp_signature_preserves_arguments(self):
        self.assertEqual(normalize_cpp_types("handle: int, value: handle"),
                         "handle: int, value: object")
        self.assertEqual(normalize_cpp_types("handle"), "object")
        signature = FunctionSignature("Play", "handle: int, value: HEngine::Unbound", "bool")
        self.assertEqual(signature.args, "handle: int, value: typing.Any")
        self.assertEqual(signature.rtype, "bool")
        self.assertEqual(normalize_cpp_types('label: str = "HEngine::Unbound"'),
                         'label: str = "HEngine::Unbound"')

    def test_cpp_property_is_valid(self):
        def getter(self): pass
        getter.__doc__ = "(self: HEngine.Camera) -> HEngine::SceneCamera"
        signature = StubsGenerator.property_signature_from_docstring(property(getter), "HEngine")
        self.assertEqual(signature.rtype, "typing.Any")

    def test_runtime_modules_coverage_noop_and_freshness(self):
        module = types.ModuleType("StubFixture")
        child = types.ModuleType("StubFixture.VFX")
        def play(path): pass
        play.__doc__ = "Play(path: str) -> int"
        child.Play = play
        child.Col_Button = 21
        child.WindowFlags_None = 0
        module.VFX = child
        sys.modules[module.__name__] = module
        sys.modules[child.__name__] = child
        try:
            with tempfile.TemporaryDirectory() as folder:
                output = Path(folder)
                self.assertEqual(generate(output, ROOT, module)["modules"], 2)
                self.assertEqual(check(output), [])
                times = {p: p.stat().st_mtime_ns for p in output.rglob("*") if p.is_file()}
                result = generate(output, ROOT, module)
                self.assertEqual((result["changed"], result["deleted"]), (0, 0))
                self.assertEqual(times, {p: p.stat().st_mtime_ns for p in times})
                manifest = json.loads((output / "stub-manifest.json").read_text())
                manifest["sources"] = {}
                (output / "stub-manifest.json").write_text(json.dumps(manifest))
                self.assertTrue(any("sources changed" in error for error in check(output)))
                (output / "StubFixture/VFX/__init__.pyi").write_text("def bad(::): ...")
                self.assertTrue(any("invalid syntax" in error for error in check(output)))
        finally:
            del sys.modules[module.__name__]
            del sys.modules[child.__name__]

    def test_bound_cpp_type_resolves(self):
        module = types.ModuleType("HEngine")
        module.Bound = type("Bound", (), {})
        previous = sys.modules.get("HEngine")
        sys.modules["HEngine"] = module
        try:
            self.assertEqual(normalize_cpp_types("HEngine::Bound"), "HEngine.Bound")
        finally:
            if previous is None:
                del sys.modules["HEngine"]
            else:
                sys.modules["HEngine"] = previous


if __name__ == "__main__":
    unittest.main()
