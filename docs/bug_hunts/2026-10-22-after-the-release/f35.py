import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
def asym(lower, upper, steps):
    return {"format": "qucs-s multiphysics 1",
     "components": [{"unit": "nm",
        "geometry": [{"type": "rectangle", "name": "below", "size": ["10", "30"]},
                     {"type": "rectangle", "name": "well", "position": ["0", "30"], "size": ["10", "8"]},
                     {"type": "rectangle", "name": "above", "position": ["0", "38"], "size": ["10", "30"]}],
        "materials": [{"material": "b1", "compound": "AlGaAs", "x": lower, "selection": {"only": ["below"]}},
                      {"material": "b2", "compound": "AlGaAs", "x": upper, "selection": {"only": ["above"]}},
                      {"material": "well", "compound": "GaAs", "x": "0", "selection": {"only": ["well"]}}],
        "physics": [{"type": "quantum", "tag": "qw", "h1d": "0.5[nm]"}], "mesh": {"size": "normal", "custom": True, "hmax": "3"}}],
     "studies": [{"type": "study", "tag": "s", "steps": steps}]}
E = "1.50[eV], 1.58[eV], 1.65[eV], 1.72[eV], 1.78[eV], 1.85[eV]"
for lo, up in (("0.3", "0.1"), ("0.1", "0.3")):
    for cont in ("bound", "states"):
        sp = {"type": "spectrum", "energies": E, "states": 30, "linewidth": "5[meV]", "lineshape": "gaussian", "continuum": cont, "normalize": "stack"}
        r = run(asym(lo, up, [sp]), [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'qw.alphaTE', 'all': True})], tag='f35', width=0)
        if r[0][0]: print(lo, up, cont, r[0][1][:200]); continue
        print('below x %s, above x %s, %-6s' % (lo, up, cont), [round(x['SI'] / 100) for x in json.loads(r[1][1])['values']], '/cm at', E)
