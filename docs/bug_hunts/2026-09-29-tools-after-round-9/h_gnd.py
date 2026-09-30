from mcp import Server, HERE
import json, shutil
WS = HERE + '/wsh2'; shutil.rmtree(WS, ignore_errors=True)
s = Server(WS)
s.call('new_document', {})
for name, num, y in [('IN', '1', 100), ('GND', '2', 200)]:
    s.call('add_component', {'type': 'Port', 'name': name, 'x': 100, 'y': y, 'properties': {'Num': num}})
s.call('add_component', {'type': 'R', 'name': 'R1', 'x': 250, 'y': 150, 'rotation': 1})
s.call('connect', {'from': 'IN.1', 'to': 'R1.2'}); s.call('connect', {'from': 'GND.1', 'to': 'R1.1'})
s.call('save_document', {'as': 'blk'}); print(s.call('make_symbol', {})['pins']); s.call('save_document', {})
s.call('new_document', {})
s.call('batch', {'atomic': True, 'calls': [
  {'tool': 'add_component', 'arguments': {'type': 'Sub', 'name': 'X1', 'x': 400, 'y': 300, 'properties': {'File': 'blk.sch'}}},
  {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 150, 'y': 300, 'properties': {'U': '5 V'}}},
  {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'VM', 'x': 150, 'y': 500, 'properties': {'U': '2.5 V'}}},
  {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'X1.1'}}, {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}},
  {'tool': 'connect', 'arguments': {'from': 'VM.1', 'to': 'X1.2'}}, {'tool': 'connect', 'arguments': {'from': 'VM.2', 'to': 'ground'}},
  {'tool': 'add_analysis', 'arguments': {'kind': 'op'}},
  {'tool': 'save_document', 'arguments': {'as': WS + '/top.sch'}}]})
print('pins', [(p.port.Name if False else None) for p in []])
g = s.call('get_schematic', {'path': 'top.sch'})
print('X1 pins', [(p.get('name'), p.get('net')) for c in g['components'] if c['name'] == 'X1' for p in c['pins']])
before = s.call('get_netlist', {'path': 'top.sch', 'map': True})
r = s.call('create_subcircuit', {'path': 'top.sch', 'names': ['X1'], 'save_as': 'wrap.sch'}, ok=False)
print(json.dumps(r)[:900])
after = s.call('get_netlist', {'path': 'top.sch', 'map': True}, ok=False)
print('before nodes', before['nodes'])
print('after nodes', after.get('nodes') if isinstance(after, dict) else after)
print('\n'.join(l for l in after['netlist'] if l[:1] in 'VX') if isinstance(after, dict) else '')
s.close()
