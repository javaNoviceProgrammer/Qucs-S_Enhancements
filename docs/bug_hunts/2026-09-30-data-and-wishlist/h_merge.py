# A labelled net wired to a grounded one (one wire, as in the GUI): what node the merged net gets.
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_merge'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
s = Server(ws)
s.call('new_document', {})
s.call('batch', {'calls': [
  {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200}},
  {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 300, 'y': 200}},
  {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R2', 'x': 500, 'y': 200}},
  {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'R1.1'}}, {'tool': 'connect', 'arguments': {'from': 'R1.1', 'to': 'R2.1'}},
  {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}},
  {'tool': 'connect', 'arguments': {'from': 'R1.2', 'to': 'R2.2'}},
  {'tool': 'add_analysis', 'arguments': {'kind': 'op'}}]})
sch = s.call('get_schematic', {})
pins = {c['name']: {str(p['pin']): (p['x'], p['y']) for p in c['pins']} for c in sch['components'] if 'pins' in c}
print('label:', s.call('set_label', {'at': list(pins['R2']['2']), 'name': 'ret'}, ok=False))
print('connect R1.2 to V1.2:', str(s.call('connect', {'from': 'R1.2', 'to': 'V1.2'}, ok=False))[:200])
net = s.call('get_netlist', {}, ok=False)
net = net if isinstance(net, str) else json.dumps(net)
print([l for l in net.splitlines() if l[:2] in ('V1', 'R1', 'R2')])
chk = s.call('check_schematic', {}, ok=False)
print('check:', [i['message'][:160] for k in ('errors', 'warnings', 'notes') for i in chk.get(k, [])])
r = s.call('simulate', {}, ok=False)
print('simulate:', r.get('succeeded'), [w.get('message')[:80] for w in r.get('warnings', [])][:3])
s.close()
