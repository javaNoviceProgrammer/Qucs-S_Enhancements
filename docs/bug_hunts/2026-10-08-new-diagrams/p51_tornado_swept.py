"""A box plot of a swept trace (the divider: 11 curves over R1): each curve's value at R1 = 5050 and at 3000 (between points), against numpy by hand."""
import numpy as np, mcp
from common import show
s = mcp.Server('p51')
s.call('new_project', {'name': 'ct'}); s.call('open_project', {'name': 'ct'}); s.call('new_document', {})
s.call('add_component', {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
s.call('add_component', {'type': 'R', 'name': 'R1', 'x': 200, 'y': 120, 'properties': {'R': '1k'}})
s.call('add_component', {'type': 'R', 'name': 'R2', 'x': 300, 'y': 200, 'properties': {'R': '1k'}})
for a, b in (('V1.1', 'R1.1'), ('R1.2', 'R2.1'), ('V1.2', 'ground'), ('R2.2', 'ground')): s.call('connect', {'from': a, 'to': b})
s.call('set_label', {'at': 'R2.1', 'name': 'out'})
s.call('add_analysis', {'kind': 'op', 'name': 'DC1'})
s.call('add_analysis', {'kind': 'sweep', 'name': 'SW1', 'analysis': 'DC1', 'parameter': 'R1', 'from': '100', 'to': '10k', 'points': 21})
s.call('add_analysis', {'kind': 'sweep', 'name': 'SW2', 'analysis': 'SW1', 'parameter': 'R2', 'from': '100', 'to': '10k', 'points': 11})
s.call('save_document', {'as': 'div.sch'}); s.call('simulate', {'simulator': 'ngspice'})
R2 = np.array([100 + 990 * k for k in range(11)])
R1grid = np.array([100 + 495 * i for i in range(21)])
for kw in ({'at': 3000}, {'mode': 'spread', 'at': 3000}):
    r = s.call('add_diagram', {'type': 'tornado', 'traces': ['sw1.v(out)'], 'tornado': kw})
    show(f'tornado {kw}', r.get('tornado', r), 600)
print('by hand: v(out) at R1 = 3000 (interpolated) of the first curve (R2 100):', round(float(np.interp(3000, R1grid, 100 / (R1grid + 100))), 6),
      '; at the nearest grid point 2575:', round(100 / (2575 + 100), 6), '; at 3070:', round(100 / (3070 + 100), 6))
print('by hand: the first curve over R1: lowest', round(100 / (10000 + 100), 6), 'highest', round(100 / 200, 6), '; all curves: lowest', round(100 / 10100, 6), 'highest', round(10000 / 10100, 6))
s.close()
