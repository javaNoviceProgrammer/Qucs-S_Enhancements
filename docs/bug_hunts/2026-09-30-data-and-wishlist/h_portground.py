# A subcircuit whose port is on its own ground: the .SUBCKT line and the run.
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
ws = HERE + '/ws_pg'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
s = Server(ws)
s.call('new_document', {})
s.call('batch', {'calls': [
  {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P1', 'x': 100, 'y': 100}},
  {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P2', 'x': 300, 'y': 100}},
  {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P3', 'x': 100, 'y': 300}},
  {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 200, 'y': 200}},
  {'tool': 'connect', 'arguments': {'from': 'P1.1', 'to': 'R1.1'}}, {'tool': 'connect', 'arguments': {'from': 'P3.1', 'to': 'R1.2'}},
  {'tool': 'connect', 'arguments': {'from': 'P2.1', 'to': 'ground'}}, {'tool': 'connect', 'arguments': {'from': 'P3.1', 'to': 'ground'}},
  {'tool': 'save_document', 'arguments': {'as': 'thru.sch'}}]})
print('check sub:', [i['message'][:100] for k in ('errors', 'warnings', 'notes') for i in s.call('check_schematic', {}, ok=False).get(k, [])])
s.call('new_document', {})
s.call('batch', {'calls': [
  {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1'}}},
  {'tool': 'add_component', 'arguments': {'type': 'Sub', 'name': 'SUB1', 'x': 300, 'y': 200, 'properties': {'File': 'thru.sch'}}},
  {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'RL', 'x': 500, 'y': 250}},
  {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'SUB1.1'}}, {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}},
  {'tool': 'connect', 'arguments': {'from': 'SUB1.2', 'to': 'RL.1'}}, {'tool': 'connect', 'arguments': {'from': 'RL.2', 'to': 'ground'}},
  {'tool': 'connect', 'arguments': {'from': 'SUB1.3', 'to': 'ground'}},
  {'tool': 'add_analysis', 'arguments': {'kind': 'op'}}, {'tool': 'save_document', 'arguments': {'as': 'top.sch'}}]})
net = s.call('get_netlist', {}, ok=False)
net = net if isinstance(net, str) else json.dumps(net)
print('\n'.join(l for l in net.splitlines() if 'SUBCKT' in l.upper() or l[:1] in 'RX'))
r = s.call('simulate', {}, ok=False)
print('simulate:', r.get('succeeded'), [e.get('message')[:100] for e in r.get('errors', [])][:3])
op = s.call('get_dataset', {'operating_point': True}, ok=False)
print('op:', json.dumps(op)[:700])
s.close()
