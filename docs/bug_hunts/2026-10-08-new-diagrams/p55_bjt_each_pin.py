"""The 2N3904 follower with one probed pin at a time (2: collector, 3: emitter): which makes the run fail, and the write lines."""
import shutil, sys, mcp
from common import EX, show
for pin in (2, 3):
    s = mcp.Server('p55')
    s.call('new_project', {'name': 'q'}); d = s.ws + '/q_prj'
    shutil.copy(EX + '/ngspice/General Electronics/2N3904_follower.sch', d + '/f.sch')
    s.call('open_project', {'name': 'q'}); s.call('open_document', {'path': d + '/f.sch'})
    g = s.call('get_schematic', {}); q = next(c for c in g['components'] if c['name'].startswith('Q2N3904'))
    r = s.call('probe', {'what': f"{q['name']}.{pin}", 'diagram': g['diagrams'][0]['diagram']})
    sim = s.call('simulate', {'simulator': 'ngspice'})
    net = s.call('get_netlist', {})
    print(f'pin {pin}: {r.get("variable")}: succeeded {sim.get("succeeded")} {sim.get("errors")}')
    print('   ', [l for l in net.splitlines() if l.startswith(('.save', 'write'))][:3])
    s.close()
