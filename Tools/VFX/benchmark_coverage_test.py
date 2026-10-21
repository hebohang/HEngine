import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

from PIL import Image
from benchmark import coverage_proxy, collect_native_evidence, ROOT, sha


class CoverageTests(unittest.TestCase):
    def test_native_streams_capture_identity_and_failures(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary);logs=folder/'logs';logs.mkdir()
            stdout=logs/'stdout.log';stderr=logs/'stderr.log';frame=folder/'frame.png'
            receipt=f'run HGame: STARTED pid=42; stdout: {stdout}; stderr: {stderr}\n'
            (folder/'run.log').write_text(receipt+'run HGame: PASS exit=0\n')
            stdout.write_text(f'[Automation] Captured frame 60: {frame} (target=presented-backbuffer)\n')
            stderr.write_text('')
            result=collect_native_evidence(folder,frame,60,logs)
            self.assertTrue(result['captureReceiptMatches'])
            self.assertEqual(result['streams']['stdout']['sha256'],sha(stdout))
            for bad in (59,61):
                with self.assertRaises(RuntimeError):collect_native_evidence(folder,frame,bad,logs)
            stderr.write_text('VK_ERROR_DEVICE_LOST\n'*55)
            with self.assertRaises(RuntimeError):collect_native_evidence(folder,frame,60,logs)
            saved=json.loads((folder/'native-evidence.json').read_text())
            self.assertEqual(saved['errorCount'],55)
            self.assertEqual(len(saved['errors']),50)
            self.assertEqual(saved['streams']['stderr']['sha256'],sha(stderr))
            stderr.write_text('');(folder/'run.log').write_text(receipt)
            with self.assertRaises(RuntimeError):collect_native_evidence(folder,log_root=logs)
            (folder/'run.log').write_text(receipt+receipt+'run HGame: PASS exit=0\n')
            with self.assertRaises(RuntimeError):collect_native_evidence(folder,log_root=logs)

    def test_pixel_coverage_is_thresholded_and_control_drift_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)
            empty=Image.new('RGB',(10,10));active=empty.copy()
            active.putpixel((2,3),(17,0,0));active.putpixel((4,5),(4,0,0))
            for name,image in [('empty',empty),('active',active),('repeat',empty)]:
                image.save(folder/(name+'.png'))
            paths=[folder/(name+'.png') for name in ('empty','active','repeat')]
            result=coverage_proxy(*paths)
            self.assertEqual(result['status'],'PASS')
            self.assertEqual(result['effectDifference']['thresholds']['16']['coverageFraction'],.01)
            self.assertEqual(result['effectDifference']['thresholds']['1']['changedPixels'],2)
            self.assertEqual(result['effectDifference']['boundsAbove16'],(2,3,3,4))
            active.save(paths[2])
            self.assertEqual(coverage_proxy(*paths)['status'],'FAIL')
            empty.save(paths[1]);empty.save(paths[2])
            self.assertEqual(coverage_proxy(*paths)['status'],'FAIL')
            Image.new('RGB',(9,10)).save(paths[2])
            with self.assertRaises(ValueError):coverage_proxy(*paths)

    def test_empty_capture_runs_no_effects_and_flushes_at_stop(self):
        def forbidden(*args):raise AssertionError('Empty control must not load/play effects')
        fake=types.SimpleNamespace(VFX=types.SimpleNamespace(SetQualityTier=lambda value:None,
            GetStats=lambda:dict(update_serial=1,last_gpu_simulation_submission=1,last_gpu_render_submission=1),
            IsEffectPlaying=lambda handle:False,LoadEffect=forbidden,PlaySeeded=forbidden,StopEffect=forbidden))
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary);request=folder/'request.json';report=folder/'runtime.json'
            request.write_text(json.dumps(dict(effects=['effect']*16,warmupFrames=600,sampleFrames=600,coverageMode='empty',coverageFrames=60)))
            with patch.dict(sys.modules,HEngine=fake),patch.dict(os.environ,HE_VFX_BENCHMARK_REQUEST=str(request),HE_VFX_BENCHMARK_REPORT=str(report)):
                spec=importlib.util.spec_from_file_location('coverage_scene',ROOT/'Project/Scripts/VFXAuthoringBenchmark.py')
                module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
                scene=module.VFXAuthoringBenchmark()
                try:
                    scene.Start(None)
                    for _ in range(60):scene.Update(None)
                    self.assertFalse(report.exists())
                finally:scene.Stop()
            result=json.loads(report.read_text())
            self.assertEqual(result['status'],'PASS')
            self.assertTrue(all(row['active_after_restart']==0 for row in result['rows']))
            self.assertEqual(result['events'],[])


if __name__=='__main__':unittest.main()
