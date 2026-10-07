"""p6b: the netlist and ngspice's error for a library part whose subcircuit places another subcircuit; and the same part twice."""
import os, re, json, mcp, lib
s = mcp.Server('p6b')
d = lib.project(s, 'hp', {'inner.sch': lib.divider('1k', '1k')})
s.call('new_document', {})
s.call('add_component', {'type': 'Port', 'name': 'P1', 'x': 100, 'y': 100})
s.call('add_component', {'type': 'Port', 'name': 'P2', 'x': 600, 'y': 100})
s.call('add_component', {'type': 'Sub', 'name': 'SUB1', 'x': 320, 'y': 120, 'properties': {'File': 'inner.sch'}})
s.call('connect', {'from': 'P1.1', 'to': 'SUB1.1'})
s.call('connect', {'from': 'SUB1.2', 'to': 'P2.1'})
s.call('save_document', {'as': d + '/outer.sch'})
s.call('close_document', {})
r = s.call('create_library', {'name': 'Hier', 'subcircuits': ['outer']})
place = r['parts'][0]['place']
out = lib.bench(s, place, d + '/use_outer.sch')
print('errors:', json.dumps(json.loads(out['sim']).get('errors') if 'sim' in out else out)[:800])
print(s.call('get_netlist', {'numbered': True}) if True else '')
s.close()
