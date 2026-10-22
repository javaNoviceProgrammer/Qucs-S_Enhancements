# a 1D semiconductor read along its line at points outside its domains (beside it, above, below)
import sys, json, contextlib, io
sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
with contextlib.redirect_stdout(io.StringIO()):
    from f6 import junction
for space in ("1d", "2d"):
    m = junction(space, [], "0.07" if space == "1d" else "0.05", {"h1d": "5[nm]"} if space == "1d" else {})
    pts = [[0.3, 0.1], [5, 0.1], [-40, 0.1], [0.3, 0.45], [0.3, -0.05], [1.0, 0.1], [1.001, 0.1]]
    calls = [('fem_solve', {'study': 's'})] + [('fem_evaluate', {'study': 's', 'expression': 'semi.V', 'at': [p]}) for p in pts]
    r = run(m, calls, tag='f74' + space, width=0)
    for p, (e, t) in zip(pts, r[1:]):
        try: j = json.loads(t); print(space, p, [x.get('value') for x in j['values']], j.get('note', '')[:150])
        except Exception: print(space, p, 'ERR' if e else 'TEXT', t[:200])
