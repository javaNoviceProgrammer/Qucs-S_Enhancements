#!/usr/bin/env python3
"""The CI hunt's interface rule (A4) broken on purpose: each break applied to the source,
test_fem_devices built in <repo>/build, aHeterojunctionsNodesWhateverTheirOrder run, the
source restored."""
import os, subprocess, sys, tempfile
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
SRC = ROOT + '/qucs-s-26.1.1/qucs/multiphysics/fem_semiconductor.cpp'
SP = tempfile.mkdtemp(prefix='ci-hunt-')
for d in ('home', 'settings', 'trash', 'cache'):
    os.makedirs(os.path.join(SP, d), exist_ok=True)
env = dict(os.environ, HOME=SP + '/home', QUCS_SETTINGS_DIR=SP + '/settings', QUCS_TRASH_DIR=SP + '/trash', QUCS_CACHE_DIR=SP + '/cache')
RULE = 'if (g < h - 1e-9 * std::abs(h) || (std::abs(g - h) <= 1e-9 * std::abs(h) && d < nd)) nd = d;'
BREAKS = [
    ('R1 the first triangle\'s', RULE, 'if (false) nd = d;'),
    ('R2 the wider gap\'s', RULE, 'if (g > h + 1e-9 * std::abs(h) || (std::abs(g - h) <= 1e-9 * std::abs(h) && d < nd)) nd = d;'),
    ('R3 ties: the first triangle\'s', RULE, 'if (g < h - 1e-9 * std::abs(h)) nd = d;'),
    ('R4 ties: the higher domain\'s', RULE, 'if (g < h - 1e-9 * std::abs(h) || (std::abs(g - h) <= 1e-9 * std::abs(h) && d > nd)) nd = d;'),
]
orig = open(SRC, encoding='utf-8').read()
try:
    for name, old, new in BREAKS:
        assert orig.count(old) == 1, name
        open(SRC, 'w', encoding='utf-8').write(orig.replace(old, new))
        b = subprocess.run(['cmake', '--build', ROOT + '/build', '--target', 'test_fem_devices', '-j', '8'], capture_output=True, text=True)
        if b.returncode:
            print(name, 'BUILD FAILED', b.stdout[-1500:]); continue
        r = subprocess.run([ROOT + '/build/qucs/tests/test_fem_devices', 'aHeterojunctionsNodesWhateverTheirOrder'], capture_output=True, text=True, env=env, timeout=3000)
        fails = [l[:200] for l in r.stdout.splitlines() if l.startswith('FAIL!')]
        print(f'{name}: {"CAUGHT: " + " | ".join(fails) if fails else "NOT CAUGHT"}', flush=True)
finally:
    open(SRC, 'w', encoding='utf-8').write(orig)
    subprocess.run(['cmake', '--build', ROOT + '/build', '--target', 'test_fem_devices', '-j', '8'], capture_output=True, text=True)
    print('restored')
