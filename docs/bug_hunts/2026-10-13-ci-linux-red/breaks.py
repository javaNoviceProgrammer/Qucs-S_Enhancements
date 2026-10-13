#!/usr/bin/env python3
"""The CI hunt's changes broken on purpose (13 October 2026): each applied to the source,
test_fem_devices built in <repo>/build (the Release build), its tests run, the source
restored. Run from anywhere: python3 breaks.py [B1 B2 ...]."""
import os, subprocess, sys, tempfile
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
SRC = ROOT + '/qucs-s-26.1.1/qucs/multiphysics/fem_semiconductor.cpp'
SP = tempfile.mkdtemp(prefix='ci-hunt-')
for d in ('home', 'settings', 'trash', 'cache'):
    os.makedirs(os.path.join(SP, d), exist_ok=True)
env = dict(os.environ, HOME=SP + '/home', QUCS_SETTINGS_DIR=SP + '/settings', QUCS_TRASH_DIR=SP + '/trash', QUCS_CACHE_DIR=SP + '/cache')
TESTS = ['aRightAngleIsNoEdgeLeftOut', 'fowlerNordheim', 'exampleFowlerNordheim', 'aSweepStartsFromTheLast']
BREAKS = [
    ('B1 right angle: the cotangents alone', 'magnitude[std::size_t(em.edge[k])] += std::abs(em.cot[k]) + em.half;',
     'magnitude[std::size_t(em.edge[k])] += std::abs(em.cot[k]);'),
    ('B2 tolerance swallowing an obtuse angle', 'em.half = 0.5 * weight;', 'em.half = 0.5 * weight * 1e9;'),
    ('B3 FN: the first triangle\'s field', '                // Out of t where a falling coordinate reaches nought.\n',
     '                if (hop == 0) return std::abs(rate);\n                // Out of t where a falling coordinate reaches nought.\n'),
    ('B4 FN: the barrier\'s mean over half its potential', 'if (tau >= 0 && tau <= out) return fn.phi / (s + tau);',
     'if (tau >= 0 && tau <= out) return 0.5 * fn.phi / (s + tau);'),
    ('B5 FN thin: phi over the thickness', 'return s > 0 ? std::abs(moved) / s : 0.0;', 'return s > 0 ? fn.phi / s : 0.0;'),
    ('B6 FN: the normal into the semiconductor', 'if ((g.x() - m.x()) * d.x() + (g.y() - m.y()) * d.y() < 0) d = -d;',
     'if ((g.x() - m.x()) * d.x() + (g.y() - m.y()) * d.y() > 0) d = -d;'),
    ('B7 FN: a corner passed through ends the walk', 't = locate(t, here + d * (1e-6 * size));\n                    continue;',
     'break;'),
    ('B8 FN: V0 one end\'s', 'const double V0 = 0.5 * (v[std::size_t(fn.edges[k][0])] + v[std::size_t(fn.edges[k][1])]);',
     'const double V0 = v[std::size_t(fn.edges[k][0])];'),
    ('B9 FN thin: the signed change', 'return s > 0 ? std::abs(moved) / s : 0.0;', 'return s > 0 ? moved / s : 0.0;'),
]
only = sys.argv[1:]
orig = open(SRC, encoding='utf-8').read()
try:
    for name, old, new in BREAKS:
        if only and name.split()[0] not in only:
            continue
        assert orig.count(old) == 1, name
        open(SRC, 'w', encoding='utf-8').write(orig.replace(old, new))
        b = subprocess.run(['cmake', '--build', ROOT + '/build', '--target', 'test_fem_devices', '-j', '8'], capture_output=True, text=True)
        if b.returncode:
            print(name, 'BUILD FAILED', b.stdout[-2000:]); continue
        r = subprocess.run([ROOT + '/build/qucs/tests/test_fem_devices'] + TESTS, capture_output=True, text=True, env=env, timeout=3000)
        fails = [l.split('::')[1].split('(')[0] for l in r.stdout.splitlines() if l.startswith('FAIL!')]
        print(f'{name}: {"CAUGHT by " + ", ".join(fails) if fails else "NOT CAUGHT"}', flush=True)
finally:
    open(SRC, 'w', encoding='utf-8').write(orig)
    subprocess.run(['cmake', '--build', ROOT + '/build', '--target', 'test_fem_devices', '-j', '8'], capture_output=True, text=True)
    print('restored')
