"""A search path holding a folder named mylib.lib, a link loop, and an empty mylib.lib; then the real one later."""
import sys, os, json; sys.path.insert(0, __file__.rsplit('/', 1)[0]); from mcp import Server
s = Server('p37')
a, b, c = s.root + '/A', s.root + '/B', s.root + '/C'
for d in (a, b, c): os.makedirs(d, exist_ok=True)
os.makedirs(a + '/mylib.lib')                      # a folder of the name
os.symlink(b + '/loop.lib', b + '/loop.lib')       # a link to itself
os.symlink(b + '/mylib.lib', b + '/mylib.lib')     # a link loop of the name
open(c + '/mylib.lib', 'w').write('<Qucs Library 26.1.5 "mylib">\n\n<Component opamp>\n  <Description>\nx\n  </Description>\n  <Model>\n.Def:mylib_opamp _net1 _net2\nR:R1 _net1 _net2 R="1k"\n.Def:End\n  </Model>\n  <Symbol>\n  <.PortSym 0 0 1 0>\n  <.PortSym 40 0 2 0>\n  <Line 0 0 40 0 #000080 2 1>\n  </Symbol>\n</Component>\n')
print('paths:', json.dumps(s.call('set_settings', {'scope': 'app', 'values': {'Locations/Library search paths': [a, b, c]}}))[:300])
r = s.call('list_libraries', {}); print('list:', [(x['folder'][-3:], [(l.get('name'), l.get('parts'), l.get('kind')) for l in x.get('libraries', [])], {k: v for k, v in x.items() if k not in ('folder', 'libraries')}) for x in r['sections'] if s.root in x['folder']])
print('find:', json.dumps(s.call('find_library_component', {'search': 'opamp'}))[:400])
print('describe:', json.dumps(s.call('describe_part', {'library': 'mylib', 'part': 'opamp'}))[:300], s.last_error)
s.call('new_document', {'kind': 'schematic'})
r = s.call('add_component', {'type': 'Lib', 'x': 100, 'y': 100, 'properties': {'Lib': 'mylib', 'Comp': 'opamp'}}); print('place:', str(r)[:300])
print('check:', json.dumps(s.call('check_schematic', {}))[:400])
s.close()
