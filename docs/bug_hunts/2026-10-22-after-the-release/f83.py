# fem_evaluate's "point" past a sweep's points, 0, negative, not a whole number
import sys, json, contextlib, io
sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
with contextlib.redirect_stdout(io.StringIO()):
    from f6 import junction
m = junction("2d", [], "0.05", {})
m['studies'][0]['steps'] = [{"type": "sweep", "parameters": [["Va", "0, 0.3, 0.6", "V"]]}, {"type": "stationary"}]
ps = [1, 2, 3, 4, 99, 0, -2, 2.7, "2"]
calls = [('fem_solve', {'study': 's'})] + [('fem_evaluate', {'study': 's', 'expression': 'semi.V', 'at': [[0.3, 0.1]], 'point': p}) for p in ps]
with contextlib.redirect_stdout(io.StringIO()):
    r = run(m, calls, tag='f83', width=0)
for p, (e, txt) in zip(ps, r[1:]):
    try:
        j = json.loads(txt); print('%-5r' % (p,), [(x.get('solution', x.get('name', ''))[:30], x['value']) for x in j['values']], j.get('note', '')[:120])
    except Exception: print('%-5r' % (p,), 'ERR' if e else 'TEXT', txt[:200])
