import json
from pathlib import Path
import tempfile
import unittest

from PIL import Image
from template_matrix_review import review,digest


class ReviewTests(unittest.TestCase):
    def fixture(self,root):
        effect_dir=root/'game-gpu'/'effect';effect_dir.mkdir(parents=True)
        def capture(path,color,**extra):
            Image.new('RGB',(80,40),color).save(path)
            return dict(path=str(path),sha256=digest(path),**extra)
        empty=capture(root/'empty.png','black')
        effect=dict(id='effect',captures=[capture(effect_dir/f'{i}.png',(i*20,0,0),frame=i,stages=[f'stage{i}']) for i in range(7)],
                    retired=capture(effect_dir/'retired.png','black'))
        (effect_dir/'report.json').write_text(json.dumps(effect))
        summary=dict(id=effect['id'],report=str(effect_dir/'report.json'),commandStatus='PASS')
        source=root/'report.json';source.write_text(json.dumps(dict(backend='Vulkan',groups=[dict(scene='Assets/scene.he',simulation='gpu',empty=empty,effects=[summary])])))
        return source

    def test_all_moments_and_references_preserved_without_visual_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);source=self.fixture(root)
            originals={p:digest(p) for p in root.rglob('*') if p.is_file()}
            output,result=review(source)
            self.assertEqual(result['visualStatus'],'NOT_REVIEWED')
            effect=result['effects'][0]
            self.assertEqual([im['frame'] for im in effect['images']],[None,*range(7),None])
            self.assertEqual([i for s in effect['sheets'] for i in s['imageIndices']],list(range(9)))
            for sheet in effect['sheets']:
                p=Path(sheet['path'])
                self.assertLessEqual(p.stat().st_size,500*1024)
                with Image.open(p) as im:self.assertLessEqual(max(im.size),1280)
            self.assertEqual(originals,{p:digest(p) for p in originals})
            self.assertEqual(effect['images'][-1]['vsEmpty']['maxChannelDelta'],0)
            with self.assertRaises(ValueError):review(source,output)

    def test_tampered_png_rejected_and_failed_manifest_still_not_reviewed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);source=self.fixture(root)
            Image.new('RGB',(80,40),'white').save(root/'empty.png')
            output=root/'new-review'
            with self.assertRaisesRegex(ValueError,'hash mismatch'):review(source,output)
            result=json.loads((output/'review.json').read_text())
            self.assertEqual(result['preparationStatus'],'FAIL')
            self.assertEqual(result['visualStatus'],'NOT_REVIEWED')


if __name__=='__main__':unittest.main()
