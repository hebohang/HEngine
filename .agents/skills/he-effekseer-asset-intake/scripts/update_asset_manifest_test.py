import copy
from pathlib import Path
import tempfile
import unittest
from update_asset_manifest import validate_renderer_evidence, sha256_file, SUPPORTED_RENDERER_APIS


class RendererEvidenceTests(unittest.TestCase):
    def manifest(self, root, gpu=False):
        refs={}
        for field in ('executable','effect','report'):
            path=root/field;path.write_bytes(field.encode())
            refs[field]=dict(path=str(path),sha256=sha256_file(path))
        entries=[dict(backend=api,status='PASS',simulation='gpu' if gpu else 'cpu',
                      execution_scope='development_probe' if gpu else 'production',
                      gpu_enabled=gpu,gpu_validated=False,**refs) for api in SUPPORTED_RENDERER_APIS]
        return dict(supported_renderer_apis=list(SUPPORTED_RENDERER_APIS),uses_gpu_particles=gpu,
                    profiler_baseline=dict(renderer_evidence=entries))

    def test_both_backends_require_current_matching_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);manifest=self.manifest(root)
            validate_renderer_evidence(manifest,str(root/'effect'))
            missing=copy.deepcopy(manifest);missing['profiler_baseline']['renderer_evidence'].pop()
            with self.assertRaises(ValueError):validate_renderer_evidence(missing)
            with self.assertRaises(ValueError):validate_renderer_evidence(manifest,str(root/'another'))
            (root/'executable').write_bytes(b'new build')
            with self.assertRaisesRegex(ValueError,'hash mismatch'):validate_renderer_evidence(manifest)

    def test_probe_does_not_publish_production_gpu_support(self):
        with tempfile.TemporaryDirectory() as temporary:
            manifest=self.manifest(Path(temporary),gpu=True)
            validate_renderer_evidence(manifest)
            entry=manifest['profiler_baseline']['renderer_evidence'][0]
            entry['execution_scope']='production'
            with self.assertRaisesRegex(ValueError,'production support'):validate_renderer_evidence(manifest)
            entry['gpu_validated']=True
            validate_renderer_evidence(manifest)
            entry['gpu_enabled']=False
            with self.assertRaises(ValueError):validate_renderer_evidence(manifest)


if __name__=='__main__':unittest.main()
