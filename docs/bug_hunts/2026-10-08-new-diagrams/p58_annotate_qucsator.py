"""Values at the Marker under Qucsator: a divider's transient (out = 0.5 V by hand), the marker's labels."""
import mcp
from common import show
s = mcp.Server('p58')
s.call('new_project', {'name': 'qq'}); s.call('open_project', {'name': 'qq'}); s.call('new_document', {})
s.call('add_component', {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
s.call('add_component', {'type': 'R', 'name': 'R1', 'x': 200, 'y': 120, 'properties': {'R': '1k'}})
s.call('add_component', {'type': 'R', 'name': 'R2', 'x': 300, 'y': 200, 'properties': {'R': '1k'}})
for a, b in (('V1.1', 'R1.1'), ('R1.2', 'R2.1'), ('V1.2', 'ground'), ('R2.2', 'ground')): s.call('connect', {'from': a, 'to': b})
s.call('set_label', {'at': 'R2.1', 'name': 'out'}); s.call('set_label', {'at': 'V1.1', 'name': 'top'})
s.call('set_simulator', {'simulator': 'qucsator'})
s.call('add_analysis', {'kind': 'tran', 'to': '1 ms'}); s.call('save_document', {'as': 'qq.sch'})
sim = s.call('simulate', {}); print('run', sim.get('succeeded'))
print('vars', [v['name'] for v in s.call('get_dataset', {'points': 0}).get('variables', [])])
d = s.call('add_diagram', {'type': 'rect', 'traces': ['out.Vt']})
show('diagram', [(t['variable'], t.get('points'), t.get('no data')) for t in d.get('traces', [])], 300)
m = s.call('add_marker', {'diagram': d['diagram'], 'at': 0.0005, 'annotate': True})
show('marker', {k: m.get(k) for k in ('text', 'values at the marker')} if isinstance(m, dict) else m, 600)
s.close()
