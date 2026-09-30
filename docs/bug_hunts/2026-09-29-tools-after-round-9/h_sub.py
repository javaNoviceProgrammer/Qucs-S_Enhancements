from mcp import Server, HERE
import json, shutil
WS = HERE + '/wsh9'; shutil.rmtree(WS, ignore_errors=True)
s = Server(WS)
# a subcircuit: a regulator block making -5 V on its port VNEG and +5 V on VPOS
s.call('new_document', {})
s.call('batch', {'atomic': True, 'calls': [
 {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'VEE', 'x': 400, 'y': 100, 'properties': {'Num': '1'}}},
 {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'VCC', 'x': 400, 'y': 300, 'properties': {'Num': '2'}}},
 {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 200, 'y': 150, 'properties': {'U': '5 V'}}},
 {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V2', 'x': 200, 'y': 350, 'properties': {'U': '5 V'}}},
 {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'ground'}}, {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'VEE.1'}},
 {'tool': 'connect', 'arguments': {'from': 'V2.2', 'to': 'ground'}}, {'tool': 'connect', 'arguments': {'from': 'V2.1', 'to': 'VCC.1'}},
 {'tool': 'save_document', 'arguments': {'as': WS + '/reg.sch'}}]})
c = s.call('check_schematic', {'path': 'reg.sch'})
print('sub check:', c['found'], [n['message'][:130] for n in c['notes'] + c['warnings']])
s.call('set_label', {'path': 'reg.sch', 'at': 'VEE.1', 'name': 'vee'})
c = s.call('check_schematic', {'path': 'reg.sch'})
print('with label vee:', c['found'], [n['message'][:130] for n in c['notes'] + c['warnings']])
s.close()
