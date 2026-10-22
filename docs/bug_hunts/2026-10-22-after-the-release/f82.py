# B9's sibling: fem_evaluate's "time" on a transient - text it cannot read, a time past the run, no unit
import sys, json, contextlib, io, copy
sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
with contextlib.redirect_stdout(io.StringIO()):
    import f24
m = copy.deepcopy(f24.bar); m.pop('results')
m['studies'][0]['steps'][1]['times'] = "range(0, 25[ps], 0.5[ns])"
ts = ["0.25[ns]", "0.25 ns", "250ps", "x", "5[ns]", "0.25", "-1[ns]", "1[V]"]
calls = [('fem_solve', {'study': 's'})] + [('fem_evaluate', {'study': 's', 'expression': 'semi.I_2', 'time': t}) for t in ts]
with contextlib.redirect_stdout(io.StringIO()):
    r = run(m, calls, tag='f82', width=0)
for t, (e, txt) in zip(ts, r[1:]):
    try:
        j = json.loads(txt); print('%-9s' % t, [(x.get('name', '')[:40], x.get('solution', ''), x['value']) for x in j['values']], j.get('note', '')[:120])
    except Exception: print('%-9s' % t, 'ERR' if e else 'TEXT', txt[:200])
