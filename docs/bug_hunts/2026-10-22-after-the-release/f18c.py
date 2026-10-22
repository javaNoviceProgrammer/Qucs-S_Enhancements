import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
exec(open('/tmp/qucs-hunt/hunt2/f18b.py').read().split('for states')[0])
rows = {}
for edge in (False, True):
    sp = {"type": "spectrum", "energies": "linspace(1.78[eV], 1.88[eV], 11)", "states": 2, "linewidth": "30[meV]", "lineshape": "gaussian", "continuum": "states", "normalize": "stack"}
    if edge: sp.update({"bulkexcitons": True, "urbach": "0.5[meV]"})
    r = run(thin([sp]), [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'qw.alphaTE', 'all': True})], tag='f18c', width=0)
    rows[edge] = [round(x['SI'] / 100, 1) for x in json.loads(r[1][1])['values']]
print('E      ', [round(1.78 + 0.01 * k, 2) for k in range(11)]); print('states ', rows[False]); print('+edge  ', rows[True])
