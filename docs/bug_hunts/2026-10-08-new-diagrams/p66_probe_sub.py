"""Probe a subcircuit instance's pins and the instance itself (the shipped quartz example): what each adds, and whether the run still succeeds."""
import os, shutil, sys, mcp
from common import EX, show
src = EX + '/ngspice/Devices/XTAL'
for what in ('pin1', 'pin2', 'part'):
    s = mcp.Server('p66')
    s.call('new_project', {'name': 'xt'}); d = s.ws + '/xt_prj'
    for f in os.listdir(src):
        if f.endswith('.sch'): shutil.copy(os.path.join(src, f), d)
    s.call('open_project', {'name': 'xt'}); s.call('open_document', {'path': d + '/quarz_test.sch'})
    g = s.call('get_schematic', {})
    sub = next(c for c in g['components'] if c.get('type', c.get('model', '')) in ('Sub',) or c['name'].upper().startswith(('SUB', 'X')))
    target = {'pin1': f"{sub['name']}.1", 'pin2': f"{sub['name']}.2", 'part': sub['name']}[what]
    dg = g['diagrams'][0]['diagram'] if g.get('diagrams') else s.call('add_diagram', {'type': 'rect'})['diagram']
    r = s.call('probe', {'what': target, 'diagram': dg})
    sim = s.call('simulate', {'simulator': 'ngspice'})
    print(f"{target}: {r.get('variable') if isinstance(r, dict) else str(r)[:150]} -> run {sim.get('succeeded') if isinstance(sim, dict) else sim} {sim.get('errors') if isinstance(sim, dict) else ''}"[:600], flush=True)
    s.close()
