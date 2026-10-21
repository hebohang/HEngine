"""Pure mock checks for acceptance drivers; never starts an engine or build."""
import copy
import contextlib
import json
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch

import character_acceptance as character
import editor_prefab_acceptance as editor
import prefab_acceptance as prefab


class AcceptanceContractTests(unittest.TestCase):
    def test_check_status_evidence_cannot_override_assertion(self):
        with tempfile.TemporaryDirectory() as directory:
            report=dict(checks=[])
            transaction=prefab.Transactions(types.SimpleNamespace(output=Path(directory)),report)
            transaction.check("status_readback",True,status={"world":"actual"})
            self.assertEqual(report["checks"][0]["status"],"PASS")
            self.assertEqual(report["checks"][0]["evidence"]["status"],{"world":"actual"})
            with self.assertRaisesRegex(RuntimeError,"Assertion failed"):
                transaction.check("negative",False,status="PASS")
            self.assertEqual(report["checks"][-1]["status"],"FAIL")

    def test_editor_queued_ticket_returns_inner_scene_receipt(self):
        client=editor.Mailbox(None,None,None,None)
        receipt=dict(ok=True,sceneId="scene",revision=3,completion="authorCommitted",temporaryIds={"$a":"uuid-a"})
        with patch.object(client,"call",side_effect=[dict(ok=True,completion="queued",ticket="ticket"),receipt]) as call:
            self.assertEqual(client.scene("apply",{}),receipt)
            self.assertEqual(call.call_args.args,("scene.result",{"ticket":"ticket"}))

    def test_editor_plan_has_no_temporary_ids_apply_does(self):
        client=editor.Mailbox(None,None,None,None)
        state=dict(ok=True,sceneId="scene",revision=3)
        plan=dict(state,completion="planned")
        applied=dict(ok=True,sceneId="scene",revision=4,completion="authorCommitted",temporaryIds={"$a":"uuid-a"})
        with patch.object(client,"describe",return_value=state), patch.object(client,"scene",side_effect=[plan,applied]) as scene:
            self.assertEqual(client.edit([{"op":"prefab"}])["temporaryIds"]["$a"],"uuid-a")
            self.assertEqual(scene.call_args_list[0].args[1],scene.call_args_list[1].args[1])
        bad=dict(plan,revision=4)
        with patch.object(client,"describe",return_value=state), patch.object(client,"scene",return_value=bad) as scene:
            with self.assertRaisesRegex(RuntimeError,"plan changed"):
                client.edit([])
            self.assertEqual(scene.call_count,1)

    def test_current_actor_scripts_are_real_and_dotted_path_rejected(self):
        source=editor.ROOT/"Project/Assets/Effects/Authoring/CharacterDemo"
        for filename in ("Actor.hprefab","Actor.propagation-v2.hprefab"):
            entities=json.loads((source/filename).read_text(encoding="utf-8"))["entities"]
            editor.verify_actor_scripts(entities,{"A":{"actor":"actor"}})
            broken=copy.deepcopy(entities)
            broken["actor"]["PythonScriptComponent"]["Path"]="Scripts.VFXCharacterAcceptance.py"
            with self.assertRaisesRegex(AssertionError,"script path"):
                editor.verify_actor_scripts(broken,{"A":{"actor":"actor"}})

    def test_character_receipt_requires_all_checks_and_real_motion(self):
        report=dict(status="passed",checks=list(character.REQUIRED_CHARACTER),instances=["a","b"],
                    events=[dict(name="attack"),dict(name="footstep")],
                    positions=[dict(before=[0,0,0],after=[1,2,3],expected=[1,2,3])])
        character.validate_receipt("character",report)
        incomplete=copy.deepcopy(report); incomplete["checks"].pop()
        with self.assertRaisesRegex(ValueError,"Missing character"):
            character.validate_receipt("character",incomplete)
        report["positions"][0]["expected"]=[1,2,4]
        with self.assertRaisesRegex(ValueError,"did not follow"):
            character.validate_receipt("character",report)

    def test_character_cleanup_failure_prevents_second_session(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            output=root/"out/agent/rendering-evidence/vfx-authoring/mock"
            storage_record={}
            @contextlib.contextmanager
            def scope(*args):
                yield storage_record
            storage=types.SimpleNamespace(workflow_scope=scope)
            failed=dict(case="character",status="FAIL",cleanupError="mock stop failed")
            argv=["character_acceptance.py","--backend","DX12","--existing-build-sha256","hash","--output",str(output)]
            with patch.object(character,"ROOT",root), patch.object(character,"digest",return_value="hash"), \
                 patch.object(character,"input_hashes",return_value={}), patch.object(character,"Storage",return_value=storage), \
                 patch.object(character,"run_case",return_value=failed) as run_case, patch("sys.argv",argv), patch("builtins.print"):
                self.assertEqual(character.main(),1)
                self.assertEqual(run_case.call_count,1)
            self.assertEqual(storage_record["status"],"unknown")


if __name__=="__main__":
    unittest.main()
