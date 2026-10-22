import sys, os, json, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mq import Q, lines
HERE = os.path.dirname(os.path.abspath(__file__))
# 1. The Miller model's States study (its quantum node's valence as the file has it), levels.
path = os.path.join(HERE, 'st_miller.qfem'); open(path, 'w').write(open(sys.argv[1]).read())
q = Q(path)
t = time.time(); r = q.call('fem_solve', {'study': 'std2', 'timeout': 300}, 400)
print(f'Miller States: {time.time()-t:.1f} s', 'ERROR ' + str(r.get('error'))[:200] if 'error' in r else 'ok', [l[:120] for l in lines(r, 'warn', 'k·p', 'kp')][:3], flush=True)
ev = q.call('fem_evaluate', {'study': 'std2', 'expression': 'qw.Ee1; qw.Ehh1; qw.Elh1'}, 120)
print('  levels', [v.get('value') for v in ev.get('values', [])] if 'values' in ev else str(ev)[:200], flush=True)
q.close()
# 2. A 2D quantum wire with periodic ends.
src = json.load(open('/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples/multiphysics/quantum_wire.qfem'))
for comp in src['components']:
    for ph in comp.get('physics', []):
        if ph.get('type') == 'quantum': ph['ends'] = 'periodic'
path = os.path.join(HERE, 'wire_periodic.qfem'); json.dump(src, open(path, 'w'))
q = Q(path)
r = q.call('fem_solve', {'timeout': 300}, 400)
print('2D wire, ends periodic:', 'ERROR ' + str(r.get('error'))[:200] if 'error' in r else 'ok', [l[:140] for l in lines(r, 'warn', 'periodic', 'ends')][:3], flush=True)
q.close()
