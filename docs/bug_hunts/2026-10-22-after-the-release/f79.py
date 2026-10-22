# the spectrum's default plot over wavelengths and over a field sweep: its unit and expression
import sys, json, re, contextlib, io
sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
from f2 import well, spec
cases = {'nm': [dict(spec, energies="linspace(800[nm], 900[nm], 11)")],
         'gain': [dict(spec, energies="linspace(1.38[eV], 1.6[eV], 12)", density="2e18[1/cm^3]")],
         'field': [{"type": "sweep", "parameters": [["F", "0, 1e7", "V/m"]]}, dict(spec, energies="linspace(1.38[eV], 1.6[eV], 12)")]}
for name, steps in cases.items():
    m = well([{"name": "F", "expression": "0[V/m]"}], steps, {"F": "F"})
    with contextlib.redirect_stdout(io.StringIO()):
        r = run(m, [('fem_solve', {'study': 's'}), ('fem_model', {'action': 'save'})], tag='f79' + name, width=0)
    q = json.load(open(sys.path[0] + '/f79' + name + '.qfem'))
    def walk(n, depth=0):
        if isinstance(n, dict):
            if n.get('type') in ('plotgroup1d', 'globalgraph', 'plotgroup2d'): print('  ' * depth, name, n.get('type'), n.get('name'), n.get('expression'), n.get('unit'), n.get('xunit'), n.get('xaxis'))
            for v in n.values(): walk(v, depth + 1)
        elif isinstance(n, list):
            for v in n: walk(v, depth)
    walk(q.get('results', q))
