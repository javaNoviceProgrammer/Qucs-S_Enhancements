import sys, json, re; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
def edge(first, steps):
    geo = ([{"type": "rectangle", "name": "well", "size": ["10", "9.4"]},
            {"type": "rectangle", "name": "above", "position": ["0", "9.4"], "size": ["10", "16"]}] if first == 'well' else
           [{"type": "rectangle", "name": "above", "size": ["10", "16"]},
            {"type": "rectangle", "name": "well", "position": ["0", "16"], "size": ["10", "9.4"]}])
    return {"format": "qucs-s multiphysics 1",
     "components": [{"unit": "nm", "geometry": geo,
        "materials": [{"material": "barrier", "compound": "AlGaAs", "x": "0.3", "selection": {"only": ["above"]}},
                      {"material": "well", "compound": "GaAs", "x": "0", "selection": {"only": ["well"]}}],
        "physics": [{"type": "quantum", "tag": "qw", "h1d": "0.25[nm]"}], "mesh": {"size": "normal", "custom": True, "hmax": "2"}}],
     "studies": [{"type": "study", "tag": "s", "steps": steps}]}
spec = {"type": "spectrum", "energies": "1.40[eV], 1.44[eV], 1.50[eV], 1.60[eV], 1.75[eV], 1.85[eV]", "states": 12, "linewidth": "5.5[meV]", "lineshape": "gaussian"}
for first in ('well', 'barrier'):
    for name, extra in (('states', {"continuum": "states"}), ('states+edge', {"continuum": "states", "bulkexcitons": True, "urbach": "6[meV]"}),
                        ('bound', {"continuum": "bound"})):
        r = run(edge(first, [dict(spec, **extra)]), [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'qw.alphaTE', 'all': True})], tag='f4', width=0)
        v = json.loads(r[1][1]); 
        print('%-8s first, %-12s' % (first, name), [round(x['SI'] / 1e5, 2) for x in v['values']], '(1e3/cm at 1.40 1.44 1.50 1.60 1.75 1.85)')
        print('   ', [l[:160] for l in json.loads(r[0][1])['log'] if 'gap' in l or 'bulk' in l][:2])
