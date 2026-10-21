import importlib.util
from contextlib import contextmanager
from pathlib import Path
import stat
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('install_archive', Path(__file__).with_name('InstallArchive.py'))
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


@contextmanager
def long_path_temporary():
    directory = tempfile.TemporaryDirectory()
    try:
        yield Path(directory.name)
    finally:
        directory.name = str(installer.native_path(directory.name))
        directory.cleanup()


class InstallArchiveTests(unittest.TestCase):
    def test_long_paths_and_runtime_merge(self):
        with long_path_temporary() as root:
            archive = root / 'tool.zip'
            relative = '/'.join(['long-directory-' + str(i) for i in range(18)]) + '/tool.txt'
            with zipfile.ZipFile(archive, 'w') as bundle:
                bundle.writestr('sdk/' + relative, 'verified tool')
            destination = root / 'installed'
            destination.mkdir()
            (destination / 'runtime.txt').write_text('retained')
            installer.install(archive, root / 'stage', destination, 'sdk')
            self.assertEqual((installer.native_path(destination) / relative).read_text(), 'verified tool')
            self.assertEqual((destination / 'runtime.txt').read_text(), 'retained')

    def test_unsafe_entries_fail_before_extraction(self):
        for name in ('../escape', '/absolute', 'C:/drive', 'folder\\..\\escape'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                with zipfile.ZipFile(root / 'bad.zip', 'w') as bundle:
                    bundle.writestr(name, 'bad')
                with self.assertRaises(ValueError):
                    installer.install(root / 'bad.zip', root / 'stage', root / 'installed')
                self.assertFalse((root / 'stage').exists())

    def test_symbolic_links_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entry = zipfile.ZipInfo('link')
            entry.create_system = 3
            entry.external_attr = (stat.S_IFLNK | 0o777) << 16
            with zipfile.ZipFile(root / 'bad.zip', 'w') as bundle:
                bundle.writestr(entry, '../outside')
            with self.assertRaises(ValueError):
                installer.install(root / 'bad.zip', root / 'stage', root / 'installed')


if __name__ == '__main__':
    unittest.main()
