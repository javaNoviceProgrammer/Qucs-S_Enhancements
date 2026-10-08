"""Values at the Marker on a two-level sweep (the divider of p16): a marker on a curve other than the first - whose values are labelled? By hand v(out) = R2/(R1+R2)."""
import json, sys, mcp
from common import show
s = mcp.Server('p45')
s.call('new_project', {'name': 'ct'}); s.call('open_project', {'name': 'ct'}); s.call('new_document', {})
s.call('add_component', {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
s.call('add_component', {'type': 'R', 'name': 'R1', 'x': 200, 'y': 120, 'properties': {'R': '1k'}})
s.call('add_component', {'type': 'R', 'name': 'R2', 'x': 300, 'y': 200, 'properties': {'R': '1k'}})
for a, b in (('V1.1', 'R1.1'), ('R1.2', 'R2.1'), ('V1.2', 'ground'), ('R2.2', 'ground')): s.call('connect', {'from': a, 'to': b})
s.call('set_label', {'at': 'R2.1', 'name': 'out'}); s.call('set_label', {'at': 'V1.1', 'name': 'top'})
s.call('add_analysis', {'kind': 'op', 'name': 'DC1'})
s.call('add_analysis', {'kind': 'sweep', 'name': 'SW1', 'analysis': 'DC1', 'parameter': 'R1', 'from': '100', 'to': '10k', 'points': 21})
s.call('add_analysis', {'kind': 'sweep', 'name': 'SW2', 'analysis': 'SW1', 'parameter': 'R2', 'from': '100', 'to': '10k', 'points': 11})
s.call('save_document', {'as': 'div.sch'}); s.call('simulate', {'simulator': 'ngspice'})
V = 'sw1.v(out)'
d = s.call('add_diagram', {'type': 'rect', 'traces': [V]})
show('diagram', {k: d.get(k) for k in ('traces',)}, 300)
for branch_at in (5050,):
    m = s.call('add_marker', {'diagram': d['diagram'], 'at': branch_at, 'annotate': True})
    show('marker (first curve?)', {k: m.get(k) for k in ('at', 'text', 'values at the marker', 'value')} if isinstance(m, dict) else m, 900)
r1 = 100 + 495 * 10
for j, r2 in enumerate([100 + 990 * k for k in range(11)]):
    if j in (0, 5, 10): print(f'by hand R1 {r1} R2 {r2}: out {r2 / (r1 + r2):.6f}')
s.close()
