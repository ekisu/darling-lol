"""Failed Git inspections must stop source preparation."""
import ast
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('git'), 'Git required')
class PreparationInspectionTests(unittest.TestCase):
    def setUp(self):
        tree = ast.parse((Path(__file__).resolve().parents[1] / 'scripts/prepare.py').read_text())
        definitions = [node for node in tree.body if isinstance(node, ast.FunctionDef)
                       and node.name in ('submodules', 'pointers', 'repair')]
        self.namespace = {'subprocess': subprocess}
        exec(compile(ast.Module(body=definitions, type_ignores=[]), 'prepare.py', 'exec'),
             self.namespace)
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_failed_submodule_inspection_is_not_an_empty_list(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.namespace['submodules'](self.root)

    def test_failed_lfs_inspection_is_not_an_empty_list(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.namespace['pointers'](self.root)

    def test_failed_status_inspection_stops_repair(self):
        child = self.root / 'child'
        child.mkdir()
        self.namespace.update(submodules=lambda _root: [child], pointers=lambda _repo: [])
        with self.assertRaises(subprocess.CalledProcessError):
            self.namespace['repair'](self.root)


if __name__ == '__main__':
    unittest.main()
