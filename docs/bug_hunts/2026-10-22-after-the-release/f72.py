# a field whose drop over the structure exceeds the gap (10 nm at 1 GV/m: 10 V): the electron's state below the hole's
import sys, json
sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
m = json.load(open('fz/fz_2_8.qfem'))
m['studies'][0]['steps'] = [{"type": "sweep", "parameters": [["F", "0, 1e8, 3e8, 1e9", "V/m"]]},
    {"type": "spectrum", "energies": "linspace(1.2[eV], 1.8[eV], 7)", "states": 6, "linewidth": "2[meV]", "lineshape": "lorentzian", "continuum": "states"}]
r = run(m, [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'qw.Ee1hh1', 'all': True}),
            ('fem_evaluate', {'study': 's', 'expression': 'qw.alphaTE', 'all': True})], tag='f72', width=100)
j = json.loads(r[0][1]); print('WARNINGS:', [w[:300] for w in j.get('warnings', [])])
print([l[:200] for l in j.get('log', []) if 'arning' in l][:6])
for k in (1, 2):
    v = json.loads(r[k][1]); print([(x["name"], x["value"]) for x in v["values"] if k == 1 or "E = 1.4 eV" in x["name"]])
