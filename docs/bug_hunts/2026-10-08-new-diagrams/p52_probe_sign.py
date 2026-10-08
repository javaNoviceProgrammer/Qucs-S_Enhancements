"""Probe a pin's current: 'the current into it'. R1.1 and R1.2 of the RC: the two must be opposite. By hand i into R1.1 = (v(in) - v(out)) / 100k."""
import numpy as np, mcp
from common import rc, show
s = mcp.Server('p52')
p, sim = rc(s)
d = s.call('add_diagram', {'type': 'rect', 'traces': ['tran.v(in)']})['diagram']
a = s.call('probe', {'what': 'R1.1', 'diagram': d}); b = s.call('probe', {'what': 'R1.2', 'diagram': d})
show('R1.1', {k: a.get(k) for k in ('variable', 'text', 'saved by the next run')}, 300)
show('R1.2', {k: b.get(k) for k in ('variable', 'text', 'saved by the next run', 'already there')}, 300)
s.call('simulate', {'simulator': 'ngspice'})
g = s.call('get_dataset', {'variables': ['tran.v(in)', 'tran.v(out)'] + [v for v in {a.get('variable', '').split(':')[-1].split('/')[-1], b.get('variable', '').split(':')[-1].split('/')[-1]} if v], 'at': [0.0002, 0.0007]})
for v in g.get('variables', []): print(v['name'], v.get('at'))
print('by hand i into R1.1 at 0.2 ms, 0.7 ms = (v(in) - v(out)) / 100k')
s.close()
