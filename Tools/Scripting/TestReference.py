"""Offline SDK reference tests; fixtures do not constitute runtime validation."""
import json
from pathlib import Path
import tempfile
import unittest

from GenerateReference import generate, check


class ReferenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.xml = self.root / "ScriptCore.xml"
        self.xml.write_text('<doc><members><member name="T:HEngine.Audio"><summary>Native audio.</summary></member><member name="T:HEngine.GameSettings"><summary>User settings.</summary></member></members></doc>')
        self.output = self.root / "Docs/Api"

    def tearDown(self):
        self.temp.cleanup()

    def test_source_free_generation_and_verification(self):
        result = generate(self.xml, self.output)
        self.assertEqual(result["members"], 2)
        self.assertEqual(check(self.xml, self.output)["status"], "PASS")

    def test_changed_assembly_documentation_rejected(self):
        generate(self.xml, self.output)
        self.xml.write_text('<doc><members/></doc>')
        with self.assertRaisesRegex(ValueError, "inputs"):
            check(self.xml, self.output)

    def test_old_assembly_rejected(self):
        self.xml.write_text('<doc><members/></doc>')
        with self.assertRaisesRegex(ValueError, "missing"):
            generate(self.xml, self.output)

    def test_changed_page_rejected(self):
        generate(self.xml, self.output)
        (self.output / "index.md").write_text('wrong reference')
        with self.assertRaisesRegex(ValueError, "changed"):
            check(self.xml, self.output)

    def test_changed_member_index_rejected(self):
        generate(self.xml, self.output)
        path = self.output / "api-index.json"
        data = json.loads(path.read_text())
        data["members"] = []
        path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "member index"):
            check(self.xml, self.output)


if __name__ == '__main__':
    unittest.main()
