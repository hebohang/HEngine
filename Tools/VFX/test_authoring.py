import copy
import json
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

import authoring as a


class AuthoringTests(unittest.TestCase):
    def test_cpu_explosion_radial_contract_and_publication_rejection(self):
        import shutil
        recipe = json.loads((a.RECIPES/'explosion-ember-low.recipe.json').read_text())
        recipe['id'] = 'test-cpu-explosion-radial'
        with tempfile.TemporaryDirectory(prefix='he-cpu-radial-test-') as temporary:
            root = Path(temporary)
            lock = json.loads((a.RECIPES/'toolchain.lock.json').read_text())
            for relative in list(lock['files']) + [a.stress.SOURCE_LICENSE]:
                destination = root/relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(a.ROOT/relative, destination)
            # Validate tool identity in the isolated root, execute the real tool
            # from its complete installation, and publish only into that root.
            def compiler(_editor, project, binary):
                a.toon.compile_effect(a.ROOT/'Engine/Tools/Effekseer/bin/Effekseer.exe', project, binary)
            result = a.build(recipe, root=root, compiler=compiler)
            author = root/'Project/SourceAssets/VFXAuthoring'/recipe['id']
            binary = author/(recipe['id']+'.efkefc')
            actual = a.inspect_compiled(binary, recipe)
            self.assertFalse(actual['gpu_enabled'])
            values = actual['verified_parameters']
            self.assertEqual(values['GenerationLocationValues/Type'], 1)
            self.assertEqual(values['GenerationLocationValues/Sphere/Radius/Center'], 0)
            self.assertAlmostEqual(values['LocationValues/PVA/Velocity/Z/Center'], recipe['compile']['radius_m']/int(actual['life_frames']))
            self.assertEqual(ET.fromstring((author/(recipe['id']+'.efkproj')).read_bytes()).findtext('Global/RandomSeed'), str(recipe['compile']['seed']))
            def snapshot():
                return {str(p.relative_to(root)): (a.sha(p.read_bytes()), p.stat().st_mtime_ns)
                        for p in (root/'Project').rglob('*') if p.is_file()}
            before = snapshot()
            called = []
            def forbidden_compiler(*args):
                called.append(True)
                raise AssertionError('Invalid direction reached the compiler')
            for direction in ([1,0,0], [0,0,1], [0,-1,0], [0,.5,0]):
                invalid = copy.deepcopy(recipe)
                invalid['compile']['direction'] = direction
                with self.assertRaisesRegex(a.RecipeError, 'CPU explosion currently requires direction'):
                    a.build(invalid, root=root, force=True, compiler=forbidden_compiler)
                self.assertEqual(snapshot(), before, 'Rejected recipe altered a published artifact or timestamp')
            self.assertFalse(called)
            # An official recompile that discards rotation must fail inspection.
            xml = ET.fromstring(a.source(recipe))
            xml.find('Root/Children/Node/GenerationLocationValues/EffectsRotation').text = 'False'
            malformed = root/'discarded-rotation.efkproj'
            malformed.write_bytes(ET.tostring(xml, encoding='utf-8', xml_declaration=True))
            for name,payload in a.textures().items():
                (root/name).parent.mkdir(parents=True, exist_ok=True)
                (root/name).write_bytes(payload)
            rejected = root/'discarded-rotation.efkefc'
            compiler(None, malformed, rejected)
            with self.assertRaisesRegex(a.RecipeError, 'lost birth rotation'):
                a.inspect_compiled(rejected, recipe)
            self.assertEqual(snapshot(), before)

    def test_cpu_explosion_direction_scope_and_description(self):
        cpu = json.loads((a.RECIPES/'explosion-ember-low.recipe.json').read_text())
        self.assertEqual(a.validate(cpu)['compile']['direction'], [0,1,0])
        for name in ('hit-ember', 'trail-ember', 'environment-ember-low', 'explosion-ember'):
            recipe = json.loads((a.RECIPES/(name+'.recipe.json')).read_text())
            recipe['compile']['direction'] = [1,0,0]
            self.assertEqual(a.validate(recipe)['compile']['direction'], [1,0,0])
        description = a.catalog(lambda: {})['compile_parameters']
        self.assertIn('CPU explosion only accepts [0,1,0]', description['direction'])
        self.assertIn('particle-center radius', description['radius_m'])
        self.assertIn('not the visible', description['radius_m'])

    def setUp(self):
        self.recipe = json.loads((a.RECIPES / "explosion-ember.recipe.json").read_text())

    def test_strict_validation(self):
        for mutation in (lambda r:r.update(schema_version=2),
                         lambda r:r["compile"].update(unknown_dynamic_radius=1),
                         lambda r:r["compile"].update(radius_m=float('nan')),
                         lambda r:r["compile"].update(particles=100001),
                         lambda r:r["compile"].update(color=[1,2,3]),
                         lambda r:r["playback"].update(intensity=-.01),
                         lambda r:r["playback"].update(intensity=1.01),
                         lambda r:r["playback"].update(intensity=float('nan')),
                         lambda r:r["playback"].update(intensity=True),
                         lambda r:r["playback"].pop("intensity"),
                         lambda r:r["requirements"].update(fallback_recipe=None),
                         lambda r:r.update(dependencies=["missing.png"])):
            value = copy.deepcopy(self.recipe)
            mutation(value)
            with self.assertRaises(a.RecipeError):
                a.validate(value)

    def test_independent_fallback_requires_matching_cpu_publication(self):
        fallback=copy.deepcopy(json.loads((a.RECIPES/'explosion-ember-low.recipe.json').read_text()))
        fallback['id']='test-independent-fallback'
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            author=root/'Project/SourceAssets/VFXAuthoring'/fallback['id']
            author.mkdir(parents=True)
            source=author/'recipe.json'
            source.write_bytes(a.encoded(fallback))
            with self.assertRaisesRegex(a.RecipeError,'Build the independent'):
                a.resolve_fallback(fallback['id'],root)
            package=root/'Project/Assets/Effects/Authoring'/(fallback['id']+'.efkpkg')
            package.parent.mkdir(parents=True);package.write_bytes(b'CPU-only test publication')
            (author/'build.json').write_bytes(a.encoded(dict(inputs=dict(recipe=a.sha(a.encoded(fallback))),
                outputs={package.relative_to(root).as_posix():a.sha(package.read_bytes())})))
            self.assertEqual(a.resolve_fallback(fallback['id'],root),fallback)
            package.write_bytes(b'tampered')
            with self.assertRaisesRegex(a.RecipeError,'stale or modified'):
                a.resolve_fallback(fallback['id'],root)
            fallback['id']='wrong-id';source.write_bytes(a.encoded(fallback))
            with self.assertRaisesRegex(a.RecipeError,'explicit CPU recipe'):
                a.resolve_fallback('test-independent-fallback',root)
            fallback=copy.deepcopy(self.recipe);fallback['id']='test-independent-fallback'
            source.write_bytes(a.encoded(fallback))
            with self.assertRaisesRegex(a.RecipeError,'explicit CPU recipe'):
                a.resolve_fallback(fallback['id'],root)

    def test_real_compiler_timing_roundtrip_rejects_legacy_schedule(self):
        # Use isolated artifacts: this test never replaces a live recipe/package.
        with tempfile.TemporaryDirectory(prefix="he-vfx-timing-test-") as temporary:
            stage = Path(temporary)
            for name, payload in a.textures().items():
                (stage/name).parent.mkdir(parents=True, exist_ok=True)
                (stage/name).write_bytes(payload)
            for name in ("hit-ember", "environment-ember-low", "environment-arcane", "aura-arcane"):
                recipe = json.loads((a.RECIPES/(name+".recipe.json")).read_text(encoding="utf-8"))
                project = stage/(name+".efkproj")
                binary = stage/(name+".efkefc")
                project.write_bytes(a.source(recipe))
                a.toon.compile_effect(a.ROOT/"Engine/Tools/Effekseer/bin/Effekseer.exe", project, binary)
                inspected = a.inspect_compiled(binary, recipe)["verified_parameters"]
                self.assertEqual(inspected["CommonValues/Generation/GenerationTime/Center"],
                                 {"hit-ember": .00001, "environment-ember-low": 1,
                                  "environment-arcane": .75, "aura-arcane": 96}[name])
                if name == "environment-arcane":
                    self.assertEqual(inspected["GpuParticles/Basic/EmitPerFrame"], 2)
                    self.assertEqual(inspected["GpuParticles/Basic/LifeTime/Center"], 187)
                    self.assertEqual(inspected["CommonValues/Life/Center"], 378)
            xml = ET.fromstring(a.source(recipe))
            common = xml.find("Root/Children/Node/CommonValues")
            generation = common.find("Generation")
            for field in list(generation):
                common.append(field)
            common.remove(generation)
            project.write_bytes(ET.tostring(xml, encoding="utf-8", xml_declaration=True))
            a.toon.compile_effect(a.ROOT/"Engine/Tools/Effekseer/bin/Effekseer.exe", project, binary)
            with self.assertRaisesRegex(a.RecipeError, "GenerationTime"):
                a.inspect_compiled(binary, recipe)

    def test_playback_is_not_baked_and_catalog_queries_live_runtime(self):
        original = a.source(self.recipe)
        for intensity in (0, .5, 1):
            recipe = copy.deepcopy(self.recipe)
            recipe["playback"].update(intensity=intensity,speed=0,scale=1000)
            a.validate(recipe)
            self.assertEqual(a.source(recipe),original)
        state = {"gpu":{"validated":False,"enabled":False}}
        self.assertFalse(a.catalog(lambda:copy.deepcopy(state))["runtime_support"]["description"]["gpu"]["validated"])
        state["gpu"].update(validated=True,enabled=True)
        self.assertTrue(a.catalog(lambda:copy.deepcopy(state))["runtime_support"]["description"]["gpu"]["validated"])
        self.assertEqual(a.catalog()["runtime_support"]["status"],"unavailable")

    def test_real_compiler_trail_birth_binding_and_velocity(self):
        lock = json.loads((a.RECIPES / "toolchain.lock.json").read_text())
        for relative, expected in lock["files"].items():
            self.assertEqual(a.sha((a.ROOT / relative).read_bytes()), expected)
        with tempfile.TemporaryDirectory(prefix="he-vfx-trail-test-") as temporary:
            stage = Path(temporary)
            for name, payload in a.textures().items():
                (stage/name).parent.mkdir(parents=True, exist_ok=True)
                (stage/name).write_bytes(payload)
            bindings = ("LocationEffectType", "RotationEffectType", "ScaleEffectType")
            for name in ("trail-arcane", "trail-ember", "trail-frost"):
                recipe = a.validate(json.loads((a.RECIPES/(name+".recipe.json")).read_text()))
                self.assertEqual(recipe["compile"]["direction"], [0, 0, -1])
                project, binary = stage/(name+".efkproj"), stage/(name+".efkefc")
                project.write_bytes(a.source(recipe))
                a.toon.compile_effect(a.ROOT/"Engine/Tools/Effekseer/bin/Effekseer.exe", project, binary)
                actual = a.inspect_compiled(binary, recipe)["verified_parameters"]
                for field in bindings:
                    self.assertEqual(actual["CommonValues/" + field], 1)
                for field in ("Center", "Min", "Max"):
                    for axis in ("X", "Y"):
                        self.assertEqual(actual["LocationValues/PVA/Velocity/" + axis + "/" + field], 0)
                    self.assertGreater(actual["LocationValues/PVA/Velocity/Z/" + field], 0)
            # Each mutation is compiled by the real pinned tool. A missing
            # non-default binding must resolve to actual default 2 and fail.
            for mutation in ("always", "omitted", "vertical"):
                with self.subTest(mutation=mutation):
                    xml = ET.fromstring(a.source(recipe))
                    node = xml.find("Root/Children/Node")
                    common = node.find("CommonValues")
                    if mutation == "always":
                        for field in bindings:
                            common.find(field).text = "2"
                    elif mutation == "omitted":
                        for field in bindings:
                            common.remove(common.find(field))
                    else:
                        for field in ("Center", "Min", "Max"):
                            z = node.find("LocationValues/PVA/Velocity/Z/" + field)
                            node.find("LocationValues/PVA/Velocity/Y/" + field).text = str(-float(z.text))
                            z.text = "0"
                    project.write_bytes(ET.tostring(xml, encoding="utf-8", xml_declaration=True))
                    a.toon.compile_effect(a.ROOT/"Engine/Tools/Effekseer/bin/Effekseer.exe", project, binary)
                    with self.assertRaisesRegex(a.RecipeError, "LocationEffectType|Velocity/Y"):
                        a.inspect_compiled(binary, recipe)
            # Keep the shared helper and other templates' binding semantics.
            unrelated = ET.fromstring(a.source(self.recipe))
            for field in bindings:
                self.assertEqual(unrelated.findtext("Root/Children/Node/CommonValues/" + field), "2")

    def test_all_templates_have_three_distinct_compiled_inputs(self):
        seen = {name:set() for name in a.TEMPLATES}
        for path in a.RECIPES.glob("*.recipe.json"):
            recipe = a.validate(json.loads(path.read_text()))
            source = a.source(recipe)
            xml = ET.fromstring(source)
            self.assertEqual(xml.findtext("Global/RandomSeed"), str(recipe["compile"]["seed"]))
            gpu = xml.find(".//GpuParticles/Enabled")
            self.assertEqual(gpu is not None and gpu.text == "True", recipe["compile"]["simulation"] == "gpu")
            if recipe["compile"]["simulation"] == "gpu":
                self.assertEqual(xml.findtext(".//GpuParticles/RenderColor/ColorInheritType"), "3")
            if recipe["compile"]["quality"] == "High":
                seen[recipe["template"]].add((tuple(recipe["compile"]["color"]),
                    recipe["compile"]["radius_m"], recipe["compile"]["attack_s"],
                    recipe["compile"]["sustain_s"], recipe["compile"]["decay_s"]))
        self.assertTrue(all(len(variants) == 3 for variants in seen.values()))

    def test_real_compiler_repeatability_and_failure_recovery(self):
        first = a.build(self.recipe, force=True)
        author = a.ROOT / "Project/SourceAssets/VFXAuthoring" / self.recipe["id"]
        report = json.loads((author / "build.json").read_text())
        before = {path: (a.sha((a.ROOT/path).read_bytes()), (a.ROOT/path).stat().st_mtime_ns) for path in report["outputs"]}
        self.assertEqual(a.build(self.recipe)["changed"], 0)
        after = {path: (a.sha((a.ROOT/path).read_bytes()), (a.ROOT/path).stat().st_mtime_ns) for path in report["outputs"]}
        self.assertEqual(before, after)
        self.assertEqual(a.build(self.recipe, force=True)["changed"], 0)
        broken = copy.deepcopy(self.recipe)
        broken["compile"]["color"][0] -= 1
        def fail(*args):
            raise a.toon.GenerationError("Injected compiler failure")
        with self.assertRaises(a.toon.GenerationError):
            a.build(broken, force=True, compiler=fail)
        self.assertEqual(before, {path: (a.sha((a.ROOT/path).read_bytes()), (a.ROOT/path).stat().st_mtime_ns) for path in report["outputs"]})

    def test_publication_failure_restores_whole_previous_release(self):
        a.build(self.recipe)
        author = a.ROOT / "Project/SourceAssets/VFXAuthoring" / self.recipe["id"]
        files = [path for path in author.rglob('*') if path.is_file()]
        files.append(a.ROOT / "Project/Assets/Effects/Authoring/explosion-ember.efkpkg")
        before = {path:(path.read_bytes(),path.stat().st_mtime_ns) for path in files}
        broken = copy.deepcopy(self.recipe)
        broken["compile"]["radius_m"] += .05
        writes = 0
        def fail_third(path, data):
            nonlocal writes
            writes += 1
            if writes == 3:
                raise OSError("Injected third publication failure")
            a.toon.publish_atomic(path,data)
        with self.assertRaises(OSError):
            a.build(broken, force=True, publisher=fail_third)
        self.assertEqual(writes, 3)
        self.assertEqual(before, {path:(path.read_bytes(),path.stat().st_mtime_ns) for path in files})

    def test_runtime_capacity_is_separate_from_compiler_limit(self):
        with self.assertRaises(a.RecipeError):
            a.validate_runtime_budget(self.recipe, {"particle_buffer_capacity":128})
        with self.assertRaises(a.RecipeError):
            a.validate_runtime_budget(self.recipe, {"particle_buffer_capacity":50176,"gpu_particles_enabled":True,"gpu_particles_capable":False})
        self.assertEqual(a.validate_runtime_budget(self.recipe, {"particle_buffer_capacity":{"current":50176},"gpu_particles_enabled":True,"gpu_particles_capable":True})["status"], "capacity_checked")


if __name__ == "__main__":
    unittest.main()
