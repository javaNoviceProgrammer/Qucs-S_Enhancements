# the sum-rule check when the first point absorbs nothing in the window (Gaussian line, below the edge)
import sys, json, re, contextlib, io
sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
from f2 import well, spec
for lo, hi in (("1.30", "1.42"), ("1.20", "1.30")):
    m = well([{"name": "F", "expression": "0[V/m]"}], [{"type": "sweep", "parameters": [["F", "0, 4e7", "V/m"]]},
             dict(spec, energies="linspace(%s[eV], %s[eV], 13)" % (lo, hi), lineshape="gaussian", linewidth="2[meV]")], {"F": "F"})
    with contextlib.redirect_stdout(io.StringIO()):
        r = run(m, [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'qw.alphaTE', 'all': True})], tag='f78', width=0)
    e, t = r[0]
    if e: print(lo, hi, 'refused:', t[:200]); continue
    j = json.loads(t)
    print(lo, hi, 'warn', [x[:200] for x in j.get('warnings', []) if '∫' in x or 'sum' in x])
    v = json.loads(r[1][1])['values']; print('   max alpha per point', [max(x['SI'] for x in v if x['solution'].startswith(p)) for p in ('F = 0 ', 'F = 40')] if 'solution' in v[0] else [x['value'] for x in v][:26])
