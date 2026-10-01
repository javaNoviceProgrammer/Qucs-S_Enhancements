# A port's net named after it (P1) beside a net labelled p1: one node to ngspice, which reads names without case?
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
ws = HERE + '/ws_pcase'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
s = Server(ws)
s.call('new_document', {})
s.call('batch', {'calls': [
  {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P1', 'x': 100, 'y': 100}},
  {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P2', 'x': 100, 'y': 300}},
  {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 250, 'y': 200}},
  {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R2', 'x': 450, 'y': 200}},
  {'tool': 'connect', 'arguments': {'from': 'P1.1', 'to': 'R1.1'}}, {'tool': 'connect', 'arguments': {'from': 'R1.2', 'to': 'R2.1'}},
  {'tool': 'connect', 'arguments': {'from': 'P2.1', 'to': 'R2.2'}}]})
sch = s.call('get_schematic', {})
pins = {c['name']: {str(p['pin']): (p['x'], p['y']) for p in c['pins']} for c in sch['components'] if 'pins' in c}
print('label p1 on the middle net:', s.call('set_label', {'at': list(pins['R1']['2']), 'name': 'p1'}, ok=False))
s.call('save_document', {'as': 'sub.sch'})
print('check:', [i['message'][:120] for k in ('errors', 'warnings', 'notes') for i in s.call('check_schematic', {}, ok=False).get(k, [])])
net = s.call('get_netlist', {}, ok=False)
s.call('new_document', {})
s.call('batch', {'calls': [
  {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '2'}}},
  {'tool': 'add_component', 'arguments': {'type': 'Sub', 'name': 'SUB1', 'x': 300, 'y': 200, 'properties': {'File': 'sub.sch'}}},
  {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'SUB1.1'}}, {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}},
  {'tool': 'connect', 'arguments': {'from': 'SUB1.2', 'to': 'ground'}},
  {'tool': 'add_analysis', 'arguments': {'kind': 'op'}}, {'tool': 'save_document', 'arguments': {'as': 'top.sch'}}]})
net = s.call('get_netlist', {}, ok=False); net = net if isinstance(net, str) else json.dumps(net)
print([l for l in net.splitlines() if 'SUBCKT' in l.upper() or l[:2] in ('R1', 'R2')])
s.call('simulate', {}, ok=False)
op = s.call('get_dataset', {'operating_point': True}, ok=False)
print('i(R1), i(R2):', [(d.get('inside'), d['values'].get('i')) for d in op.get('operating point', {}).get('devices', []) if d.get('inside')], '(2 V over 2k: 1 mA each)')
s.close()
