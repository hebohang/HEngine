import json
import tempfile
import unittest
from pathlib import Path
from SharedContent import validate


class SharedContentTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[2] / "out/agent/shared-content-tests"
        root.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=root)
        self.project = Path(self.temp.name)
        self.write("Project.heproject", {"GameConfig": "Config/Game.json"})
        self.write("Config/Game.json", {"Game": {"DefaultScene": "Assets/Scene.he"}})
        self.write("Config/SharedContent.json", {"schema": 1, "portableComponentFallbacks": {"CameraComponent": "project camera adapter"}})
        self.write("Config/BuildProfiles.json", {"profiles": [{"id": "windows", "platform": "Windows"}, {"id": "web", "platform": "Web"}]})
        self.scene = {"root": {"TransformComponent": {"Translation": [4, 5, 6]}}, "child": {"TransformComponent": {"ParentUuid": "root"}, "CameraComponent": {}}}
        self.write("Assets/Scene.he", self.scene)

    def tearDown(self):
        self.temp.cleanup()

    def write(self, name, data):
        file = self.project / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(json.dumps(data), encoding="utf-8")

    def test_shared_scene_and_explicit_fallback(self):
        result = validate(self.project)
        self.assertEqual(result["scenePath"], "Assets/Scene.he")
        self.assertEqual(result["portableFallbacks"], {"CameraComponent": "project camera adapter"})

    def test_scene_divergence(self):
        self.write("Config/Other.json", {"Game": {"DefaultScene": "Assets/Other.he"}})
        self.write("Config/BuildProfiles.json", {"profiles": [{"id": "windows", "platform": "Windows", "gameConfig": "Config/Other.json"}]})
        with self.assertRaisesRegex(ValueError, "startup scene differs"):
            validate(self.project)

    def test_unknown_component(self):
        self.scene["child"]["NewUnsupportedComponent"] = {}
        self.write("Assets/Scene.he", self.scene)
        with self.assertRaisesRegex(ValueError, "child: NewUnsupportedComponent"):
            validate(self.project)

    def test_native_learning_variant_does_not_bypass_web_scene_contract(self):
        self.write("Config/Python.json", {"Game": {"DefaultScene": "Assets/Python.he"}})
        self.write("Assets/Python.he", {"game": {"PythonScriptComponent": {}}})
        self.write("Config/SharedContent.json", {"schema": 1, "nativeSceneVariants": ["Assets/Python.he"],
            "portableComponentFallbacks": {"CameraComponent": "project camera adapter"}})
        profile = {"id": "python", "platform": "Windows", "gameConfig": "Config/Python.json"}
        self.write("Config/BuildProfiles.json", {"profiles": [profile]})
        self.assertEqual(validate(self.project)["scenePath"], "Assets/Scene.he")
        profile["platform"] = "Web"
        self.write("Config/BuildProfiles.json", {"profiles": [profile]})
        with self.assertRaisesRegex(ValueError, "startup scene differs"):
            validate(self.project)

    def test_unknown_script(self):
        self.scene["child"]["ScriptComponent"] = {"Backend": "he.script.csharp", "Type": "custom.game"}
        self.write("Assets/Scene.he", self.scene)
        with self.assertRaisesRegex(ValueError, "script he.script.csharp:custom.game"):
            validate(self.project)

    def test_cycle_and_missing_parent(self):
        for parent in ["child", "missing"]:
            self.scene["root"]["TransformComponent"]["ParentUuid"] = parent
            self.write("Assets/Scene.he", self.scene)
            with self.assertRaisesRegex(ValueError, "hierarchy cycle"):
                validate(self.project)


if __name__ == "__main__":
    unittest.main()
