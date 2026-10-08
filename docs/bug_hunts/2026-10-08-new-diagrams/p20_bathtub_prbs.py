"""The bathtub curve on the shipped PRBS eye example: against the eye diagram's own measurements, and odd thresholds and levels."""
import json, shutil, sys, mcp
from common import EX, show
s = mcp.Server('p20')
s.call('new_project', {'name': 'eye'}); d = s.ws + '/eye_prj'
shutil.copy(EX + '/ngspice/NGspice features/PRBS_eye_diagram.sch', d + '/eye.sch')
s.call('open_project', {'name': 'eye'}); s.call('open_document', {'path': d + '/eye.sch'})
sim = s.call('simulate', {'simulator': 'ngspice'})
show('sim', sim.get('succeeded') if isinstance(sim, dict) else sim, 200)
g = s.call('get_schematic', {})
for dd in g['diagrams']:
    show(f"diagram {dd['diagram']} {dd['type']}", {k: dd.get(k) for k in ('traces', 'eye', 'analyses')}, 1500)
V = next(t['variable'].split(':')[-1].split('/')[-1] for dd in g['diagrams'] if dd['type'] == 'eye' for t in dd['traces'])
print('V =', V)
def c(label, args, n=900):
    r = s.call('add_diagram', dict(type='bathtub', traces=[V], **args))
    show(f'[{s.last_time:4.1f}s err={int(s.last_error)}] {label}:', (r.get('analyses'), r.get('bathtub')) if isinstance(r, dict) else r, n)
    if s.p.poll() is not None: print('SERVER DIED'); sys.exit(1)
c('default', {})
c('levels 4 on NRZ', {'bathtub': {'levels': 4}})
c('threshold 100 V', {'bathtub': {'threshold': 100}})
c('ber 0.01', {'bathtub': {'ber': 0.01}})
c('ber 1e-300', {'bathtub': {'ber': 1e-300}})
c('from past the end', {'bathtub': {'from': 1}})
show('get_dataset eye+bathtub', s.call('get_dataset', {'variables': [V], 'measure': ['eye', 'bathtub']}), 2500)
s.close()
