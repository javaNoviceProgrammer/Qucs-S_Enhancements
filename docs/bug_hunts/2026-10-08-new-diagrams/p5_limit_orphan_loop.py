"""A limit's verdict before and after its pane is taken away; a loop of delta markers moved."""
import json, sys, mcp
from common import rc, show
s = mcp.Server('p5')
p, sim = rc(s)
V, I = 'tran.v(out)', 'tran.v(in)'
def diagram(n):
    return next(d for d in s.call('get_schematic', {})['diagrams'] if d['diagram'] == n)
d = s.call('add_diagram', {'type': 'stacked', 'traces': [V], 'panes': 4})['diagram']
s.call('add_trace', {'diagram': d, 'variable': I, 'pane': 4})
s.call('edit_diagram', {'diagram': d, 'limits': [{'upper': 0.9, 'pane': 4, 'label': 'max'}]})
show('pane 4 of 4, its limit', diagram(d).get('verdict'), 400)
s.call('edit_diagram', {'diagram': d, 'panes': 2})
show('panes now 2: the trace in', [t['pane'] for t in diagram(d)['traces']], 100)
show('the verdict', diagram(d).get('verdict'), 400)
show('check_schematic', [w for w in s.call('check_schematic', {}).get('warnings', []) if 'limit' in json.dumps(w).lower() or 'diagram' in json.dumps(w).lower()], 600)
s.call('export_image', {'diagram': d, 'save_as': s.root + '/orphan.png'})
# the loop
r = s.call('add_diagram', {'type': 'rect', 'traces': [V]})['diagram']
for at, rel in ((0.002, None), (0.004, 1), (0.006, 2)):
    a = {'diagram': r, 'at': at}
    if rel: a['relative_to'] = rel
    s.call('add_marker', a)
s.call('edit_marker', {'diagram': r, 'marker': 1, 'relative_to': 3})
for m, at in ((1, 0.003), (2, 0.0045), (3, 0.0071)):
    x = s.call('edit_marker', {'diagram': r, 'marker': m, 'at': at})
    show(f'moved {m} to {at} [{s.last_time:.1f}s]', {k: x.get(k) for k in ('at', 'delta', 'relative to')} if isinstance(x, dict) else x, 300)
show('all', [(m['marker'], m['at'], m.get('relative to'), m.get('delta')) for m in diagram(r)['markers']], 900)
s.call('export_image', {'diagram': r, 'save_as': s.root + '/loop.png'})
print('alive', s.p.poll() is None, s.root)
s.close()
