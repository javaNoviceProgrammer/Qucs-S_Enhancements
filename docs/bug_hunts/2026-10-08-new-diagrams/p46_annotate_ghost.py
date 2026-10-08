"""Values at the Marker with the marker on a ghost (a kept run): whose values label the schematic - the run the marker reads, or the current one?"""
import mcp
from common import rc, show
s = mcp.Server('p46')
p, sim = rc(s)
s.call('simulate', {'simulator': 'ngspice', 'keep_as': 'before'})
s.call('edit_component', {'name': 'R1', 'properties': {'R': '10k'}})
s.call('simulate', {'simulator': 'ngspice'})
d = s.call('add_diagram', {'type': 'rect', 'traces': ['tran.v(out)']})['diagram']
s.call('edit_diagram', {'diagram': d, 'overlay': 'before'})
g = next(x for x in s.call('get_schematic', {})['diagrams'] if x['diagram'] == d)
show('traces', [(t['trace'], t['variable'], t.get('ghost')) for t in g['traces']], 300)
for tr in (1, 2):
    m = s.call('add_marker', {'diagram': d, 'at': 0.0015, 'trace': tr, 'annotate': True})
    show(f'marker on trace {tr}', {k: m.get(k) for k in ('text', 'values at the marker')} if isinstance(m, dict) else m, 600)
s.close()
