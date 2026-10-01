# A subcircuit's port on a net that carries a label: does the .SUBCKT's node and the parts' node agree?
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_pl'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
s = Server(ws)
s.call('new_document', {})
s.call('batch', {'calls': [
  {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P1', 'x': 100, 'y': 100}},
  {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P2', 'x': 100, 'y': 300}},
  {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 250, 'y': 200}},
  {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R2', 'x': 450, 'y': 200}},
  {'tool': 'connect', 'arguments': {'from': 'P1.1', 'to': 'R1.1'}}, {'tool': 'connect', 'arguments': {'from': 'R1.1', 'to': 'R2.1'}},
  {'tool': 'connect', 'arguments': {'from': 'P2.1', 'to': 'R1.2'}}, {'tool': 'connect', 'arguments': {'from': 'R1.2', 'to': 'R2.2'}}]})
sch = s.call('get_schematic', {})
pins = {c['name']: {str(p['pin']): (p['x'], p['y']) for p in c['pins']} for c in sch['components'] if 'pins' in c}
print('label:', s.call('set_label', {'at': list(pins['R2']['1']), 'name': 'foo'}, ok=False))
s.call('save_document', {'as': 'sub.sch'})
s.call('new_document', {})
s.call('batch', {'calls': [
  {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1'}}},
  {'tool': 'add_component', 'arguments': {'type': 'Sub', 'name': 'SUB1', 'x': 300, 'y': 200, 'properties': {'File': 'sub.sch'}}},
  {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'SUB1.1'}}, {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}},
  {'tool': 'connect', 'arguments': {'from': 'SUB1.2', 'to': 'ground'}}, {'tool': 'add_analysis', 'arguments': {'kind': 'op'}},
  {'tool': 'save_document', 'arguments': {'as': 'top.sch'}}]})
net = s.call('get_netlist', {}, ok=False)
net = net if isinstance(net, str) else json.dumps(net)
print('\n'.join(l for l in net.splitlines() if l[:1] in 'RXV.' and not l.startswith('.control') or 'SUBCKT' in l.upper() or 'ENDS' in l.upper())[:800])
r = s.call('simulate', {}, ok=False)
op = s.call('get_dataset', {'operating_point': True}, ok=False)
print('I(V1):', json.dumps(op)[:400])
s.close()
