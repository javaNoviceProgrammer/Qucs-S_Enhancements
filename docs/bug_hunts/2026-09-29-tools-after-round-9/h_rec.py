from mcp import Server, HERE
import json, shutil, time
WS = HERE + '/wsh10'; shutil.rmtree(WS, ignore_errors=True)
import os; os.makedirs(WS, exist_ok=True)
s = Server(WS)
# self.sch uses itself as a subcircuit
s.call('new_document', {})
s.call('batch', {'atomic': True, 'calls': [
 {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'a', 'x': 100, 'y': 100}},
 {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 250, 'y': 100}},
 {'tool': 'connect', 'arguments': {'from': 'a.1', 'to': 'R1.1'}}, {'tool': 'connect', 'arguments': {'from': 'R1.2', 'to': 'ground'}},
 {'tool': 'add_component', 'arguments': {'type': 'Sub', 'name': 'X1', 'x': 400, 'y': 300, 'properties': {'File': 'self.sch'}}},
 {'tool': 'save_document', 'arguments': {'as': WS + '/self.sch'}}]})
t0 = time.time(); c = s.call('check_schematic', {'path': 'self.sch', 'subcircuits': True}, ok=False); print('self', round(time.time()-t0, 2), json.dumps(c)[:300])
t0 = time.time(); c = s.call('get_netlist', {'path': 'self.sch'}, ok=False); print('netlist self', round(time.time()-t0, 2), str(c)[:200].replace('\n', '|'))
# empty File and missing file
s.call('new_document', {})
s.call('batch', {'atomic': True, 'calls': [
 {'tool': 'add_component', 'arguments': {'type': 'Sub', 'name': 'X2', 'x': 400, 'y': 300, 'properties': {'File': ''}}},
 {'tool': 'add_component', 'arguments': {'type': 'Sub', 'name': 'X3', 'x': 600, 'y': 300, 'properties': {'File': 'nosuch.sch'}}},
 {'tool': 'add_analysis', 'arguments': {'kind': 'op'}},
 {'tool': 'save_document', 'arguments': {'as': WS + '/e.sch'}}]})
c = s.call('check_schematic', {'path': 'e.sch', 'subcircuits': True}, ok=False); print('empty/missing', json.dumps(c)[:600])
s.close()
