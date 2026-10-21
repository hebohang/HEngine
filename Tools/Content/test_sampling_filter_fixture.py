"""CPU fixture contract only; no importer, Game or GPU."""
from pathlib import Path
import struct
import tempfile
import unittest
import uuid

import sampling_filter_acceptance as fixture


class FilterFixtureTests(unittest.TestCase):
    def test_modes_footprints_identity_and_source_are_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)/"source"; namespace=uuid.uuid4()
            document,manifest=fixture.generate(root,namespace)
            hashes=manifest["sourceSha256"]
            repeated,repeated_manifest=fixture.generate(root,namespace)
            self.assertEqual(hashes,repeated_manifest["sourceSha256"])
            self.assertEqual(document,repeated)
            self.assertEqual(len(document["nodes"]),8)
            self.assertEqual([p["minFilter"] for p in manifest["panels"][:6]],fixture.FILTERS)
            self.assertEqual([p["magFilter"] for p in manifest["panels"][-2:]],[9728,9729])
            self.assertTrue(all(1<p["lambdaEstimate"]<2 for p in manifest["panels"][:6]))
            self.assertTrue(all(p["lambdaEstimate"]<0 for p in manifest["panels"][-2:]))
            for panel in manifest["panels"]:
                left,top,right,bottom=panel["projectedBbox"]
                self.assertTrue(0<left<right<1280 and 0<top<bottom<720)
            data=(root/"sampling.bin").read_bytes()
            for mesh in document["meshes"]:
                attributes=mesh["primitives"][0]["attributes"]
                accessor=document["accessors"][attributes["TEXCOORD_0"]]
                offset=document["bufferViews"][accessor["bufferView"]]["byteOffset"]
                self.assertEqual(struct.unpack_from('<8f',data,offset),(0,0,1,0,1,1,0,1))
            for size in (8,256):
                self.assertEqual(struct.unpack_from('>II',(root/('frequency'+str(size)+'.png')).read_bytes(),16),(size,size))
            other,_=fixture.generate(Path(directory)/"independent",uuid.uuid4())
            self.assertNotEqual(document["scenes"][0]["extras"]["he_id"],other["scenes"][0]["extras"]["he_id"])


if __name__=="__main__":
    unittest.main()
