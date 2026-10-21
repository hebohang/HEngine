"""Owned log retention on failure; no engine process is started."""
from pathlib import Path
import tempfile
import unittest

from preview import ROOT, Session


class PreviewSessionTests(unittest.TestCase):
    def test_failed_stop_retains_native_logs(self):
        parent = (ROOT/'out/agent/sessions').resolve()
        with tempfile.TemporaryDirectory(dir=parent, prefix='vfx-log-test-') as temporary:
            folder = Path(temporary).resolve()
            self.assertTrue(folder.is_relative_to(parent))
            output = folder/'evidence'
            output.mkdir()
            mailbox = folder/'mailbox'
            mailbox.mkdir()
            (folder/'game.stdout.log').write_text('Vulkan validation: Validation Error: fixture\n')
            (folder/'game.stderr.log').write_text('native stderr fixture\n')
            receipt = output/'start.stdout.log'
            receipt.write_text('session: '+str(mailbox)+'\n')
            session = Session(output, 'Vulkan', 'fixture-hash')

            def run(arguments, label, parse=True):
                if label == 'stop':
                    raise RuntimeError('stop validation failed')
                session.commands.append(dict(stdout=str(receipt)))

            session.run = run
            session.start()
            with self.assertRaisesRegex(RuntimeError, 'stop validation failed'):
                session.stop()
            for name in ('game.stdout.log', 'game.stderr.log'):
                self.assertEqual((output/name).read_bytes(), (folder/name).read_bytes())
            self.assertTrue((output/'runtime-logs.json').is_file())

    def test_start_rejects_log_directory_outside_sessions(self):
        parent = (ROOT/'out/agent/sessions').resolve()
        with tempfile.TemporaryDirectory(dir=parent, prefix='vfx-log-test-') as temporary:
            output = Path(temporary).resolve()
            self.assertTrue(output.is_relative_to(parent))
            receipt = output/'start.stdout.log'
            receipt.write_text('session: '+str(ROOT/'Project/mailbox')+'\n')
            session = Session(output, 'DX12', 'fixture-hash')
            session.run = lambda *args, **kwargs: session.commands.append(dict(stdout=str(receipt)))
            with self.assertRaisesRegex(RuntimeError, 'outside the expected evidence'):
                session.start()
            self.assertIsNone(session.runtime_log_directory)


if __name__ == '__main__':
    unittest.main()
