import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run, H
from f2 import well, spec
m = well([], [dict(spec, energies="linspace(1.38[eV], 1.95[eV], 20)")])
r = run(m, [('fem_solve', {'study': 's'}), ('fem_plot', {'expression': 'qw.alphaTE'}), ('fem_plot', {'expression': 'qw.alphaTM', 'unit': '1/cm'}), ('fem_model', {'action': 'save'})], tag='f47', width=300)
saved = json.load(open(H + '/f47.qfem'))
for p in saved['results']['plots']:
    if p.get('type') == 'plotgroup1d': print(p.get('label'), [(g.get('type'), g.get('expression'), g.get('unit')) for g in p.get('plots', [])])
