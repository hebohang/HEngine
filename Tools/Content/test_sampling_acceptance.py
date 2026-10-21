"""Owned capture cleanup regressions using mock sessions only; no engine/GPU."""
import argparse
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import uuid

import sampling_acceptance as acceptance


class CaptureCleanupTests(unittest.TestCase):
    def exercise(self, stop_error=False, start_error=False):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project = root / "Project"
            baseline = project / "Assets/Scenes/VFXAuthoringPreview.he"
            baseline.parent.mkdir(parents=True)
            baseline.write_text("{}", encoding="utf-8")
            imported = project / "Imported.he"
            imported.write_text('{"source":{"TagComponent":{"Tag":"source"}}}', encoding="utf-8")
            original = imported.read_bytes()
            mock_image = root / "mock-image"
            mock_image.write_bytes(b"mock screenshot receipt payload")
            namespace = uuid.uuid4()
            output = root / "evidence"
            calls = []

            class MockSession:
                def __init__(self, *args, **kwargs):
                    pass

                def start(self):
                    calls.append("start")
                    if start_error:
                        raise RuntimeError("mock start failure")

                def call(self, method, params=None):
                    if method == "runtime.step":
                        return dict(targetCompletedSteps=60)
                    if method == "session.status":
                        return dict(world=dict(runtimeControl=dict(completedSteps=60, pendingSteps=0, stepInProgress=False)))
                    if method == "capture.frame":
                        return dict(path=str(mock_image))
                    raise AssertionError(method)

                def stop(self):
                    calls.append("stop")
                    if stop_error:
                        raise RuntimeError("mock stop failure")

            args = argparse.Namespace(backend="DX12", existing_build_sha256="mock-fixed-hash")
            with patch.object(acceptance, "PROJECT", project), patch.object(acceptance, "Session", MockSession):
                if stop_error:
                    with self.assertRaises(acceptance.CleanupFailure):
                        acceptance.capture(imported, output, args, namespace)
                elif start_error:
                    with self.assertRaisesRegex(RuntimeError, "mock start failure"):
                        acceptance.capture(imported, output, args, namespace)
                else:
                    acceptance.capture(imported, output, args, namespace)
            scene = project / "Assets/Effects/Authoring/SamplingAcceptance" / str(namespace) / "Capture.he"
            receipt = acceptance.read(output / "capture.json")
            self.assertEqual(calls, ["start", "stop"])
            self.assertEqual(scene.exists(), stop_error)
            self.assertEqual(receipt["scenePath"], str(scene))
            self.assertEqual(receipt["sceneRetainedForRecovery"], stop_error)
            self.assertEqual(imported.read_bytes(), original)
            self.assertTrue((output / "Capture.he").exists())
            if stop_error:
                self.assertEqual(receipt["cleanupError"], "mock stop failure")
                self.assertEqual(scene.read_bytes(), (output / "Capture.he").read_bytes())
            else:
                self.assertNotIn("cleanupError", receipt)

    def test_successful_stop_removes_only_live_capture_scene(self):
        self.exercise()

    def test_failed_stop_preserves_recovery_scene_and_receipt(self):
        self.exercise(stop_error=True)

    def test_start_failure_still_stops_and_retains_evidence(self):
        self.exercise(start_error=True)


if __name__ == "__main__":
    unittest.main()
