"""Markers numbered trace by trace: markers 1 (2 ms) and 2 (6 ms) on trace 2; a new marker on trace 1 at 4 ms relative to 2 (the one at 6 ms, as get_schematic numbered it then). Which does it measure from?"""
import mcp
from common import rc, show
s = mcp.Server('p65')
p, sim = rc(s)
d = s.call('add_diagram', {'type': 'rect', 'traces': ['tran.v(out)', 'tran.v(in)']})['diagram']
s.call('add_marker', {'diagram': d, 'at': 0.002, 'trace': 2}); s.call('add_marker', {'diagram': d, 'at': 0.006, 'trace': 2})
def ms():
    g = next(x for x in s.call('get_schematic', {})['diagrams'] if x['diagram'] == d)
    return [(x['marker'], x['variable'].split('/')[-1], round(list(x['at'].values())[0], 6), x.get('relative to'), (x.get('delta') or {}).get('x')) for x in g.get('markers', [])]
show('before: markers', ms(), 400)
m = s.call('add_marker', {'diagram': d, 'at': 0.004, 'trace': 1, 'relative_to': 2})
show('new marker relative_to 2 (meant: the one at 6 ms)', {k: m.get(k) for k in ('marker', 'relative to', 'delta')} if isinstance(m, dict) else m, 300)
show('after: markers', ms(), 600)
print('meant: Δx = 4 ms - 6 ms = -2 ms; got the one at 2 ms if Δx = +2 ms')
s.close()
