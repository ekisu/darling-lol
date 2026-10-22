"""Startup configuration of the Metal/Indium switches: the shipped helper on the host, and no recurring lookups left."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/metal-startup-config.cpp'
HEADER = 'private-include/indium/startup-config.private.hpp'

# Former guest files that were re-read while the game ran, and their startup variables.
CONVERTED = {
    '/tmp/metal-async-no-frame-sync': 'DARLING_METAL_ASYNC_NO_FRAME_SYNC',
    '/tmp/metal-no-binding-reuse': 'DARLING_METAL_NO_BINDING_REUSE',
    '/tmp/metal-retire-tiny': 'DARLING_METAL_RETIRE_TINY',
    '/tmp/metal-no-submit-batch': 'DARLING_METAL_NO_SUBMIT_BATCH',
    '/tmp/metal-submit-batch-us': 'DARLING_METAL_SUBMIT_BATCH_US',
    '/tmp/metal-submit-batch-max': 'DARLING_METAL_SUBMIT_BATCH_MAX',
    '/tmp/metal-no-lazy-wakeup': 'DARLING_METAL_NO_LAZY_WAKEUP',
    '/tmp/metal-async-uploads': 'DARLING_METAL_ASYNC_UPLOADS',
    '/tmp/metal-no-lazy-pass': 'DARLING_METAL_NO_LAZY_PASS',
    '/tmp/metal-async-encode': 'DARLING_METAL_ASYNC_ENCODE',
    '/tmp/metal-no-async-encode': 'DARLING_METAL_NO_ASYNC_ENCODE',
    '/tmp/metal-outline-pairs': 'DARLING_METAL_OUTLINE_PAIRS',
    '/tmp/metal-outline-pairs2': 'DARLING_METAL_OUTLINE_PAIRS2',
    **{f'/tmp/metal-skip-{n}': f'DARLING_METAL_SKIP_{n.upper()}' for n in ('1180', '512', '1024', '1080', '242', 'ui', 'scene')},
}

MUTANTS = {
    'zero-means-on': ("return value[0] == '0' && value[1] == '\\0' ? 0 : 1;", 'return 1;'),
    'empty-env-wins': ('return value && *value ? value : nullptr;', 'return value;'),
    'no-clamp': ('out = value < low ? low : value > high ? high : value;', 'out = value;'),
    'out-of-range-accepted': ('if (!clamp && (value < low || value > high)) return false;', ''),
    'overflow-accepted': ('if (end == text || errno != 0) return false;', 'if (end == text) return false;'),
    'trailing-junk-accepted': ("if (*end != '\\0') return false;", ''),
    'activation-on-ignores-off-file': ('if (defaultOn) return !(offFile && access(offFile, F_OK) == 0);', 'if (defaultOn) return true;'),
    'activation-off-ignores-on-file': ('return onFile && access(onFile, F_OK) == 0;', 'return false;'),
}


def postimage(patch):
    """{path: [postimage lines of the patch's hunks]}, i.e. context and added lines per file section."""
    files = {}
    for section in patch.split('\ndiff --git ')[0:]:
        match = re.match(r'(?:diff --git )?a/(\S+) b/(\S+)\n', section)
        if not match:
            continue
        lines = files.setdefault(match.group(2), [])
        for hunk in re.split(r'(?m)^@@ [^\n]*\n', section)[1:]:
            lines += [line[1:] for line in hunk.splitlines() if line.startswith((' ', '+'))]
    return files


def new_file(patch, path):
    marker = f'diff --git a/{path} b/{path}\nnew file mode 100644\n'
    section = patch.split(marker, 1)[1].split('\ndiff --git ', 1)[0]
    head, body = section.split('\n@@ ', 1)
    lines = body.split('\n', 1)[1].splitlines(keepends=True)
    if '\n@@ ' in body or any(not line.startswith('+') for line in lines if line.strip('\n')):
        raise AssertionError('expected one new-file hunk for ' + path)
    return ''.join(line[1:] for line in lines)


def build_and_run(folder, header, timeout=60):
    target = folder / 'include/indium/startup-config.private.hpp'
    target.parent.mkdir(parents=True)
    target.write_text(header)
    binary = folder / 'check'
    build = subprocess.run(['c++', '-std=c++17', '-O1', '-I', str(folder / 'include'), str(FIXTURE), '-o', str(binary)],
                           capture_output=True, text=True)
    if build.returncode:
        return 'compile', build.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=timeout)
    return ('ok' if run.returncode == 0 else 'failed'), run.stdout + run.stderr


class PatchedSources(unittest.TestCase):
    def setUp(self):
        self.files = {}
        for patch in ('metal--deps--indium.patch', 'metal.patch'):
            self.files.update(postimage((ROOT / 'patches' / patch).read_text()))

    def test_converted_switches_are_only_read_through_startup_config(self):
        for path, variable in CONVERTED.items():
            uses = [(name, line) for name, lines in self.files.items() for line in lines if f'"{path}"' in line]
            with self.subTest(path=path):
                self.assertTrue(uses, 'switch not found in the patched sources')
                for name, line in uses:
                    self.assertIn(f'"{variable}"', line, f'{name}: {line.strip()}')
                    self.assertRegex(line, r'\b(flag|number)\(', f'{name}: {line.strip()}')
                    self.assertNotRegex(line, r'\b(access|fopen)\(', f'{name}: {line.strip()}')

    def test_no_counter_driven_filesystem_polls_remain(self):
        # a counter test (`& 255) == 0`, `% 2048 == 0`) followed within two lines by an access()/fopen() of /tmp
        offenders = []
        for name, lines in self.files.items():
            for i, line in enumerate(lines):
                if re.search(r'(&\s*\d+\)\s*==\s*0|%\s*\d+\s*==\s*0)', line):
                    window = ' '.join(lines[i:i + 3])
                    if re.search(r'\b(access|fopen)\("/tmp/metal-', window) and 'polling &&' not in line and \
                            not re.search(r'calls\.fetch_add\(1\) % (100|200) == 0|calls\+\+ & 255\) == 0\) active = access\("/tmp/metal-seqlog"', line):
                        offenders.append(f'{name}: {line.strip()}')
        # left on purpose: the late frame-interval capture trigger (once per 128 presenting frames, only until it fires)
        offenders = [o for o in offenders if 'checks.fetch_add(1, std::memory_order_relaxed) % 128' not in o]
        self.assertEqual(offenders, [])

    def test_diagnostic_trigger_polls_need_a_startup_diagnostic(self):
        # draw-limit and state-flag experiments: polled only when /tmp/metal-debug-draws or the debug trace was on at startup
        encoder = self.files['src/Metal/MTLRenderCommandEncoder.mm']
        polls = [i for i, line in enumerate(encoder) if re.search(r'calls\.fetch_add\(1\) % (100|200) == 0', line)]
        self.assertEqual(len(polls), 2)
        for i in polls:
            window = '\n'.join(encoder[i - 4:i])
            self.assertIn('static const bool possible = access("/tmp/metal-debug-draws", F_OK) == 0 || MTLDebugEnabled();', window)
            self.assertIn('if (!possible) {', window)
        # the sequence-log trigger: only while GPU timing (read once) is on
        render = self.files['src/indium/render-command-encoder.cpp']
        i = next(i for i, line in enumerate(render) if 'access("/tmp/metal-seqlog", F_OK)' in line)
        self.assertIn('if (!Indium::GpuTiming::enabled()) return false;', '\n'.join(render[i - 4:i]))

    def test_snapshot_trigger_is_polled_only_when_enabled_at_startup(self):
        sites = [(name, i, lines) for name, lines in self.files.items() for i, line in enumerate(lines)
                 if 'fopen("/tmp/metal-snapshot", "r")' in line]
        self.assertEqual(len(sites), 2)
        for name, i, lines in sites:
            window = '\n'.join(lines[max(0, i - 4):i])
            self.assertIn('Indium::StartupConfig::flag("DARLING_METAL_SNAPSHOT", nullptr)', window, name)
            self.assertIn('if (polling &&', window, name)

    def test_frame_end_sync_switch_is_cached(self):
        command_buffer = '\n'.join(self.files['src/indium/command-buffer.cpp'])
        self.assertEqual(command_buffer.count('!frameSyncDisabled()'), 3)
        self.assertNotIn('access("/tmp/metal-async-no-frame-sync"', command_buffer)


@unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
class HelperChecks(unittest.TestCase):
    def header(self):
        return new_file((ROOT / 'patches/metal--deps--indium.patch').read_text(), HEADER)

    def test_precedence_parsing_clamping_and_single_lookup_per_site(self):
        with tempfile.TemporaryDirectory() as folder:
            status, output = build_and_run(Path(folder), self.header())
        self.assertEqual(status, 'ok', output)
        self.assertIn('all ok', output)

    def test_fixture_rejects_each_broken_rule(self):
        header = self.header()
        for name, (old, new) in MUTANTS.items():
            with self.subTest(mutant=name):
                self.assertEqual(header.count(old), 1, 'mutation anchor must be unique')
                with tempfile.TemporaryDirectory() as folder:
                    status, output = build_and_run(Path(folder), header.replace(old, new))
                self.assertEqual(status, 'failed', output)


if __name__ == '__main__':
    unittest.main()


ACTIVATION_DRIVER = r'''
#include <cstdio>
#include <indium/startup-config.private.hpp>
namespace Indium { bool directPresentationRequested(); bool directAsyncCommitRequested(); }
/* ACTUAL_PATCHED_ACTIVATION */
int main() { printf("%d %d\n", (int) Indium::directPresentationRequested(), (int) Indium::directAsyncCommitRequested()); return 0; }
'''


def activation_source(patch, folder):
    """The two activation functions exactly as patched into indium.cpp, with their guest files moved to `folder`."""
    lines = postimage(patch)['src/indium/indium.cpp']
    text = '\n'.join(lines)
    start = text.index('bool Indium::directPresentationRequested() {')
    end = text.index('\n}\n', text.index('bool Indium::directAsyncCommitRequested() {')) + 3
    region = text[start:end]
    for name in ('metal-direct-present', 'metal-no-direct-present', 'metal-direct-async-commit', 'metal-no-direct-async-commit'):
        assert region.count(f'"/tmp/{name}"') == 1, name
        region = region.replace(f'"/tmp/{name}"', f'"{folder}/{name}"')
    return region


@unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
class DirectActivationChecks(unittest.TestCase):
    """DARLING_METAL_DIRECT_PRESENT / DARLING_METAL_DIRECT_ASYNC_COMMIT over the guest files and the build defaults."""

    def run_case(self, binary, folder, env=None, files=()):
        for name in ('metal-direct-present', 'metal-no-direct-present', 'metal-direct-async-commit', 'metal-no-direct-async-commit'):
            (folder / name).unlink(missing_ok=True)
        for name in files:
            (folder / name).write_text('')
        environment = {k: v for k, v in __import__('os').environ.items() if not k.startswith('DARLING_METAL_')}
        environment.update(env or {})
        out = subprocess.run([str(binary)], env=environment, capture_output=True, text=True, timeout=30)
        self.assertEqual(out.returncode, 0, out.stderr)
        return tuple(int(x) for x in out.stdout.split())

    def test_environment_files_and_build_defaults(self):
        header = new_file((ROOT / 'patches/metal--deps--indium.patch').read_text(), HEADER)
        patch = (ROOT / 'patches/metal--deps--indium.patch').read_text()
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / 'include/indium').mkdir(parents=True)
            (folder / 'include/indium/startup-config.private.hpp').write_text(header)
            (folder / 'main.cpp').write_text(ACTIVATION_DRIVER.replace('/* ACTUAL_PATCHED_ACTIVATION */', activation_source(patch, folder)))
            binaries = {}
            for build, defines in (('off', []), ('on', ['-DINDIUM_DIRECT_PRESENT_DEFAULT_ON=1', '-DINDIUM_DIRECT_ASYNC_COMMIT_DEFAULT_ON=1'])):
                binaries[build] = folder / ('check-' + build)
                built = subprocess.run(['c++', '-std=c++17', *defines, '-I', str(folder / 'include'), str(folder / 'main.cpp'),
                                        '-o', str(binaries[build])], capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr)
            P, A = 'DARLING_METAL_DIRECT_PRESENT', 'DARLING_METAL_DIRECT_ASYNC_COMMIT'
            cases = [
                # ordinary build (default off): what tools/run-riot-client.sh enables with 1 and turns off with 0
                ('off', {}, (), (0, 0)),
                ('off', {P: '1', A: '1'}, (), (1, 1)),
                ('off', {P: '1', A: '0'}, (), (1, 0)),
                ('off', {P: '0', A: '0'}, ('metal-direct-present', 'metal-direct-async-commit'), (0, 0)),
                ('off', {P: '', A: ''}, ('metal-direct-present',), (1, 0)),
                ('off', {}, ('metal-direct-present', 'metal-direct-async-commit'), (1, 1)),
                # default-on benchmark build
                ('on', {}, (), (1, 1)),
                ('on', {P: '0', A: '0'}, (), (0, 0)),
                ('on', {}, ('metal-no-direct-present',), (0, 1)),
                ('on', {P: '1'}, ('metal-no-direct-present', 'metal-no-direct-async-commit'), (1, 0)),
                ('on', {P: ''}, ('metal-no-direct-present',), (0, 1)),
            ]
            for build, env, files, expected in cases:
                with self.subTest(build=build, env=env, files=files):
                    self.assertEqual(self.run_case(binaries[build], folder, env, files), expected)
