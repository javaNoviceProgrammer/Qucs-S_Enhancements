"""A trace deleted under a delta marker's reference; panes taken away then undone; on the ASan build."""
import sys, mcp
from common import rc, show
s = mcp.Server('p29')
p, sim = rc(s)
V, I = 'tran.v(out)', 'tran.v(in)'
def diagram(n):
    return next(d for d in s.call('get_schematic', {})['diagrams'] if d['diagram'] == n)
r = s.call('add_diagram', {'type': 'rect', 'traces': [V, I]})['diagram']
s.call('add_marker', {'diagram': r, 'at': 0.002, 'trace': 1})
s.call('add_marker', {'diagram': r, 'at': 0.004, 'trace': 2, 'relative_to': 1})
x = s.call('delete', {'traces': [{'diagram': r, 'trace': 1}]})
show('delete trace 1 (marker 1, the reference, on it)', x, 300)
show('markers now', [(m['marker'], m.get('relative to'), m.get('delta'), m['text'].replace('\n', ' | ')) for m in diagram(r).get('markers', [])], 600)
s.call('export_image', {'diagram': r, 'save_as': s.root + '/after_delete.png'})
show('undo', s.call('undo', {}), 200)
show('markers after undo', [(m['marker'], m.get('relative to'), m.get('delta')) for m in diagram(r).get('markers', [])], 600)
d = s.call('add_diagram', {'type': 'stacked', 'traces': [V], 'panes': 4})['diagram']
s.call('add_trace', {'diagram': d, 'variable': I, 'pane': 4})
s.call('edit_diagram', {'diagram': d, 'panes': 2})
show('panes 2: trace panes', [t['pane'] for t in diagram(d)['traces']], 100)
show('undo', s.call('undo', {}), 200)
show('after undo: panes, trace panes', (len(diagram(d)['panes']), [t['pane'] for t in diagram(d)['traces']]), 200)
show('redo', s.call('redo', {}), 200)
show('after redo', (len(diagram(d)['panes']), [t['pane'] for t in diagram(d)['traces']]), 200)
err = open(s.root + '/server.err', errors='replace').read()
print('sanitizer lines:', [l for l in err.splitlines() if 'runtime error' in l or 'Sanitizer' in l][:6], 'alive', s.p.poll() is None)
s.close()
