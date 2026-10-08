"""Probe under Qucsator (the simulator in use): a net, a pin, a part - what each adds, and whether the next Qucsator run gives it data."""
import mcp
from common import show
s = mcp.Server('p57')
s.call('new_project', {'name': 'qq'}); s.call('open_project', {'name': 'qq'}); s.call('new_document', {})
s.call('add_component', {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
s.call('add_component', {'type': 'R', 'name': 'R1', 'x': 200, 'y': 120, 'properties': {'R': '1k'}})
s.call('add_component', {'type': 'R', 'name': 'R2', 'x': 300, 'y': 200, 'properties': {'R': '1k'}})
for a, b in (('V1.1', 'R1.1'), ('R1.2', 'R2.1'), ('V1.2', 'ground'), ('R2.2', 'ground')): s.call('connect', {'from': a, 'to': b})
s.call('set_label', {'at': 'R2.1', 'name': 'out'})
show('set_simulator', s.call('set_simulator', {'simulator': 'qucsator'}), 150)
s.call('add_analysis', {'kind': 'tran', 'to': '1 ms'}); s.call('save_document', {'as': 'qq.sch'})
sim = s.call('simulate', {}); print('first run', sim.get('succeeded'), sim.get('simulator'), sim.get('errors'))
d = s.call('add_diagram', {'type': 'rect'})['diagram']
for w in ('out', 'R1.1', 'R1'):
    r = s.call('probe', {'what': w, 'diagram': d})
    show(f'probe {w}', {k: r.get(k) for k in ('variable', 'saved by the next run', 'has data', 'text')} if isinstance(r, dict) else r, 300)
sim = s.call('simulate', {}); print('after the probes', sim.get('succeeded'), sim.get('errors'))
g = next(x for x in s.call('get_schematic', {})['diagrams'] if x['diagram'] == d)
show('traces', [(t['variable'], t.get('points'), (t.get('no data') or '')[:90]) for t in g['traces']], 900)
c = s.call('check_schematic', {}); print('check_schematic:', c.get('found'), c.get('errors'), c.get('warnings'))
n = s.call('get_netlist', {}); print('netlist:', str(n)[:300].replace(chr(10), ' | '))
s.close()
