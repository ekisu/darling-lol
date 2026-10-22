"""Compile Indium's submit worker (as patched) with a host fixture: present pacing, bounded waits, mutation controls."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/metal-present-pacing.cpp'
HEADERS = ('private-include/indium/submit-thread.private.hpp', 'private-include/indium/waits.private.hpp')

# Each mutant breaks one property the fixture checks; it must fail or hang.
MUTANTS = {
    'paced-without-wait': ('\t\t\twaitFor(previous, site);\n\t\t\treturn sequence;', '\t\t\treturn sequence;'),
    'paced-waits-own-frame': ('\t\t\twaitFor(previous, site);\n\t\t\treturn sequence;', '\t\t\twaitFor(sequence, site);\n\t\t\treturn sequence;'),
    'paced-waits-whole-queue': ('\t\t\twaitFor(previous, site);\n\t\t\treturn sequence;', '\t\t\tdrain(site);\n\t\t\treturn sequence;'),
    'wait-for-extended-by-later-tasks': ('\t\t\tif (onWorker() || target == 0) return;\n\t\t\tstd::unique_lock<std::mutex> lock(s.mutex);\n\t\t\tif (s.completed >= target) return;',
                                         '\t\t\tif (onWorker() || target == 0) return;\n\t\t\tstd::unique_lock<std::mutex> lock(s.mutex);\n\t\t\ttarget = s.posted;\n\t\t\tif (s.completed >= target) return;'),
    'drain-for-unbounded': ('const bool done = s.idle.wait_for(lock, timeout, [&] { return s.completed >= target; });',
                            's.idle.wait(lock, [&] { return s.completed >= target; }); const bool done = true;'),
}


def new_file(patch, path):
    """A file the patch creates, exactly as created."""
    marker = f'diff --git a/{path} b/{path}\nnew file mode 100644\n'
    section = patch.split(marker, 1)[1].split('\ndiff --git ', 1)[0]
    head, body = section.split('\n@@ ', 1)
    if '\n+++ b/' + path not in head or '\n@@ ' in body:
        raise AssertionError('expected one new-file hunk for ' + path)
    lines = body.split('\n', 1)[1].splitlines(keepends=True)
    if any(not line.startswith('+') for line in lines if line.strip('\n')):
        raise AssertionError('new-file hunk must only add lines: ' + path)
    return ''.join(line[1:] for line in lines)


def headers():
    patch = (ROOT / 'patches/metal--deps--indium.patch').read_text()
    return {path: new_file(patch, path) for path in HEADERS}


def build_and_run(folder, files, timeout=120):
    include = folder / 'include'
    for path, text in files.items():
        target = include / Path(path).relative_to('private-include')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    binary = folder / 'check'
    build = subprocess.run(['c++', '-std=c++17', '-O2', '-pthread', '-I', str(include), str(FIXTURE), '-o', str(binary)],
                           capture_output=True, text=True)
    if build.returncode:
        return 'compile', build.stderr
    try:
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 'hang', ''
    return ('ok' if run.returncode == 0 else 'failed'), run.stdout + run.stderr


@unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
class PresentPacingChecks(unittest.TestCase):
    def test_sequence_wait_for_paced_bound_and_bounded_exit_drain(self):
        with tempfile.TemporaryDirectory() as folder:
            status, output = build_and_run(Path(folder), headers())
            self.assertEqual(status, 'ok', output)
            self.assertIn('all ok', output)

    def test_fixture_rejects_each_broken_property(self):
        files = headers()
        worker = HEADERS[0]
        for name, (old, new) in MUTANTS.items():
            with self.subTest(mutant=name):
                self.assertEqual(files[worker].count(old), 1, 'mutation anchor must be unique')
                with tempfile.TemporaryDirectory() as folder:
                    status, output = build_and_run(Path(folder), dict(files, **{worker: files[worker].replace(old, new)}), timeout=30)
                self.assertIn(status, ('failed', 'hang'), output)


if __name__ == '__main__':
    unittest.main()
