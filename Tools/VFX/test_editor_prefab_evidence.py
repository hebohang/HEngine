"""CPU tests for fail-closed native Editor evidence collection."""
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock

from editor_prefab_acceptance import Mailbox, atomic, collect_runtime_logs


class EditorEvidenceTests(unittest.TestCase):
    def test_client_accepts_claimed_request_and_atomic_response(self):
        # Exercise real filesystem publication: the client must not require its
        # request to remain at the producer path while awaiting a response.
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)
            for name in ("requests","responses"): (folder/name).mkdir()
            errors=[]
            def consumer():
                try:
                    deadline=time.monotonic()+3
                    while time.monotonic()<deadline:
                        requests=list((folder/"requests").glob("*.json"))
                        if requests: break
                        time.sleep(.005)
                    else: raise TimeoutError("No published request")
                    request=requests[0]
                    claimed=request.with_suffix(".claimed")
                    request.rename(claimed)
                    value=json.loads(claimed.read_text())
                    atomic(folder/"responses"/request.name,dict(protocolVersion=1,
                        requestId=value["requestId"],ok=True,result=dict(stopping=True)))
                    claimed.unlink()
                except Exception as error: errors.append(error)
            worker=threading.Thread(target=consumer)
            worker.start()
            try:
                client=Mailbox(folder,"test",Mock(poll=lambda:None),folder)
                self.assertEqual(client.call("session.stop",timeout=4),dict(stopping=True))
            finally: worker.join(timeout=4)
            self.assertFalse(worker.is_alive())
            self.assertEqual(errors,[])

    def fixture(self, folder, stderr="", passed=True):
        logs=folder/"logs"; logs.mkdir()
        output=folder/"evidence"; output.mkdir()
        stdout=logs/"native.stdout.log"; stdout.write_text("Editor started\n",encoding="utf-8")
        error=logs/"native.stderr.log"; error.write_text(stderr,encoding="utf-8")
        (output/"editor-check.stdout.log").write_text(
            f"run HEngineEditor: STARTED pid=42; stdout: {stdout}; stderr: {error}\n"+
            ("run HEngineEditor: PASS exit=0\n" if passed else "run HEngineEditor: FAIL exit=1\n"),encoding="utf-8")
        return logs,output

    def test_success_retains_both_streams(self):
        with tempfile.TemporaryDirectory() as temporary:
            logs,output=self.fixture(Path(temporary))
            report={}; collect_runtime_logs(output,report,logs)
            self.assertEqual(report["validationErrorCount"],0)
            self.assertTrue(report["nativeCheckPassed"])
            self.assertEqual(set(report["runtimeLogs"]),{"stdout","stderr"})

    def test_stderr_error_outside_wrapper_tail_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            logs,output=self.fixture(Path(temporary),"VK_ERROR_DEVICE_LOST\n"+"ordinary line\n"*100)
            report={}
            with self.assertRaisesRegex(RuntimeError,"validation"):
                collect_runtime_logs(output,report,logs)
            self.assertEqual(report["validationErrorCount"],1)
            self.assertTrue((output/"editor.stdout.log").is_file())
            self.assertEqual(json.loads((output/"runtime-validation-errors.json").read_text())[0]["line"],1)

    def test_failed_check_still_retains_native_logs(self):
        with tempfile.TemporaryDirectory() as temporary:
            logs,output=self.fixture(Path(temporary),passed=False)
            report={}
            with self.assertRaisesRegex(RuntimeError,"bounded check failed"):
                collect_runtime_logs(output,report,logs)
            self.assertEqual(len(report["runtimeLogs"]),2)

    def test_missing_receipt_cannot_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            logs,output=self.fixture(Path(temporary))
            (output/"editor-check.stdout.log").write_text("run HEngineEditor: PASS exit=0\n")
            with self.assertRaisesRegex(RuntimeError,"STARTED receipt"):
                collect_runtime_logs(output,{},logs)


if __name__=="__main__": unittest.main()
