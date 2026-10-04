"""p12: two search paths, each a mylib with a part opamp (different descriptions): what find_library_component,
list_libraries and describe_part give for the second, and where its 'place' really leads."""
import os, json, mcp
s = mcp.Server('p12')
src = open(os.environ['MYLIB']).read()
for d, desc in (('A', 'FIRST opamp of A'), ('B', 'SECOND opamp of B')):
    os.makedirs(f'{s.root}/{d}', exist_ok=True)
    open(f'{s.root}/{d}/mylib.lib', 'w').write(src.replace('Simple behavioural op-amp (Verilog-A)', desc))
s.call('set_settings', {'scope': 'app', 'values': {'Locations/Library search paths': [s.root + '/A', s.root + '/B']}})
f = s.call('find_library_component', {'search': 'opamp', 'library': 'mylib', 'limit': 10})
print('find:', json.dumps([(x.get('library'), x.get('description'), x.get('place')) for x in f.get('found', [])])[:700] if isinstance(f, dict) else f)
l = s.call('list_libraries', {'library': s.root + '/B/mylib.lib'})
print('list B:', json.dumps(l.get('parts', l) if isinstance(l, dict) else l)[:400])
place = l['parts'][0]['place'] if isinstance(l, dict) and l.get('parts') else None
s.call('new_document', {})
if place:
    s.call('add_component', {'type': place['type'], 'name': 'U1', 'x': 200, 'y': 200, 'properties': place['properties']})
    d = s.call('get_schematic', {'components': ['U1']})
    print('placed from B, Lib =', place['properties'].get('Lib'))
dp = s.call('describe_part', {'library': 'mylib', 'part': 'opamp'})
print('describe_part:', json.dumps(dp)[:300])
s.call('save_document', {'as': s.ws + '/t.sch'})
c = s.call('check_schematic', {'path': 't.sch'})
print('check:', [w['message'] for w in c.get('warnings', []) if 'library' in w['message']])
s.close()
