"""CPU-only evidence/observation validation; no Game session is constructed."""
import copy
import importlib.util
from pathlib import Path
import tempfile
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import content_sync as sync
from baked_model_acceptance import run
from baked_model_runtime_acceptance import validate_inputs, prepare_case, validate_observation
from test_baked_model_sync import native_outputs


class RuntimeEvidenceTests(unittest.TestCase):
    def test_probe_requires_actual_progress_and_acknowledgements(self):
        with tempfile.TemporaryDirectory() as temp:
            fake_engine = SimpleNamespace(GetSavedPath=lambda relative: str(Path(temp) / relative))
            path = Path(__file__).resolve().parents[2] / "Project/Scripts/BakedModelRuntimeProbe.py"
            with patch.dict(sys.modules, HEngine=fake_engine):
                spec = importlib.util.spec_from_file_location("tested_baked_probe", path)
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
            class Animator:
                animResPath = "Assets/Walk.hanim"
                progress = 0.03
                def GetClipCount(self): return 1
                def GetClipName(self, index): return "Walk"
                def GetProgress(self): return self.progress
                def SetSpeed(self, value): pass
                def Stop(self): self.progress = 0
                def Play(self, index): pass
                def Pause(self): pass
                def Step(self, seconds): self.progress += seconds / 2
            probe = module.BakedModelRuntimeProbe()
            probe.anim = Animator()
            probe.owner = SimpleNamespace(GetUUID=lambda: "model-uuid")
            probe.frame = 0
            probe.request = dict(nonce="fresh", clipPath=probe.anim.animResPath, durationSeconds=2)
            values = []
            probe.write = lambda value: values.append(value)
            routine = probe.observe()
            for _ in range(40):
                try:
                    next(routine)
                except StopIteration:
                    break
                if values:
                    sync.atomic(Path(temp) / "Reports/BakedModelRuntimeProbeAck.json", sync.encode(dict(nonce="fresh", sequence=values[-1]["sequence"])))
            self.assertEqual([value["progress"] for value in values], [0.0, 0.25, 0.5])
            probe.anim.Step = lambda seconds: None
            probe.anim.progress = 0
            routine = probe.observe()
            with self.assertRaisesRegex(AssertionError, "Actual Walk progress"):
                for _ in range(40):
                    next(routine)
                    if values:
                        sync.atomic(Path(temp) / "Reports/BakedModelRuntimeProbeAck.json", sync.encode(dict(nonce="fresh", sequence=values[-1]["sequence"])))

    def test_exact_snapshots_display_diff_and_tamper_rejection(self):
        with tempfile.TemporaryDirectory() as temp:
            project = Path(temp)
            relative = "SourceAssets/Quaternius/CuteAnimatedMonsters/Derived/Cactus/Cactus.gltf"
            sync.atomic(project / relative, (Path(__file__).resolve().parents[2] / "Project" / relative).read_bytes())
            sync.atomic(project / "Project.heproject", b"{}")
            importer = project / "importer.exe"
            sync.atomic(importer, b"test native identity")
            def process(command, log, timeout):
                if command[-1] == "--validate-scene":
                    return 0
                doc = sync.read_json(command[2])
                if doc["buffers"][0]["uri"].endswith(",AA=="):
                    return 1
                native_outputs(project, Path(command[3]), doc)
                # Native duration metadata needed by the playback driver.
                for path in Path(command[3]).glob("*.hanim"):
                    animation = sync.read_json(path)
                    animation["clips"][0]["ticksPerSecond"] = 1
                    sync.atomic(path, sync.encode(animation))
                return 0
            with patch.object(sync, "run_process", process):
                report = run(SimpleNamespace(project=str(project), importer=str(importer), importer_sha256=sync.file_hash(importer), report=str(project / "native"), timeout=5))
            report_path = project / "native/report.json"
            pin = sync.file_hash(report_path)
            _, cases = validate_inputs(report_path, pin, project)
            self.assertEqual(validate_inputs(report_path, pin.upper(), project)[1], cases)
            original_scene_hash = sync.file_hash(report["scene"])
            harness = prepare_case(cases["final"], "final", project)
            self.assertEqual(original_scene_hash, sync.file_hash(report["scene"]))
            self.assertTrue(harness["differences"])
            self.assertEqual(harness["resources"], cases["final"]["registry"]["artifacts"])
            actual = sync.read_json(project / harness["scene"])[harness["entityId"]]
            self.assertEqual(actual["AnimatorComponent"]["animResPath"], cases["final"]["clipPath"])
            display_scene = sync.read_json(project / harness["scene"])
            camera = next(value["CameraComponent"] for value in display_scene.values() if "CameraComponent" in value)
            self.assertTrue(camera["bOverrideExposure"])
            self.assertAlmostEqual(camera["ExposureEV100"], 0.3219280948873623)
            # A historic clip is still independently pinned by its snapshot.
            (project / cases["initial"]["clipPath"]).write_text("tampered")
            with self.assertRaisesRegex(ValueError, "Native resource"):
                validate_inputs(report_path, pin, project)

    def test_rendered_palette_and_weights_require_complete_evidence(self):
        item = dict(entityId=17, currentPaletteCount=7, hashCoversFullPalettes=True, nonFiniteComparisons=0,
            weights=dict(complete=True, inspectedVertices=100, invalidWeights=0, positiveInfluencesOutsidePalette=0, verticesWithoutValidWeight=0))
        value = dict(available=True, objectsTruncated=False, skinnedObjectCount=1, objects=[item])
        self.assertEqual(validate_observation(value, "persisted-uuid"), item)
        for field, replacement in (("invalidWeights", 1), ("complete", False), ("positiveInfluencesOutsidePalette", 1)):
            bad = copy.deepcopy(value)
            bad["objects"][0]["weights"][field] = replacement
            with self.assertRaises(ValueError):
                validate_observation(bad, "persisted-uuid")
        value["skinnedObjectCount"] = 2
        with self.assertRaises(ValueError):
            validate_observation(value, "persisted-uuid")


if __name__ == "__main__":
    unittest.main()
