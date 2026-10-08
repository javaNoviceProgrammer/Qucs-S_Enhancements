"""Values at the Marker through a save and reopen, and after the next simulation with a changed part: shown still, and up to date?"""
import mcp
from common import rc, show
s = mcp.Server('p76')
p, sim = rc(s)
d = s.call('add_diagram', {'type': 'rect', 'traces': ['tran.v(out)']})['diagram']
m = s.call('add_marker', {'diagram': d, 'at': 0.0015, 'annotate': True})
show('placed', m.get('values at the marker'), 300)
s.call('save_document', {}); s.call('close_document', {}); s.call('open_document', {'path': p})
g = s.call('get_schematic', {}); show('after reopen', g.get('values at the marker'), 300)
s.call('edit_component', {'name': 'R1', 'properties': {'R': '10k'}}); s.call('simulate', {'simulator': 'ngspice'})
g = s.call('get_schematic', {}); show('after R1 10k and a run (out was 0.538, now about 0.993)', g.get('values at the marker'), 300)
s.close()
