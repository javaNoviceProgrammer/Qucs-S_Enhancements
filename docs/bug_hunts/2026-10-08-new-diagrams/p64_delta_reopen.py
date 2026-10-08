"""A delta marker whose reference is on a later trace: marker 1 on trace 2 (v(in) at 2 ms), marker 2 on trace 1 (v(out) at 4 ms) relative to 1. Saved and reopened: which marker is its reference then?"""
import mcp
from common import rc, show
s = mcp.Server('p64')
p, sim = rc(s)
d = s.call('add_diagram', {'type': 'rect', 'traces': ['tran.v(out)', 'tran.v(in)']})['diagram']
s.call('add_marker', {'diagram': d, 'at': 0.002, 'trace': 2})
show('marker 2', s.call('add_marker', {'diagram': d, 'at': 0.004, 'trace': 1, 'relative_to': 1}), 300)
def ms():
    g = next(x for x in s.call('get_schematic', {})['diagrams'] if x['diagram'] == d)
    return [(x['marker'], x['variable'].split(':')[-1].split('/')[-1], round(list(x['at'].values())[0], 6), x.get('relative to'), x.get('delta')) for x in g.get('markers', [])]
show('before', ms(), 600)
s.call('save_document', {}); s.call('close_document', {}); s.call('open_document', {'path': p})
show('after reopen', ms(), 600)
s.close()
