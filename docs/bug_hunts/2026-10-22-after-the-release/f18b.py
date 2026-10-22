import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
def thin(steps):
    return {"format": "qucs-s multiphysics 1",
     "components": [{"unit": "nm",
        "geometry": [{"type": "rectangle", "name": "below", "size": ["10", "50"]},
                     {"type": "rectangle", "name": "well", "position": ["0", "50"], "size": ["10", "2"]},
                     {"type": "rectangle", "name": "above", "position": ["0", "52"], "size": ["10", "50"]}],
        "materials": [{"material": "barrier", "compound": "AlGaAs", "x": "0.3", "selection": {"only": ["below", "above"]}},
                      {"material": "well", "compound": "GaAs", "x": "0", "selection": {"only": ["well"]}}],
        "physics": [{"type": "quantum", "tag": "qw", "h1d": "0.5[nm]"}], "mesh": {"size": "normal", "custom": True, "hmax": "5"}}],
     "studies": [{"type": "study", "tag": "s", "steps": steps}]}
for states, lw, ub in ((1, "20[meV]", "1[meV]"), (2, "30[meV]", "0.5[meV]"), (40, "20[meV]", "1[meV]")):
    sp = {"type": "spectrum", "energies": "linspace(1.65[eV], 2.0[eV], 36)", "states": states, "linewidth": lw, "lineshape": "gaussian",
          "continuum": "states", "bulkexcitons": True, "urbach": ub, "normalize": "stack"}
    r = run(thin([sp]), [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'qw.alphaTE', 'all': True})], tag='f18b', width=0)
    if r[0][0]: print(r[0][1][:300]); continue
    v = json.loads(r[1][1])['values']
    neg = [(x['name'].split('E = ')[1].split(':')[0], round(x['SI'] / 100, 1)) for x in v if x['SI'] < 0]
    print('states %d, line %s, urbach %s: min %.1f /cm; negative at %d of %d %s' % (states, lw, ub, min(x['SI'] for x in v) / 100, len(neg), len(v), neg[:5]))
