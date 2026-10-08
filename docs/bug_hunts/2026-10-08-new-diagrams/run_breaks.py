"""Each fix of the hunt undone, one at a time: the test given must fail. The source restored after each."""
import os, subprocess, sys, json, time
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../..'))
Q = ROOT + '/qucs-s-26.1.1/qucs/'
BUILD = ROOT + '/build'
ENV = dict(os.environ, QT_QPA_PLATFORM='offscreen', QUCS_CLAUDE='/nonexistent/claude', QUCS_GH='/nonexistent/gh')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from breaks import BREAKS
only = sys.argv[1:]
results = []
for b in BREAKS:
    name, path, old, new, target, functions = b
    if only and name not in only: continue
    f = Q + path
    src = open(f).read()
    if src.count(old) < 1:
        results.append((name, 'OLD NOT FOUND')); print(name, 'OLD NOT FOUND', flush=True); continue
    try:
        open(f, 'w').write(src.replace(old, new, 1))
        b_ = subprocess.run(['cmake', '--build', BUILD, '--target', target], capture_output=True, text=True)
        if b_.returncode != 0:
            results.append((name, 'BUILD FAILED')); print(name, 'BUILD FAILED', b_.stdout[-800:], flush=True); continue
        caught = []
        for fn in functions:
            t = subprocess.run([f'{BUILD}/qucs/tests/{target}', fn], capture_output=True, text=True, env=ENV, timeout=600)
            caught.append(t.returncode != 0)
            fails = [l for l in t.stdout.splitlines() if l.startswith('FAIL!')]
        status = 'caught' if any(caught) else 'NOT CAUGHT'
        results.append((name, status)); print(name, status, (fails[:1] if fails else ''), flush=True)
    finally:
        open(f, 'w').write(src)
subprocess.run(['cmake', '--build', BUILD], capture_output=True, text=True)
print(json.dumps(results))
print('caught', sum(1 for r in results if r[1] == 'caught'), 'of', len(results))
