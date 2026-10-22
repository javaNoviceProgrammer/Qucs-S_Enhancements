import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run, H
from f2 import well, spec
m = well([{"name": "F", "expression": "0[V/m]"}], [{"type": "sweep", "parameters": [["F", "0, 5e6, 1e7", "V/m"]]}, dict(spec, energies="linspace(1.38[eV], 1.95[eV], 58)", dnwindow=["1.38[eV]", "1.5[eV]"], conservearea=True)], {"F": "F"})
run(m, [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'qw.dnTMloc', 'frequency': '1.40[eV]', 'all': True}),
        ('fem_evaluate', {'study': 's', 'expression': 'qw.dnTMloc', 'frequency': '1.40[eV]', 'point': 3}),
        ('fem_evaluate', {'study': 's', 'expression': 'qw.dnTMloc', 'frequency': '1.405[eV]', 'point': 3}),
        ('fem_evaluate', {'study': 's', 'expression': 'qw.dnTMloc', 'frequency': '885.6[nm]', 'point': 3})], tag='f10', width=700)
