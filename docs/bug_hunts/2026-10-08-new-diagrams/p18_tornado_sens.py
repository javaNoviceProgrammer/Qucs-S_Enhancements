"""The tornado chart's parts on the shipped .SENS example: what each bar is, against the dataset's own numbers."""
import json, shutil, sys, mcp
from common import EX, show
s = mcp.Server('p18')
s.call('new_project', {'name': 'se'}); d = s.ws + '/se_prj'
shutil.copy(EX + '/ngspice/NGspice features/sensitivityACandDC.sch', d + '/sens.sch')
s.call('open_project', {'name': 'se'}); s.call('open_document', {'path': d + '/sens.sch'})
sim = s.call('simulate', {'simulator': 'ngspice'})
show('sim', {k: sim.get(k) for k in ('succeeded', 'errors')} if isinstance(sim, dict) else sim, 300)
vs = s.call('get_dataset', {'points': 0})
show('vars', [(v['name'], v.get('points'), v.get('final')) for v in vs.get('variables', [])], 2500)
dc = [v for v in vs['variables'] if '.' not in v['name']]
show('dc sens vars', [(v['name'], v.get('points'), v.get('depends on'), v.get('min'), v.get('max')) for v in dc][:12], 1500)
for tr, kw in ((['r1'], {'parts': True}), (['ac.v(r1)'], {'parts': True, 'at': 500}), (['ac.v(r1)'], {'parts': True}), (['r1_scale'], {'parts': True})):
    r = s.call('add_diagram', {'type': 'tornado', 'traces': tr, 'tornado': kw})
    show(f'tornado {tr} {kw} err={int(s.last_error)}', r.get('tornado', r) if isinstance(r, dict) else r, 1500)
s.close()
