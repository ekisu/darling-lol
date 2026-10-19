"""Compile the shipped address-table helper against an inert CPU-only fixture."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = 'private-include/indium/command-encoder.private.hpp'


def address_helper():
    patch = (ROOT / 'patches/metal--deps--indium.patch').read_text()
    marker = f'diff --git a/{HEADER} b/{HEADER}\n'
    section = patch.split(marker, 1)[1].split('\ndiff --git ', 1)[0]
    # Extract each postimage hunk separately: never silently join a source gap.
    hunks = re.split(r'(?m)^@@ [^\n]*\n', section)[1:]
    postimages = [''.join(line[1:] for line in hunk.splitlines(keepends=True)
                         if line.startswith((' ', '+'))) for hunk in hunks]

    def region(start, end):
        matches = [text[text.index(start):text.index(end, text.index(start))]
                   for text in postimages if start in text and end in text]
        if len(matches) != 1:
            raise AssertionError('expected one complete patched region: ' + start)
        return matches[0]

    return (region('    template<class T, size_t capacity = 64> class DescriptorScratch',
                   '    // Per-encoder binding tables') +
            region('\tstruct AddressPool {', '\n\ttemplate<size_t setCount>'))


@unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
class AddressShadowChecks(unittest.TestCase):
    def test_interleaving_fallback_padding_rollover_large_keys_and_allocations(self):
        fixture = (ROOT / 'tests/fixtures/indium-address-shadow.cpp').read_text()
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            source = path / 'check.cpp'
            source.write_text(fixture.replace('/* ACTUAL_PATCHED_HELPER */', address_helper()))
            build = subprocess.run(['c++', '-std=c++17', '-O1', str(source), '-o', str(path/'check')],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(path/'check')], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
