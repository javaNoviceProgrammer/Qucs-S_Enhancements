# A subcircuit's port named gnd (or 0) on an unlabelled net: is the pin then ground inside, whatever the parent wires to it?
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
for pname in ('gnd', 'GND', '0'):
    ws = HERE + '/ws_pgnd'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
    s = Server(ws)
    s.call('new_document', {})
    r = s.call('batch', {'calls': [
      {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P1', 'x': 100, 'y': 100}},
      {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P2', 'x': 100, 'y': 300}},
      {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 250, 'y': 200}},
      {'tool': 'connect', 'arguments': {'from': 'P1.1', 'to': 'R1.1'}}, {'tool': 'connect', 'arguments': {'from': 'P2.1', 'to': 'R1.2'}},
      {'tool': 'edit_component', 'arguments': {'name': 'P2', 'rename': pname}},
      {'tool': 'save_document', 'arguments': {'as': 'sub.sch'}}]}, ok=False)
    if isinstance(r, str) and 'rror' in r[:200]: print(repr(pname), 'build:', r[:200])
    s.call('new_document', {})
    s.call('batch', {'calls': [
      {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '5'}}},
      {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V2', 'x': 500, 'y': 200, 'properties': {'U': '2'}}},
      {'tool': 'add_component', 'arguments': {'type': 'Sub', 'name': 'SUB1', 'x': 300, 'y': 200, 'properties': {'File': 'sub.sch'}}},
      {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'SUB1.1'}}, {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}},
      {'tool': 'connect', 'arguments': {'from': 'SUB1.2', 'to': 'V2.1'}}, {'tool': 'connect', 'arguments': {'from': 'V2.2', 'to': 'ground'}},
      {'tool': 'add_analysis', 'arguments': {'kind': 'op'}}, {'tool': 'save_document', 'arguments': {'as': 'top.sch'}}]}, ok=False)
    net = s.call('get_netlist', {}, ok=False)
    net = net if isinstance(net, str) else json.dumps(net)
    s.call('simulate', {}, ok=False)
    op = s.call('get_dataset', {'operating_point': True}, ok=False)
    i = [d['values'].get('i') for d in (op.get('operating point', {}).get('devices', []) if isinstance(op, dict) else []) if d.get('inside') == 'r1']
    print(repr(pname), '|', [l for l in net.splitlines() if 'SUBCKT' in l.upper() or l.startswith('R1')], '| i(R1):', i, '(5 V - 2 V over 1k: 3 mA)')
    chk = s.call('check_schematic', {'path': 'sub.sch', 'subcircuits': True}, ok=False) if False else None
    s.close()
