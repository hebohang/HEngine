import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import asset_operations as assets


class AssetOperationsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        # Match plan/apply's canonical paths (Windows TEMP may use an 8.3 alias).
        self.root = Path(self.temp.name).resolve()
        (self.root / 'Assets/Models').mkdir(parents=True)
        (self.root / 'Project.heproject').write_text('{}')
        (self.root / 'Assets/Models/box.hmesh').write_text('{}')
        self.scene = self.root / 'Assets/world.he'
        self.scene.write_text(json.dumps({'actor': {'TagComponent': {'Tag': 'Assets/Models/box.hmesh'},
            'MeshComponent': {'Path': 'Assets/Models/box.hmesh', 'MaterialOverrides': []}}}))

    def plan(self):
        return assets.plan(self.root, 'Assets/Models', 'Assets/Renamed')

    def test_folder_move_repairs_only_resource_references_and_retains_identity(self):
        assets.apply(self.plan())
        value = json.loads(self.scene.read_text())['actor']
        self.assertEqual(value['TagComponent']['Tag'], 'Assets/Models/box.hmesh')
        self.assertEqual(value['MeshComponent']['Path'], 'Assets/Renamed/box.hmesh')
        catalog = assets.read_json(self.root / '.he-assets/catalog.json')['assets']
        identity = catalog['Assets/Renamed/box.hmesh']
        assets.apply(assets.plan(self.root, 'Assets/Renamed', 'Assets/Again'))
        self.assertEqual(assets.read_json(self.root / '.he-assets/catalog.json')['assets']['Assets/Again/box.hmesh'], identity)

    def test_stale_preview_does_not_change_any_file(self):
        transaction = self.plan()
        self.scene.write_text('{}')
        with self.assertRaises(assets.Failure):
            assets.apply(transaction)
        self.assertTrue((self.root / 'Assets/Models/box.hmesh').exists())
        self.assertFalse((self.root / 'Assets/Renamed').exists())

    def test_move_updates_startup_cook_and_warmup_configuration(self):
        (self.root / 'Config').mkdir()
        config = self.root / 'Config/Game.json'
        config.write_text(json.dumps({'Game': {'DefaultScene': 'Assets/Models/box.hmesh',
            'ResourceWarmup': {'Groups': [{'Roots': [{'Path': 'Assets/Models/box.hmesh'}]}]}},
            'Cook': {'RootFiles': ['Assets/Models/box.hmesh'], 'IncludeDirectories': ['Assets/Models']}}))
        assets.apply(self.plan())
        result = json.loads(config.read_text())
        self.assertEqual(result['Game']['DefaultScene'], 'Assets/Renamed/box.hmesh')
        self.assertEqual(result['Game']['ResourceWarmup']['Groups'][0]['Roots'][0]['Path'], 'Assets/Renamed/box.hmesh')
        self.assertEqual(result['Cook']['RootFiles'], ['Assets/Renamed/box.hmesh'])
        self.assertEqual(result['Cook']['IncludeDirectories'], ['Assets/Renamed'])

    def test_unknown_reference_blocks_instead_of_guessing(self):
        self.scene.write_text(json.dumps({'custom': 'Assets/Models/box.hmesh'}))
        with self.assertRaises(assets.Failure) as caught:
            self.plan()
        self.assertEqual(caught.exception.diagnostic['code'], 'unclassified_reference')

    def test_script_literal_blocks(self):
        (self.root / 'Scripts').mkdir()
        (self.root / 'Scripts/game.py').write_text('model = "Assets/Models/box.hmesh"')
        with self.assertRaises(assets.Failure):
            self.plan()

    def test_real_script_field_format_and_mesh_skeleton_reference(self):
        fields = {'schema': 1, 'fields': {'model': {'type': 'resource', 'value': 'Assets/Models/box.hmesh'}}}
        self.scene.write_text(json.dumps({'actor': {'ScriptComponent': {'Fields': json.dumps(fields)}}}))
        assets.apply(self.plan())
        result = json.loads(json.loads(self.scene.read_text())['actor']['ScriptComponent']['Fields'])
        self.assertEqual(result['fields']['model']['value'], 'Assets/Renamed/box.hmesh')

    def test_relative_mesh_and_ui_dependencies_survive_owner_move(self):
        (self.root / 'Assets/Models/box.hmesh').write_text(json.dumps({'uri': '../data.bin', 'skeletonPath': '../rig.hskel'}))
        ui = self.root / 'Assets/Models/view.rml'
        ui.write_bytes(b'<img src="../picture.png"/>\r\n<link href="../style.rcss"/>')
        (self.root / 'Assets/Other').mkdir()
        assets.apply(assets.plan(self.root, 'Assets/Models', 'Assets/Other/Models'))
        mesh = json.loads((self.root / 'Assets/Other/Models/box.hmesh').read_text())
        self.assertEqual(mesh['uri'], '../../data.bin')
        self.assertEqual(mesh['skeletonPath'], '../../rig.hskel')
        self.assertEqual((self.root / 'Assets/Other/Models/view.rml').read_bytes(),
            b'<img src="../../picture.png"/>\r\n<link href="../../style.rcss"/>')

    def test_ui_relative_reference_tracks_target_move(self):
        (self.root / 'Assets/view.rcss').write_text('body { background-image: url("Models/box.hmesh"); }')
        assets.apply(self.plan())
        self.assertIn('url("Renamed/box.hmesh")', (self.root / 'Assets/view.rcss').read_text())

    def test_failure_rolls_back_files_and_move(self):
        transaction = self.plan()
        original = self.scene.read_bytes()
        native = assets.atomic
        failed = False
        def fail_once(path, data):
            nonlocal failed
            if Path(path) == self.scene and not failed:
                failed = True
                raise OSError('injected disk failure')
            return native(path, data)
        with patch.object(assets, 'atomic', fail_once):
            with self.assertRaises(OSError):
                assets.apply(transaction)
        self.assertTrue(failed, 'The scene-write failure must actually be injected.')
        self.assertEqual(self.scene.read_bytes(), original)
        self.assertTrue((self.root / 'Assets/Models/box.hmesh').exists())
        self.assertFalse((self.root / 'Assets/Renamed').exists())
        self.assertFalse((self.root / '.he-assets/pending.json').exists())

    def test_escape_and_nested_destination_rejected(self):
        for target in ('Assets/../outside', 'Assets/Models/nested', 'C:/outside', 'Assets/NUL.txt',
                       'Assets/bad /file', 'Assets/wild*card', 'Assets/COM1', 'Assets/bad\x01name'):
            with self.assertRaises(assets.Failure):
                assets.plan(self.root, 'Assets/Models', target)

    def test_crlf_and_bom_survive_failed_transaction(self):
        original = b'\xef\xbb\xbf{\r\n  "MeshComponent": {"Path": "Assets/Models/box.hmesh"}\r\n}\r\n'
        self.scene.write_bytes(original)
        transaction = self.plan()
        native = assets.atomic
        failed = False
        def fail_once(path, data):
            nonlocal failed
            if Path(path).name == 'catalog.json' and not failed:
                failed = True
                raise OSError('injected catalog write failure')
            return native(path, data)
        with patch.object(assets, 'atomic', fail_once):
            with self.assertRaises(OSError):
                assets.apply(transaction)
        self.assertTrue(failed, 'Transaction must pass its byte-exact preflight.')
        self.assertEqual(self.scene.read_bytes(), original)
        self.assertTrue((self.root / 'Assets/Models').exists())

    def test_successful_crlf_move(self):
        self.scene.write_bytes(b'{\r\n"MeshComponent":{"Path":"Assets/Models/box.hmesh"}\r\n}\r\n')
        assets.apply(self.plan())
        self.assertEqual(json.loads(self.scene.read_bytes())['MeshComponent']['Path'], 'Assets/Renamed/box.hmesh')

    def test_new_reference_after_preview_blocks(self):
        transaction = self.plan()
        (self.root / 'Assets/new.he').write_text('{}')
        with self.assertRaises(assets.Failure):
            assets.apply(transaction)

    def test_recovery_preserves_later_user_edit(self):
        transaction = self.plan()
        (self.root / 'Assets/Models').rename(self.root / 'Assets/Renamed')
        self.scene.write_text('{"user":"edit"}')
        with self.assertRaises(assets.Failure):
            assets.restore(self.root, transaction)
        self.assertEqual(self.scene.read_text(), '{"user":"edit"}')


if __name__ == '__main__':
    unittest.main()
