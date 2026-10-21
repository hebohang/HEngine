import copy
import importlib.util
import pathlib
import shutil
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('export_wechat', ROOT / 'Tools/WeChat/export_wechat.py')
exporter = importlib.util.module_from_spec(spec); spec.loader.exec_module(exporter)

class ExportContracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='he-wechat-export-test-')
        self.project = pathlib.Path(self.temp.name)
        shutil.copytree(ROOT / 'Project/Assets/WeChatSample', self.project / 'Assets/WeChatSample')
        self.config = exporter.read_json(ROOT / 'Project/Config/WeChat.json')
        self.scene_path = self.project / self.config['scene']
    def tearDown(self): self.temp.cleanup()
    def mutate(self, callback):
        scene = exporter.read_json(self.scene_path)
        callback(next(iter(scene.values())))
        self.scene_path.write_text(exporter.json.dumps(scene), encoding='utf-8')
    def test_current_scene(self): exporter.validate(self.project, self.config)
    def test_scene_package_preserves_resource_identity_and_bytes(self):
        # Non-ASCII, BOM and CRLF must survive packaging; JSON/text processing
        # can change byte count even when the scene remains semantically equal.
        scene = exporter.read_json(self.scene_path)
        next(iter(scene.values()))['TagComponent']['Tag'] = '触摸样例'
        payload = ('\ufeff' + exporter.json.dumps(scene, ensure_ascii=False, indent=2).replace('\n', '\r\n')).encode('utf-8')
        self.scene_path.write_bytes(payload)
        exporter.validate(self.project, self.config)
        staging = self.project / 'staging'
        resources = exporter.package_resources(self.project, self.config, staging)
        scene_resource = next(item for item in resources if item['type'] == 'scene')
        self.assertEqual(scene_resource['path'], self.config['scene'])
        self.assertTrue(scene_resource['packagePath'].endswith('.bin'))
        self.assertEqual((staging / scene_resource['packagePath']).read_bytes(), payload)
        self.assertEqual(scene_resource['bytes'], len(payload))
        self.assertEqual(scene_resource['sha256'], exporter.hashlib.sha256(payload).hexdigest())
        self.assertFalse((staging / self.config['scene']).exists())
        self.assertFalse((staging / (self.config['scene'] + '.json')).exists())
        for item in resources:
            self.assertEqual((staging / item['packagePath']).read_bytes(), (self.project / item['path']).read_bytes())
    def test_python_rejected(self):
        self.mutate(lambda entity: entity.update(PythonScriptComponent={'Module': 'player'}))
        with self.assertRaisesRegex(ValueError, 'PythonScriptComponent'): exporter.validate(self.project, self.config)
    def test_mesh_rejected(self):
        self.mutate(lambda entity: entity.update(MeshComponent={'Path': 'model.hmesh'}))
        with self.assertRaisesRegex(ValueError, 'Unsupported WeChat component MeshComponent'): exporter.validate(self.project, self.config)
    def test_shader_rejected(self):
        self.config['shaderProfile'] = 'desktop-pbr'
        with self.assertRaisesRegex(ValueError, 'Shader profile'): exporter.validate(self.project, self.config)
    def test_missing_texture(self):
        (self.project / self.config['texture']).unlink()
        with self.assertRaisesRegex(ValueError, 'Resource does not exist'): exporter.validate(self.project, self.config)
    def test_path_escape(self):
        for path in ('../outside.png', 'C:/outside.png', '/outside.png', 'Assets\\outside.png'):
            with self.assertRaises(ValueError): exporter.resolve_asset(self.project, path)
    def test_unknown_native(self):
        self.mutate(lambda entity: entity['NativeScriptComponent'].update(Type='NotLinked'))
        with self.assertRaisesRegex(ValueError, 'Unknown native type'): exporter.validate(self.project, self.config)
    def test_unsupported_property(self):
        self.mutate(lambda entity: entity['SpriteRendererComponent'].update(Shader='pbr.hlsl'))
        with self.assertRaisesRegex(ValueError, 'Unsupported fields'): exporter.validate(self.project, self.config)
    def test_invalid_save_defaults(self):
        self.mutate(lambda entity: entity['NativeScriptComponent']['Properties'].update(Score=-1))
        with self.assertRaisesRegex(ValueError, 'Score'): exporter.validate(self.project, self.config)
    def test_no_forged_appid(self):
        self.config['appid'] = 'touristappid'
        with self.assertRaisesRegex(ValueError, 'AppID'): exporter.validate(self.project, self.config)

if __name__ == '__main__': unittest.main(verbosity=2)
