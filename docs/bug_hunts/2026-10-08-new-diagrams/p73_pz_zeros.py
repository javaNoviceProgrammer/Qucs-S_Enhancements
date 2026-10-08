"""A .PZ run with zeros: an RC high-pass (C 1u then R 1k to ground): a zero at 0, a pole at -1000 rad/s by hand. Are the zeros drawn and listed as zeros?"""
import mcp
from common import show
s = mcp.Server('p73')
s.call('new_project', {'name': 'hp'}); s.call('open_project', {'name': 'hp'}); s.call('new_document', {})
s.call('add_component', {'type': 'Vac', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
s.call('add_component', {'type': 'C', 'name': 'C1', 'x': 200, 'y': 120, 'properties': {'C': '1u'}})
s.call('add_component', {'type': 'R', 'name': 'R1', 'x': 300, 'y': 200, 'properties': {'R': '1k'}})
for a, b in (('V1.1', 'C1.1'), ('C1.2', 'R1.1'), ('V1.2', 'ground'), ('R1.2', 'ground')): s.call('connect', {'from': a, 'to': b})
s.call('set_label', {'at': 'V1.1', 'name': 'in'}); s.call('set_label', {'at': 'R1.1', 'name': 'out'})
s.call('add_component', {'type': '.PZ', 'name': 'PZ1', 'x': 100, 'y': 400, 'properties': {'Input': 'in 0', 'Output': 'out 0'}})
s.call('save_document', {'as': 'hp.sch'}); sim = s.call('simulate', {'simulator': 'ngspice'})
print('run', sim.get('succeeded'), sim.get('errors'))
vs = [v['name'] for v in s.call('get_dataset', {'points': 0}).get('variables', [])]; print('vars', vs)
r = s.call('add_diagram', {'type': 'pole_zero', 'traces': vs})
show('roots', r.get('roots') if isinstance(r, dict) else r, 900)
s.close()
