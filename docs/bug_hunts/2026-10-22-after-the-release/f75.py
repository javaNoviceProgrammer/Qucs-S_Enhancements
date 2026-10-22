# a two-parameter sweep solved in parallel against in turn: the same values?
import sys, json, contextlib, io, time
sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
with contextlib.redirect_stdout(io.StringIO()):
    from f6 import junction
out = {}
for par in (0, 2, 4):
    m = junction("2d", [], "0.05", {})
    m['parameters'].append({"name": "N", "expression": "1e17[1/cm^3]"})
    m['components'][0]['physics'][0]['features'][0]['N0'] = 'N'
    sw = {"type": "sweep", "parameters": [["N", "1e16, 1e17, 1e18", "1/cm^3"], ["Va", "0, 0.3, 0.6, 0.9", "V"]]}
    if par: sw["parallel"] = par
    m['studies'][0]['steps'] = [sw, {"type": "stationary"}]
    t = time.time()
    with contextlib.redirect_stdout(io.StringIO()):
        r = run(m, [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'semi.n', 'at': [[0.3, 0.15]], 'all': True}),
                    ('fem_evaluate', {'study': 's', 'expression': 'semi.Ja', 'all': True})], tag='f75_%d' % par, width=0)
    vals = []
    for e, txt in r[1:]:
        try: vals += [(x.get('solution', x.get('name')), x['SI']) for x in json.loads(txt)['values']]
        except Exception: vals.append(('ERR', txt[:200]))
    out[par] = vals
    print('parallel', par, '%.1f s' % (time.time() - t), len(vals), 'values', r[0][1][:150] if r[0][0] else '')
worst = 0
for par in (2, 4):
    for (na, a), (nb, b) in zip(out[0], out[par]):
        if isinstance(a, float) and isinstance(b, float) and b != 0:
            d = abs(a / b - 1)
            if d > worst: worst = d; print('par', par, na, a, b, d)
        elif na != nb: print('MISMATCH', na, nb)
print('worst relative difference %.3g' % worst)
