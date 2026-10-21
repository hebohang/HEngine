import json
from pathlib import Path
import tempfile
import unittest

from Preflight import inspect


class PreflightTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.write('Project.heproject', {'GameConfig': 'Config/Game.json'})
        self.write('Config/Game.json', {'Game': {'DefaultScene': 'Assets/Main.he'}})
        self.write('Config/Plugins.json', {'he.script.csharp': {'Game': True}})
        self.write('Assets/Main.he', {})

    def tearDown(self): self.temp.cleanup()

    def write(self, name, value):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value), encoding='utf-8')

    def check(self, platform='Windows'):
        return inspect(self.root, platform, engine=self.root / 'no-engine')

    def test_valid_empty_scene(self):
        self.assertTrue(self.check()['success'])

    def test_portable_profile_cannot_claim_native_scene_support(self):
        result = inspect(self.root, 'Web', {'requiredFeatures': ['render.native-scene']})
        self.assertFalse(result['success'])
        self.assertIn('unsupported_feature', {d['code'] for d in result['diagnostics']})

    def test_shared_rules_supported_but_unknown_capability_fails_closed(self):
        for platform in ('Windows', 'Android', 'Web', 'WeChat'):
            if platform == 'Android':
                self.write('Config/Android.json', {'gameConfig': 'Config/Game.json'})
            self.assertTrue(inspect(self.root, platform, {'requiredFeatures': ['game.shared-session']})['success'])
        self.assertFalse(inspect(self.root, 'Windows', {'requiredFeatures': ['invented.feature']})['success'])

    def test_invalid_requirement_schema(self):
        self.assertFalse(inspect(self.root, 'Windows', {'requiredFeatures': 'render.native-scene'})['success'])

    def test_android_rejects_disabled_script_plugin(self):
        self.write('Config/Android.json', {'gameConfig': 'Config/Game.json'})
        self.write('Assets/Main.he', {'id': {'ScriptComponent': {'Backend': 'he.script.python'}}})
        self.assertIn('script_plugin_disabled', {d['code'] for d in self.check('Android')['diagnostics']})

    def test_precompiled_player_requires_explicit_scene_mode(self):
        result = inspect(self.root, 'Windows', precompiled_player=True)
        self.assertFalse(result['success'])
        self.assertTrue(any(d['code'] == 'native_project_requires_source' for d in result['diagnostics']))

    def test_precompiled_player_rejects_native_scripts_even_in_scene_mode(self):
        self.write('Project.heproject', {'RuntimeMode': 'Scene', 'GameConfig': 'Config/Game.json'})
        self.write('Assets/Main.he', {'entity': {'NativeScriptComponent': {'Type': 'CustomGameLogic'}}})
        result = inspect(self.root, 'Windows', precompiled_player=True)
        self.assertFalse(result['success'])
        self.assertTrue(any(d['code'] == 'native_script_requires_source' for d in result['diagnostics']))

    def test_scene_mode_does_not_require_project_cpp(self):
        self.write('Project.heproject', {'RuntimeMode': 'Scene', 'GameConfig': 'Config/Game.json'})
        self.assertTrue(inspect(self.root, 'Windows', precompiled_player=True)['success'])

    def test_android_uses_its_selected_scene_config(self):
        self.write('Config/Android.json', {'gameConfig': 'Config/Mobile.json'})
        self.write('Config/Mobile.json', {'Game': {'DefaultScene': 'Assets/MissingMobile.he'}})
        result = self.check('Android')
        self.assertFalse(result['success'])
        self.assertTrue(any(d['message'] == 'Assets/MissingMobile.he' for d in result['diagnostics']))

    def test_ui_requires_generated_siblings(self):
        self.write('Config/Game.json', {'Cook': {'RootFiles': ['Assets/Hud.hui']}})
        self.write('Assets/Hud.hui', {'Nodes': []})
        result = self.check()
        self.assertFalse(result['success'])
        self.assertEqual({d['pointer'] for d in result['diagnostics']}, {'/GeneratedRmlPath', '/GeneratedRcssPath'})

    def test_missing_reference_has_entity_and_field(self):
        self.write('Assets/Main.he', {'id': {'MeshComponent': {'Path': 'Assets/Missing.hmesh'}}})
        result = self.check()
        self.assertFalse(result['success'])
        self.assertEqual(result['diagnostics'][0]['pointer'], '/id/MeshComponent/Path')

    def test_relative_dependency_and_disabled_plugin(self):
        self.write('Assets/Main.he', {'id': {'MeshComponent': {'Path': 'Assets/Model.hmesh'},
                                           'ScriptComponent': {'Backend': 'he.script.python'}}})
        self.write('Assets/Model.hmesh', {'bufferVec': [{'uri': 'missing.bin'}]})
        result = self.check()
        self.assertEqual({d['code'] for d in result['diagnostics']}, {'missing_resource', 'script_plugin_disabled'})

    def test_portable_physics_requires_explicit_adapter(self):
        self.write('Assets/Main.he', {'id': {'Rigidbody3DComponent': {}}})
        self.assertFalse(self.check('Web')['success'])
        self.write('Config/SharedContent.json', {'portableComponentFallbacks': {'Rigidbody3DComponent': 'Game handles movement in its verified portable adapter.'}})
        self.assertTrue(self.check('Web')['success'])

    def test_cycle_terminates_and_recovery_blocks(self):
        self.write('Assets/Main.he', {'id': {'PrefabInstanceComponent': {'TemplatePath': 'Assets/Cycle.hprefab'}}})
        self.write('Assets/Cycle.hprefab', {'nested': [{'path': 'Assets/Cycle.hprefab'}]})
        self.assertEqual(self.check()['resources'], 2)
        self.write('.he-assets/pending.json', {})
        self.assertFalse(self.check()['success'])

    def test_typed_script_resource_escape(self):
        self.write('Assets/Main.he', {'id': {'ScriptComponent': {'Backend': 'he.script.csharp',
            'Fields': json.dumps({'fields': {'asset': {'type': 'resource', 'value': '../../outside.hmesh'}}})}}})
        result = self.check()
        self.assertFalse(result['success'])
        self.assertIn('path_escape', {d['code'] for d in result['diagnostics']})


if __name__ == '__main__': unittest.main()
