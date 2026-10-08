"""A box plot of a swept trace (the divider: 11 curves over R1): each curve's value at R1 = 5050 and at 3000 (between points), against numpy by hand."""
import numpy as np, mcp
from common import show
s = mcp.Server('p50')
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
for at in (5050, 3000):
    r = s.call('add_diagram', {'type': 'box_plot', 'traces': ['sw1.v(out)'], 'box_plot': {'at': at, 'whiskers': 'range'}})
    b = r['box_plot']['boxes'][0]
    # by hand: each curve's value at R1 = at, straight between the grid's points
    vals = np.array([np.interp(at, R1grid, r2 / (R1grid + r2)) for r2 in R2])
    print(f'at {at}: tool', {k: b.get(k) for k in ('q1', 'median', 'q3', 'min', 'max', 'mean', 'values')})
    print(f'at {at}: hand', dict(q1=round(np.percentile(vals, 25), 6), median=round(np.median(vals), 6), q3=round(np.percentile(vals, 75), 6), min=round(vals.min(), 6), max=round(vals.max(), 6), mean=round(vals.mean(), 6), n=len(vals)))
s.close()
