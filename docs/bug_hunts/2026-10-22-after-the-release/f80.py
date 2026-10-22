# pmin (the held states' least probability in the well) out of range under continuum bound, in a field
import sys, json, re, contextlib, io
sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
from f2 import well, spec
for p in (sys.argv[2],):
    m = well([{"name": "F", "expression": sys.argv[1]}], [dict(spec, energies="linspace(1.40[eV], 1.60[eV], 5)", pmin=p)], {"F": "F"})
    with contextlib.redirect_stdout(io.StringIO()):
        r = run(m, [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'qw.alphaTE', 'all': True})], tag='f80', width=0)
    e, t = r[0]
    if e: print('%-5s refused: %s' % (p, t[:200])); continue
    j = json.loads(t)
    ws = [x[:300] for x in j.get('warnings', []) + j.get('log', []) if 'held' in x or 'bound' in x or 'pmin' in x or 'share' in x]
    try: vals = [x['value'] for x in json.loads(r[1][1])['values']]
    except Exception: vals = r[1][1][:100]
    print('%-5s solved: alpha %s warn %s' % (p, vals, ws))
