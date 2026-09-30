from mcp import Server, HERE
import json, shutil
WS = HERE + '/wsh14'; shutil.rmtree(WS, ignore_errors=True)
s = Server(WS)
for val in ['+15 V', '15V', ' 15 ', '1.5e1', '-15 V', '15 kV', '+1.5e+1']:
    s.call('new_document', {})
    s.call('batch', {'atomic': True, 'calls': [
     {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V3', 'x': 100, 'y': 200, 'properties': {'U': val}}},
     {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R9', 'x': 250, 'y': 200, 'rotation': 1}},
     {'tool': 'connect', 'arguments': {'from': 'V3.1', 'to': 'ground'}}, {'tool': 'connect', 'arguments': {'from': 'V3.2', 'to': 'R9.2'}},
     {'tool': 'connect', 'arguments': {'from': 'R9.1', 'to': 'ground'}}, {'tool': 'add_analysis', 'arguments': {'kind': 'op'}}]}, ok=False)
    c = s.call('check_schematic', {}, ok=False)
    msgs = [n['message'] for n in (c.get('notes', []) + c.get('warnings', []))] if isinstance(c, dict) else [c]
    print(repr(val), '->', [m[m.find('if it was'):][:90] if 'if it was' in m else m[:120] for m in msgs])
    s.call('close_document', {'unsaved': 'discard'}, ok=False)
s.close()
