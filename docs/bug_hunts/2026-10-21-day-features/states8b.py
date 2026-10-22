import sys, os, json, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mq import Q, lines
HERE = os.path.dirname(os.path.abspath(__file__))
for states in ['8', '1', '40']:
    path = os.path.join(HERE, f'st8_{states}.qfem')
    open(path, 'w').write(open(os.path.join(HERE, 'miller_kp8.qfem')).read().replace('"states": 8', '"states": %s' % states))
    q = Q(path)
    t = time.time(); r = q.call('fem_solve', {'study': 'std2', 'timeout': 300}, 400)
    ev = q.call('fem_evaluate', {'study': 'std2', 'expression': 'qw.Ee1; qw.Ehh1; qw.Elh1; qw.Ee2'}, 120)
    v = [x.get('value') for x in ev.get('values', [])] if isinstance(ev, dict) and 'values' in ev else str(ev)[:160]
    print(f'kp8 States, states={states}: {time.time()-t:.1f} s', 'ERROR ' + str(r.get('error'))[:200] if 'error' in r else 'ok', v, [l[:110] for l in lines(r, 'warn')][:2], flush=True)
    q.close()
