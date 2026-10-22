"""Compile the shipped direct-presentation core against a fake WSI, and check that the fixture catches broken variants."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = 'QuartzCore/CAMetalDirectPresent.h'
FIXTURE = ROOT / 'tests/fixtures/metal-direct-present.cpp'

# Each mutant breaks one rule the core relies on; the fixture must fail (or hang) for every one.
MUTANTS = {
    'retire-while-held': ('\t\t\tif (_held != 0) return false;\n\t\t\tchain = _chain;', '\t\t\tchain = _chain;'),
    'retire-without-present-fences': ('if (!_platform.presentsComplete(*chain) || !_platform.imagesIdle(*chain, spare)) return false;',
                                      'if (!_platform.imagesIdle(*chain, spare)) return false;'),
    'retire-with-busy-spare': ('if (!_platform.presentsComplete(*chain) || !_platform.imagesIdle(*chain, spare)) return false;',
                               'if (!_platform.presentsComplete(*chain)) return false;'),
    'no-held-bound': ('\t\t\tif (_held >= _maxHeld) {', '\t\t\tif (false) {'),
    'abandon-loses-spare': ('\t\t\tif (image.chain == _chain) {\n\t\t\t\t_spare.push_back(image.index);',
                            '\t\t\tif (image.chain == _chain) {\n\t\t\t\t(void) 0;'),
    'present-keeps-held': ('\t\t\tstd::lock_guard<std::mutex> lock(_mutex);\n\t\t\t--_held;\n\t\t\tif (status == Status::Ok) {',
                           '\t\t\tstd::lock_guard<std::mutex> lock(_mutex);\n\t\t\tif (status == Status::Ok) {'),
    'present-no-wakeup': ('\t\t}\n\t\t_condition.notify_all();\n\t\treturn status;', '\t\t}\n\t\treturn status;'),
    'no-activating-wait': ('\t\t\t\t// Legacy frames drain first; the main thread activates or gives up.\n',
                           '\t\t\t\treturn AcquireResult::NotActive;\n'),
    'present-failure-keeps-active': ('\t\t\t\t++_counters.presentFailures;\n\t\t\t\tif (image.chain == _chain) revokeLocked();',
                                     '\t\t\t\t++_counters.presentFailures;'),
    'acquire-ignores-revoke': ('\t\t\tif (_state != State::Active || Clock::now() >= deadline) {', '\t\t\tif (Clock::now() >= deadline) {'),
}


def presenter_header():
    """The new header exactly as the patch creates it."""
    patch = (ROOT / 'patches/cocotron.patch').read_text()
    marker = f'diff --git a/{HEADER} b/{HEADER}\nnew file mode 100644\n'
    section = patch.split(marker, 1)[1].split('\ndiff --git ', 1)[0]
    hunks = section.split('\n@@ ')
    if len(hunks) != 2 or not hunks[0].startswith('index ') or '\n+++ b/' + HEADER not in hunks[0]:
        raise AssertionError('expected one new-file hunk for ' + HEADER)
    body = hunks[1].split('\n', 1)[1]
    lines = body.splitlines(keepends=True)
    if any(not line.startswith('+') for line in lines if line.strip('\n')):
        raise AssertionError('new-file hunk must only add lines')
    return ''.join(line[1:] if line.startswith('+') else line for line in lines)


def build_and_run(folder, header, iterations='2', timeout=120):
    (folder / Path(HEADER).name).write_text(header)
    binary = folder / 'check'
    build = subprocess.run(['c++', '-std=c++17', '-O2', '-I', str(folder), str(FIXTURE), '-o', str(binary), '-lpthread'],
                           capture_output=True, text=True)
    if build.returncode:
        return 'compile', build.stderr
    try:
        run = subprocess.run([str(binary), iterations], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 'hang', ''
    return ('ok' if run.returncode == 0 else 'failed'), run.stdout + run.stderr


@unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
class DirectPresentChecks(unittest.TestCase):
    def test_presenter_ownership_retirement_activation_and_stress(self):
        with tempfile.TemporaryDirectory() as folder:
            status, output = build_and_run(Path(folder), presenter_header(), iterations='3')
            self.assertEqual(status, 'ok', output)
            self.assertIn('all ok', output)

    def test_fixture_rejects_each_broken_rule(self):
        header = presenter_header()
        for name, (old, new) in MUTANTS.items():
            with self.subTest(mutant=name):
                self.assertEqual(header.count(old), 1, 'mutation anchor must be unique')
                with tempfile.TemporaryDirectory() as folder:
                    status, output = build_and_run(Path(folder), header.replace(old, new), timeout=60)
                self.assertIn(status, ('failed', 'hang'), output)


if __name__ == '__main__':
    unittest.main()
