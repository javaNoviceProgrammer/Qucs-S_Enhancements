"""A contour map on a real two-parameter sweep (a divider, R1 and R2 swept about an op): odd pass and range; numbers by hand."""
import json, sys, mcp
from common import show
s = mcp.Server('p16')
def c(label, tool, args, n=600):
    r = s.call(tool, args)
    show(f'[{s.last_time:5.1f}s err={int(s.last_error)}] {label}:', r, n)
    if s.p.poll() is not None: print('SERVER DIED', open(s.root + '/server.err').read()[-3000:]); sys.exit(1)
    return r
s.call('new_project', {'name': 'ct'}); s.call('open_project', {'name': 'ct'}); s.call('new_document', {})
s.call('add_component', {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
s.call('add_component', {'type': 'R', 'name': 'R1', 'x': 200, 'y': 120, 'properties': {'R': '1k'}})
s.call('add_component', {'type': 'R', 'name': 'R2', 'x': 300, 'y': 200, 'properties': {'R': '1k'}})
for a, b in (('V1.1', 'R1.1'), ('R1.2', 'R2.1'), ('V1.2', 'ground'), ('R2.2', 'ground')): s.call('connect', {'from': a, 'to': b})
s.call('set_label', {'at': 'R2.1', 'name': 'out'})
c('op', 'add_analysis', {'kind': 'op', 'name': 'DC1'}, 120)
c('sweep R1', 'add_analysis', {'kind': 'sweep', 'name': 'SW1', 'analysis': 'DC1', 'parameter': 'R1', 'from': '100', 'to': '10k', 'points': 21}, 120)
c('sweep R2', 'add_analysis', {'kind': 'sweep', 'name': 'SW2', 'analysis': 'SW1', 'parameter': 'R2', 'from': '100', 'to': '10k', 'points': 11}, 120)
s.call('save_document', {'as': 'div.sch'})
c('simulate', 'simulate', {'simulator': 'ngspice'}, 300)
print('vars', json.dumps(s.call('get_dataset', {'points': 0}))[:900])
vs = s.call('get_dataset', {'points': 0}).get('variables', [])
show('variables', [(v['name'], v.get('depends on')) for v in vs], 600)
V = next(v['name'] for v in vs if 'out' in v['name'])
r = c('contour', 'add_diagram', {'type': 'contour', 'traces': [V]}, 1500)
# by hand: v(out) = R2 / (R1 + R2): at R1 = R2 the 0.5 iso-line is the diagonal
for pas in ({'min': 0.6, 'max': 0.4}, {'min': 0.4}, {'max': 2}, {'min': 'abc'}, {}, {'min': -1e308, 'max': 1e308}):
    c(f'pass {pas}', 'edit_diagram', {'diagram': r['diagram'], 'contour': {'pass': pas}}, 500)
for rg in ({'from': 1, 'to': 0}, {'from': 0.5, 'to': 0.5}, {'from': 'x'}, {'from': 0, 'to': 1e-300}):
    c(f'range {rg}', 'edit_diagram', {'diagram': r['diagram'], 'contour': {'range': rg}}, 400)
c('levels 1000', 'edit_diagram', {'diagram': r['diagram'], 'contour': {'levels': 1000, 'range': None}}, 300)
c('export', 'export_image', {'diagram': r['diagram'], 'save_as': s.root + '/contour.png'}, 150)
print('alive', s.p.poll() is None, s.root)
s.close()
