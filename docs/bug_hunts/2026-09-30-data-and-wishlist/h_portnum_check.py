# F1 again: Check Schematic of the parent, with its subcircuits, when two ports share a number.
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
ws = HERE + '/ws_pnum2'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
s = Server(ws)
s.call('new_document', {})
s.call('batch', {'calls': [
  {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P1', 'x': 100, 'y': 100}},
  {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P2', 'x': 100, 'y': 300}},
  {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P3', 'x': 400, 'y': 300}},
  {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 250, 'y': 200}},
  {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R2', 'x': 450, 'y': 200, 'properties': {'R': '2k'}}},
  {'tool': 'connect', 'arguments': {'from': 'P1.1', 'to': 'R1.1'}}, {'tool': 'connect', 'arguments': {'from': 'P2.1', 'to': 'R1.2'}},
  {'tool': 'connect', 'arguments': {'from': 'P3.1', 'to': 'R2.2'}}, {'tool': 'connect', 'arguments': {'from': 'R2.1', 'to': 'R1.2'}}]})
r = s.call('edit_component', {'name': 'P2', 'properties': {'Num': '1'}}, ok=False)
print('P2 Num=1:', (r if isinstance(r, str) else json.dumps(r))[:160])
s.call('save_document', {'as': 'sub.sch'})
print('check:', [i['message'][:120] for k in ('errors', 'warnings', 'notes') for i in s.call('check_schematic', {}, ok=False).get(k, [])])
s.call('new_document', {})
p = s.call('add_component', {'type': 'Sub', 'name': 'SUB1', 'x': 300, 'y': 200, 'properties': {'File': 'sub.sch'}}, ok=False)
print('instance pins:', [(q.get('pin'), q.get('name')) for q in p.get('pins', [])] if isinstance(p, dict) else p[:200])
s.call('save_document', {'as': 'top.sch'})
r = s.call('check_schematic', {'subcircuits': True}, ok=False)
print('check with subcircuits:', json.dumps(r)[:300])
s.close()
