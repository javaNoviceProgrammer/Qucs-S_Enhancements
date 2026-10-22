# dnwindow reversed, past the spectrum, of no width, one point wide
import sys, json, re, contextlib, io
sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
from f2 import well, spec
for w in (["1.50[eV]", "1.38[eV]"], ["1.0[eV]", "3.0[eV]"], ["1.45[eV]", "1.45[eV]"], ["1.45[eV]", "1.46[eV]"], ["2.5[eV]", "3[eV]"], ["1.38[eV]"]):
    m = well([{"name": "F", "expression": "0[V/m]"}], [{"type": "sweep", "parameters": [["F", "0, 1e7", "V/m"]]},
             dict(spec, energies="linspace(1.38[eV], 1.60[eV], 23)", dnwindow=w, conservearea=True)], {"F": "F"})
    with contextlib.redirect_stdout(io.StringIO()):
        r = run(m, [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'qw.dnTEloc', 'all': True})], tag='f77', width=0)
    e, t = r[0]
    if e: print(w, 'refused:', t[:200]); continue
    j = json.loads(t)
    ws = [x[:160] for x in j.get('warnings', []) if 'window' in x or 'local' in x or 'dnwindow' in x]
    try: vals = [x['value'] for x in json.loads(r[1][1])['values']]
    except Exception: vals = r[1][1][:150]
    print(w, 'warn', ws, 'dnTEloc', vals[:3] if isinstance(vals, list) else vals, '...', vals[-3:] if isinstance(vals, list) else '')
