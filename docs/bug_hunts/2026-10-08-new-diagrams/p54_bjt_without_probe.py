"""The 2N3904 follower simulated as shipped, then with one probe (its base), then with the probe removed: which run fails? And the netlist's control lines."""
import shutil, mcp
from common import EX, show
s = mcp.Server('p54')
s.call('new_project', {'name': 'q'}); d = s.ws + '/q_prj'
shutil.copy(EX + '/ngspice/General Electronics/2N3904_follower.sch', d + '/f.sch')
s.call('open_project', {'name': 'q'}); s.call('open_document', {'path': d + '/f.sch'})
sim = s.call('simulate', {'simulator': 'ngspice'}); print('as shipped:', sim.get('succeeded'), sim.get('errors'))
g = s.call('get_schematic', {}); q = next(c for c in g['components'] if c['name'].startswith('Q2N3904'))
dg = g['diagrams'][0]['diagram']
r = s.call('probe', {'what': f"{q['name']}.1", 'diagram': dg}); print('probe base:', r.get('variable'))
net = s.call('get_netlist', {})
print('\n'.join(l for l in net.splitlines() if l.startswith(('.save', '.options', 'write', 'ac ', 'tran ', 'op', 'dc '))))
sim = s.call('simulate', {'simulator': 'ngspice'}); print('with the probe:', sim.get('succeeded'), sim.get('errors'), sim.get('last lines', '')[-400:] if isinstance(sim.get('last lines'), str) else (sim.get('last lines') or [])[-6:])
s.close()
