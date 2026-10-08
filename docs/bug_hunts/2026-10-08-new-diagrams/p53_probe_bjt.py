"""Probe a transistor's pins (the shipped 2N3904 follower): which ngspice vector each pin gives, and its value against KCL (ib + ic + ie = 0, all into the device)."""
import shutil, mcp
from common import EX, show
s = mcp.Server('p53')
s.call('new_project', {'name': 'q'}); d = s.ws + '/q_prj'
shutil.copy(EX + '/ngspice/General Electronics/2N3904_follower.sch', d + '/f.sch')
s.call('open_project', {'name': 'q'}); s.call('open_document', {'path': d + '/f.sch'})
g = s.call('get_schematic', {})
q = next(c for c in g['components'] if c['name'].startswith('Q2N3904'))
print('pins', [(p['pin'], p.get('name'), p.get('net')) for p in q['pins']])
dg = g['diagrams'][0]['diagram'] if g.get('diagrams') else s.call('add_diagram', {'type': 'rect'})['diagram']
vs = []
for pin in (1, 2, 3):
    r = s.call('probe', {'what': f"{q['name']}.{pin}", 'diagram': dg})
    show(f'probe pin {pin}', {k: r.get(k) for k in ('variable', 'saved by the next run', 'already there', 'text')} if isinstance(r, dict) else r, 250)
    if isinstance(r, dict) and r.get('variable'): vs.append(r['variable'].split('/')[-1].split(':')[-1])
sim = s.call('simulate', {'simulator': 'ngspice'})
x = s.call('get_dataset', {'points': 0}); print('sim', {k: sim.get(k) for k in ('succeeded', 'errors', 'error', 'log', 'messages', 'ngspice said') if isinstance(sim, dict) and k in sim}, sorted(sim.keys()) if isinstance(sim, dict) else sim); print('dataset answer', str(x)[:300]); names = [v['name'] for v in (x.get('variables', []) if isinstance(x, dict) else [])]
print('dataset', [n for n in names if '@' in n])
g = s.call('get_dataset', {'variables': vs, 'points': 3})
for v in g.get('variables', []) if isinstance(g, dict) else []: print(v['name'], 'final', v.get('final'), 'mean', v.get('mean'))
s.close()
