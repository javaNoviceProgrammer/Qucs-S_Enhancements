import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run, H
from f2 import well, spec
for cons in (False, True):
    m = well([{"name": "F", "expression": "0[V/m]"}], [{"type": "sweep", "parameters": [["F", "0, 4e7", "V/m"]]}, dict(spec, energies="linspace(1.38[eV], 1.95[eV], 115)", dnwindow=["1.38[eV]", "1.44[eV]"], conservearea=cons)], {"F": "F"})
    r = run(m, [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'qw.alphaTM', 'frequency': '1.42[eV]', 'point': 1}),
                ('fem_evaluate', {'study': 's', 'expression': 'qw.alphaTM', 'frequency': '1.42[eV]', 'point': 2}),
                ('fem_evaluate', {'study': 's', 'expression': 'qw.dnTMloc', 'frequency': '1.40[eV]', 'point': 2})], tag='f11', width=0)
    vals = [json.loads(x[1])['values'][0]['SI'] for x in r[1:]]
    print('conservearea', cons, ': alphaTM(1.42) at 0 and 150 kV/cm %.4g %.4g /m; dnTMloc(1.40) %.4g' % tuple(vals), json.loads(r[0][1]).get('warnings'))
