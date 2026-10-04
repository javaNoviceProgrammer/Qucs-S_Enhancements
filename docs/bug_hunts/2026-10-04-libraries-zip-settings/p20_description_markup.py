"""p20: create_library with descriptions holding the library's own markup: </Description>, </Component>, <Component X>."""
import mcp, json, os, libsetup
s = mcp.Server('p20')
s.call('new_project', {'name': 'src'})
src = s.ws + '/src_prj'
open(src + '/vres.va', 'w').write(libsetup.VA)
open(src + '/vres.sch', 'w').write(libsetup.SUB)
s.call('open_project', {'name': 'src'})
evil = 'A resistor\n  </Description>\n</Component>\n\n<Component Fake>\n  <Description>\ninjected\n  </Description>\n  <Model>\n<R R9 1 0 0 0 0 0 0 "1k" 1>\n  </Model>'
r = s.call('create_library', {'name': 'DescLib', 'subcircuits': ['vres'], 'descriptions': {'vres': evil}})
print('create:', s.last_error, json.dumps(r.get('parts') if isinstance(r, dict) else r)[:400])
l = s.call('list_libraries', {'library': 'DescLib'})
print('parts listed:', [p.get('part') for p in l.get('parts', [])] if isinstance(l, dict) else l)
lib = s.ws + '/user_lib/DescLib.lib'
txt = open(lib).read() if os.path.isfile(lib) else ''
print('components in the file:', [line for line in txt.split('\n') if line.startswith('<Component')])
r = s.call('create_library', {'name': 'DescLib2', 'subcircuits': ['vres'], 'descriptions': {'vres': 'line one\n<Model>\nzzz'}})
d = s.call('describe_part', {'library': 'DescLib2', 'part': 'vres'})
print('describe DescLib2 vres:', s.last_error, str(d)[:300])
s.close()
