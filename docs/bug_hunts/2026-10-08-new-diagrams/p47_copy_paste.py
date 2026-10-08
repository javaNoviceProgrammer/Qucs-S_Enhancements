"""A stacked diagram with a delta pair, limits on pane 3 and a ghost, copied and pasted (Ctrl+C, Ctrl+V, a click): does the copy keep them, its deltas its own? ASan build."""
import sys, mcp
from common import rc, show
s = mcp.Server('p47')
p, sim = rc(s)
s.call('simulate', {'simulator': 'ngspice', 'keep_as': 'before'})
d = s.call('add_diagram', {'type': 'stacked', 'traces': ['tran.v(out)', {'variable': 'tran.v(in)', 'pane': 3}], 'panes': 3, 'x': 1000, 'y': 400,
                           'limits': [{'upper': 0.9, 'pane': 3, 'label': 'max'}, {'lower': [[0.004, 0.2], [0.01, 0.2]], 'pane': 1}]})['diagram']
s.call('edit_diagram', {'diagram': d, 'overlay': 'before'})
s.call('add_marker', {'diagram': d, 'at': 0.002, 'trace': 1}); s.call('add_marker', {'diagram': d, 'at': 0.004, 'trace': 1, 'relative_to': 1})
def summary(n):
    x = next(x for x in s.call('get_schematic', {})['diagrams'] if x['diagram'] == n)
    return {'panes': len(x.get('panes', [])), 'traces': [(t['variable'], t.get('pane'), t.get('ghost')) for t in x['traces']],
            'markers': [(m['marker'], m.get('relative to'), round(m['at']['time'], 6)) for m in x.get('markers', [])], 'limits': x.get('limits'), 'verdict': x.get('verdict', {}).get('pass')}
before = summary(d)
show('original', before, 900)
s.call('select', {'diagrams': [d]})
s.call('send_input', {'keys': 'Ctrl+C'})
s.call('send_input', {'keys': 'Ctrl+V'})
s.call('send_input', {'click': [1500, 1400]})
s.call('send_input', {'keys': 'Escape'})
g = s.call('get_schematic', {})['diagrams']
show('diagrams now', [(x['diagram'], x['type'], x['x'], x['y']) for x in g], 400)
if len(g) > d:
    after = summary(g[-1]['diagram'])
    show('the copy', after, 900)
    print('same as the original:', {k: before[k] == after[k] for k in before})
err = open(s.root + '/server.err', errors='replace').read()
print('sanitizer:', [l for l in err.splitlines() if 'runtime error' in l or 'Sanitizer' in l][:4], 'alive', s.p.poll() is None)
s.close()
