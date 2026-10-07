"""p3: create_library replace that fails: a subcircuit places a part of the library being replaced. What is left, and what the answer says."""
import os, json, mcp, lib
s = mcp.Server('p3')
d = lib.project(s, 'rep', {'div.sch': lib.divider('1k', '3k')})
r = s.call('create_library', {'name': 'Keep'})
print('first:', s.last_error, [p['part'] for p in r.get('parts', [])])
place = r['parts'][0]['place']
# A subcircuit of the project that uses the library's own part (a test bench made a block).
s.call('new_document', {})
s.call('add_component', {'type': 'Port', 'name': 'P1', 'x': 100, 'y': 100})
s.call('add_component', {'type': 'Port', 'name': 'P2', 'x': 600, 'y': 100})
s.call('add_component', {'type': place['type'], 'name': 'X1', 'x': 320, 'y': 120, 'properties': place['properties']})
s.call('connect', {'from': 'P1.1', 'to': 'X1.1'})
s.call('connect', {'from': 'X1.2', 'to': 'P2.1'})
s.call('save_document', {'as': d + '/wrap.sch'})
s.call('close_document', {})
before = sorted(os.listdir(s.ws + '/user_lib'))
r = s.call('create_library', {'name': 'Keep', 'replace': True})
print('replace: isError', s.last_error)
print(json.dumps(r, indent=1)[:2500] if not isinstance(r, str) else r[:2500])
print('user_lib before', before, 'after', sorted(os.listdir(s.ws + '/user_lib')))
print('trash:', sorted(os.listdir(s.root + '/trash')) if os.path.isdir(s.root + '/trash') else None)
for root, dirs, files in os.walk(s.root + '/trash'):
    for f in files: print('   ', os.path.relpath(os.path.join(root, f), s.root + '/trash'))
l = s.call('list_libraries', {})
print('user section:', [sec.get('libraries') for sec in l['sections'] if sec['section'] == 'user'])
s.close()
