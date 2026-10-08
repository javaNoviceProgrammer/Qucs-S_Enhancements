"""A delta marker at the same x as its reference: 1/Δx = 1/0 - in the text, in the tool's JSON answer, saved and reopened (ASan build)."""
import json, mcp
from common import rc, show
s = mcp.Server('p63')
p, sim = rc(s)
d = s.call('add_diagram', {'type': 'rect', 'traces': ['tran.v(out)', 'tran.v(in)']})['diagram']
s.call('add_marker', {'diagram': d, 'at': 0.002, 'trace': 1})
m = s.call('add_marker', {'diagram': d, 'at': 0.002, 'trace': 2, 'relative_to': 1})
show('same x, other trace', {k: m.get(k) for k in ('delta', 'text')} if isinstance(m, dict) else m, 400)
m = s.call('add_marker', {'diagram': d, 'at': 0.002, 'trace': 1, 'relative_to': 1})
show('same x, same trace', {k: m.get(k) for k in ('delta', 'text')} if isinstance(m, dict) else m, 400)
s.call('save_document', {}); s.call('close_document', {}); s.call('open_document', {'path': p})
g = next(x for x in s.call('get_schematic', {})['diagrams'] if x['diagram'] == d)
show('after reopen', [(x['marker'], x.get('delta')) for x in g.get('markers', [])], 400)
err = open(s.root + '/server.err', errors='replace').read()
print('sanitizer:', [l for l in err.splitlines() if 'runtime error' in l][:3], 'alive', s.p.poll() is None)
s.close()
