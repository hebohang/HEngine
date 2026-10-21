import json
import math
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

from bake import asset, digest, publish, verify
from lighting_math import FACES, blender_uv_rows, cube_irradiance_sh, cube_to_panorama, sh_basis, write_dds, write_hdr


class LightingTests(unittest.TestCase):
    def test_texture_uv_convention(self):
        uv = np.array([.2,.7,1])
        authored = np.array([[2,.4,.1],[-.3,.8,.2]])
        actual = blender_uv_rows({'useAffine':True,'affineRow0':authored[0],'affineRow1':authored[1]}) @ [uv[0],1-uv[1],1]
        expected = authored @ uv
        np.testing.assert_allclose(actual,[expected[0],1-expected[1]])
        np.testing.assert_allclose(blender_uv_rows({}),[[1,0,0],[0,1,0]])

    def test_failed_publish_restores_previous(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); output=root/'baked'; output.mkdir()
            (output/'scene').write_text('last good scene')
            stage=root/'stage'; stage.mkdir()
            original=Path.rename
            def fail_stage(path,target):
                if path==stage: raise PermissionError('Simulated locked destination')
                return original(path,target)
            with patch.object(Path,'rename',fail_stage):
                with self.assertRaises(PermissionError): publish(stage,output)
            self.assertEqual((output/'scene').read_text(),'last good scene')
            self.assertTrue(stage.is_dir())

    def test_project_override_invalidates_engine_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); (root/'Shaders').mkdir()
            (root/'Shaders/BasePBR.hlsl').write_text('new project override')
            engine=Path(__file__).resolve().parents[2]
            relative='Shaders/BasePBR.hlsl'
            manifest=root/'manifest.json'
            manifest.write_text(json.dumps({'schema':'hengine.lighting-bake','version':1,
                'inputs':{},'outputs':{},'tools':{},
                'engineInputs':{relative:digest(engine/'Engine/Content'/relative)}}))
            with self.assertRaisesRegex(ValueError,'project now overrides'):
                verify(root,manifest)

    def test_capture_camera_is_proper_rotation(self):
        for forward,right,up in FACES:
            rotation=np.stack([right,up,-forward],axis=1)
            self.assertAlmostEqual(np.linalg.det(rotation),1)
            np.testing.assert_allclose(rotation@np.array([0,0,-1]),forward)

    def test_constant_environment_energy(self):
        coefficients=cube_irradiance_sh(np.ones((6,32,32,3))*[2,.5,1])
        directions=np.array([[1,0,0],[0,1,0],[0,0,1],[-1,0,0]])
        actual=sh_basis(directions)@coefficients
        np.testing.assert_allclose(actual,np.tile(np.array([2,.5,1])*math.pi,(4,1)),atol=1e-8)

    def test_directional_signs(self):
        faces=np.zeros((6,64,64,3)); faces[0]=1
        coefficients=cube_irradiance_sh(faces)
        value=sh_basis(np.array([[1,0,0],[-1,0,0],[0,1,0]]))@coefficients
        self.assertGreater(value[0,0],value[2,0]+.5)
        self.assertGreater(value[0,0],value[1,0]+1)

    def test_panorama_orientation(self):
        faces=np.zeros((6,16,16,3))
        for i in range(6): faces[i]=i+1
        panorama=cube_to_panorama(faces,128)
        self.assertEqual(panorama[32,64,0],1) # +X
        self.assertEqual(panorama[32,96,0],5) # +Z
        self.assertEqual(panorama[0,64,0],3) # +Y

    def test_dds_header_and_mips(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'map.dds'
            write_dds(path,np.ones((16,16,3))*2)
            raw=path.read_bytes()
            self.assertEqual(raw[:4],b'DDS ')
            self.assertEqual(struct.unpack_from('<I',raw,128)[0],10)
            self.assertEqual(len(raw),148+sum(s*s*8 for s in [16,8,4,2,1]))
            np.testing.assert_allclose(np.frombuffer(raw,dtype='<f2',offset=148)[:4],[2,2,2,1])

    def test_reject_nonfinite(self):
        with self.assertRaises(ValueError): cube_irradiance_sh(np.full((6,8,8,3),float('nan')))

    def test_path_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError): asset(Path(directory),'../outside.dds')

    def test_stale_input_and_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); path=root/'source'; path.write_text('old')
            output=root/'output'; output.write_text('baked')
            manifest=root/'manifest.json'
            manifest.write_text(json.dumps({'schema':'hengine.lighting-bake','version':1,
                'inputs':{'source':digest(path)},'outputs':{'output':digest(output)},'tools':{}}))
            verify(root,manifest)
            config=root/'different-config.json'; config.write_text('{}')
            with self.assertRaises(ValueError): verify(root,manifest,config)
            path.write_text('changed')
            with self.assertRaises(ValueError): verify(root,manifest)
            path.write_text('old'); output.write_text('damaged')
            with self.assertRaises(ValueError): verify(root,manifest)


if __name__=='__main__': unittest.main()
