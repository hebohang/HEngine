"""Contract tests run the actual PowerShell runner against a fake hectl."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class StressRunnerTests(unittest.TestCase):
    def run_fixture(self, recovery=None, exit_code=0):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for folder in ("Tools/VFX", "Tools/Agent", "Project/Config", "Project/Assets/Scenes", "out/agent/vfx-stress"):
                (root / folder).mkdir(parents=True)
            runner = root / "Tools/VFX/run_effekseer_stress.ps1"
            shutil.copyfile(ROOT / "Tools/VFX/run_effekseer_stress.ps1", runner)
            config = root / "Project/Config/Game.json"
            original = b'{"Game":{"DefaultScene":"Assets/Scenes/User.he"}}'
            config.write_bytes(original)
            (root / "Project/Assets/Scenes/VFXStressTest.he").write_text("{}")
            (root / "hectl.bat").write_text('@echo off\necho %*\nexit /b {}\n'.format(exit_code))
            (root / "Tools/Agent/Storage.ps1").write_text(
                "function Start-HEStorageRun { param($Project,$Workflow,$Paths,[switch]$Outer) return 'fixture' }\n"
                "function Complete-HEStorageRun { param($Run,$State) Write-Output ('STORAGE_FINISH='+$State) }\n")
            marker = root / "out/agent/vfx-stress/active.json"
            if recovery:
                (marker.parent / "Game.json.backup").write_bytes(original)
                temporary = b'{"Game":{"DefaultScene":"Assets/Scenes/VFXStressTest.he"}}'
                marker.write_text(json.dumps({"temporarySha256":hashlib.sha256(temporary).hexdigest()}))
                config.write_bytes(temporary if recovery == "matching" else b"external edit")
            environment = dict(os.environ)
            # PowerShell 7's inherited module path hides Windows PowerShell's
            # Get-FileHash module when launching 5.1 from a 7.x parent.
            environment.pop("PSModulePath", None)
            result = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                     str(runner), "-Scenario", "baseline"], capture_output=True,
                                    text=True, env=environment)
            return result, config.read_bytes(), marker.exists(), original

    def test_startup_override_never_edits_config(self):
        result, config, marker, original = self.run_fixture()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("-StartupScene Assets/Scenes/VFXStressTest.he", result.stdout)
        self.assertIn("STORAGE_FINISH=success", result.stdout)
        self.assertEqual(config, original)
        self.assertFalse(marker)

    def test_matching_legacy_marker_recovers(self):
        result, config, marker, original = self.run_fixture("matching")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(config, original)
        self.assertFalse(marker)

    def test_conflicting_legacy_marker_preserves_user_edit(self):
        result, config, marker, _ = self.run_fixture("conflict")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(config, b"external edit")
        self.assertTrue(marker)

    def test_hectl_failure_propagates_after_storage_finish(self):
        result, config, marker, original = self.run_fixture(exit_code=7)
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertIn("STORAGE_FINISH=failed", result.stdout)
        self.assertEqual(config, original)


if __name__ == "__main__":
    unittest.main()
