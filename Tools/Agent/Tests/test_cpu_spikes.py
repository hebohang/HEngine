import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "Analyze-CpuFrameSpikes.py"
spec = importlib.util.spec_from_file_location("cpu_spikes", SCRIPT)
cpu_spikes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cpu_spikes)


class CpuSpikeReportTests(unittest.TestCase):
    def test_sparse_system_log_and_render_world_offset(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stats = root / "stats.csv"
            stats.write_text(
                "frame,cpuWallFrameMs,updateMs,renderSubmitPresentMs,gpuFrameSpanMs,gpuVisibleObjects\n"
                "111,38.5,23.0,15.2,10.5,397\n"
                "112,40.6,25.1,15.1,10.0,397\n",
                encoding="utf-8",
            )
            log = root / "runtime.log"
            log.write_text(
                "[RuntimeSystemTiming] frame=112 totalMs=25.1 systemsMs: "
                "class HEngine::RenderSystem3D=16.2\n"
                "[RenderWorldExtractTiming] frame=112 totalMs=14.7 meshMs=13.4\n"
                "[ScriptSystemTiming] frame=112 totalMs=10.5 updateMs=9.2\n"
                "[RenderWorldExtractTiming] frame=113 totalMs=15.7 meshMs=14.2\n",
                encoding="utf-8",
            )
            report = cpu_spikes.analyze(stats, log, 33.333, 1)
            first, second = report["slowCpuFrames"]
            self.assertNotIn("systems", first)
            self.assertEqual(first["extract"]["meshMs"], 13.4)
            self.assertEqual(second["systems"]["RenderSystem3D"], 16.2)
            self.assertEqual(second["extract"]["meshMs"], 14.2)
            self.assertEqual(second["scripts"]["updateMs"], 9.2)


if __name__ == "__main__":
    unittest.main()
