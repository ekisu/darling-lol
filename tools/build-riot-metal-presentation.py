#!/usr/bin/env python3
"""Build the Metal layer presentation bridge and its window regression check."""
from pathlib import Path
import json
import shlex
import subprocess
root = Path(__file__).resolve().parent.parent
config_path = root / 'build/config.json'
sdk = json.loads(config_path.read_text())['sdk'] if config_path.exists() else '/usr/local/libexec/darling'
folder = root / 'build/darling-src/build/src/external/cocotron/QuartzCore'
source = root / 'build/darling-src/src/external/cocotron/QuartzCore'
flags = {}
for line in (folder / 'CMakeFiles/QuartzCore.dir/flags.make').read_text().splitlines():
    if ' = ' in line:
        key, value = line.split(' = ', 1)
        flags[key] = shlex.split(value)
resource_include = subprocess.check_output(['clang++', '-print-resource-dir'], text=True).strip() + '/include'
for key in ('C_INCLUDES', 'CXX_INCLUDES'):
    flags[key] = [resource_include if value.startswith('/usr/lib/clang/') and value.endswith('/include') else value for value in flags[key]]
link = shlex.split((folder / 'CMakeFiles/QuartzCore.dir/link.txt').read_text())
for name, suffix in [('CALayer', 'm'), ('CALayerContext', 'm'), ('CATransaction', 'm'), ('CAMetalLayer', 'mm'), ('CAMetalDrawable', 'mm'),
                     ('CAMetalDirectPresent', 'mm')]:
    kind = 'CXX' if suffix == 'mm' else 'C'
    obj = root / f'build/riot-quartzcore-{name}.o'
    subprocess.run(['clang++' if kind == 'CXX' else 'clang', *flags[kind + '_DEFINES'],
                    *flags[kind + '_INCLUDES'], *flags[kind + '_FLAGS'], '-O2',
                    '-Wno-macro-redefined', '-c', str(source / (name + '.' + suffix)), '-o', str(obj)], check=True)
    target = 'CMakeFiles/QuartzCore.dir/' + name + '.' + suffix + '.o'
    # A build tree configured before the source was added does not list it yet.
    if target in link:
        link[link.index(target)] = str(obj)
    else:
        link.insert(link.index('-o'), str(obj))
link[link.index('-o') + 1] = str(root / 'build/riot-QuartzCore')
subprocess.run(link, cwd=folder, check=True)
for name in ('riot-metal-presentation-check', 'riot-ca-render-policy-check'):
    obj = root / f'build/{name}.o'
    subprocess.run(['clang++', *flags['CXX_DEFINES'], *flags['CXX_INCLUDES'], *flags['CXX_FLAGS'],
                    '-c', str(root / f'tools/{name}.mm'), '-o', str(obj)], check=True)
    subprocess.run(['clang++', '-target', 'x86_64-apple-macosx11.0', '-fuse-ld=lld', '-nostdlib',
                    '-isysroot', sdk, str(obj), '-lSystem', '-lc++', '-lobjc',
                    '-framework', 'AppKit', '-framework', 'OpenGL', '-framework', 'Foundation',
                    '-framework', 'IOSurface', str(root / 'build/riot-Metal'),
                    str(root / 'build/riot-QuartzCore'), '-o', str(root / f'build/{name}')], check=True)
